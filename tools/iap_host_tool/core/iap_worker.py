"""
IAP 工作线程：串口通信 + 固件发送
"""

import time
import struct
from PyQt6.QtCore import QThread, pyqtSignal

import serial

from .protocol import (
    PACKET_SIZE,
    CMD_QUERY_OFFSET,
    CMD_RESET_UPGRADE,
    RSP_ACK,
    RSP_NAK,
    RSP_OFFSET,
    build_cmd_frame,
    build_data_frame,
    build_end_frame,
    precalc_firmware_crc,
)
from .firmware import parse_firmware
from config import get_profile

_PROFILE = get_profile()


class IAPWorker(QThread):
    """串口 IAP 工作线程，所有信号从工作线程发射到 UI 线程"""

    log = pyqtSignal(str)
    progress = pyqtSignal(int, int)  # current, total
    pages = pyqtSignal(int)         # current page count
    status = pyqtSignal(str)        # one-line status
    finished = pyqtSignal(bool, str)  # success, message
    packet_sent = pyqtSignal(str)   # TX 帧 hex（含 [TX-DATA] / [TX-CMD] 前缀）
    packet_received = pyqtSignal(str)  # RX 帧 hex（含 [RX] 前缀）

    def __init__(self, parent=None):
        super().__init__(parent)
        self._port = ""
        self._baud = 115200
        self._filepath = ""
        self._mode = "auto"  # "auto" | "reset" | "resume"
        self._ser = None
        self._should_stop = False
        self._resume_pages = -1  # 续传起始页（-1 = 由 auto_connect 自动检测）
        self._limit_pct = 100  # 发送百分比限制（用于续传调试，100=全部发送）
        self._is_limited = False  # 当前是否处于限制模式
        self._corrupt_crc = False  # 模拟 CRC 错误（测试重传用）

    # ---- 配置接口 ----

    def set_port(self, port, baud=115200):
        self._port = port
        self._baud = baud

    def set_firmware(self, filepath):
        self._filepath = filepath

    def set_mode(self, mode):
        """'auto' = 发 U 触发 + 自动续传;  'reset' = 先重置再从头;  'resume' = 用预设偏移续传"""
        self._mode = mode

    def set_resume_from(self, pages: int):
        """设置续传起始页（配合 mode='resume' 使用）"""
        self._resume_pages = pages

    def set_limit_percent(self, pct: int):
        """限制只发送固件的前 pct%（用于续传调试）"""
        self._limit_pct = max(10, min(100, pct))

    def set_corrupt_crc(self, enabled: bool):
        """设置是否模拟 CRC 错误（测试重传用）"""
        self._corrupt_crc = enabled

    def stop(self):
        """请求停止工作线程"""
        self._should_stop = True
        if self._ser and self._ser.is_open:
            try:
                self._ser.close()
            except Exception:
                pass

    # ---- 线程主循环 ----

    def run(self):
        self._should_stop = False
        try:
            self._run_impl()
        except serial.SerialException as e:
            self.finished.emit(False, f"串口错误: {e}")
        except Exception as e:
            self.finished.emit(False, f"错误: {e}")

    def _run_impl(self):
        # 打开串口
        self.log.emit(f"打开串口 {self._port} @ {self._baud}")
        self._ser = serial.Serial(port=self._port, baudrate=self._baud, timeout=3)
        self._ser.reset_input_buffer()
        self._ser.reset_output_buffer()
        time.sleep(0.3)

        try:
            # "reset_only" 只发重置命令，不发固件
            if self._mode == "reset_only":
                self._do_reset()
                self.finished.emit(True, "重置完成")
                return

            # 其他模式：解析固件并发送
            self.log.emit(f"解析固件: {self._filepath}")
            firmware_data, actual_size = parse_firmware(self._filepath)
            full_size = actual_size
            self.log.emit(f"固件大小: {actual_size} 字节 ({actual_size/1024:.1f} KB)")

            # 预计算 CRC
            expected_crc = precalc_firmware_crc(firmware_data, actual_size)
            self.log.emit(f"预期 CRC: 0x{expected_crc:08X}")

            # 确定模式并发送
            if self._mode == "reset":
                self._do_reset()
                offset = 0
            elif self._mode == "resume" and self._resume_pages >= 0:
                offset = self._resume_pages * 1024
                self.log.emit(f"续传: 从第 {self._resume_pages} 页 ({offset} 字节) 继续")
            else:
                offset = self._auto_connect()

            # 发送固件
            if offset == 0:
                self.status.emit("开始发送固件...")
            else:
                self.status.emit(f"续传: 从 {offset} 字节开始...")

            send_size = full_size - offset
            send_data = firmware_data[offset:offset + send_size]

            # 应用百分比限制（续传调试用）
            self._is_limited = False
            if self._limit_pct < 100:
                limit_bytes = int(full_size * self._limit_pct / 100)
                # 向下对齐到 1024 页边界，保证 MCU 已保存偏移记录
                limit_bytes = (limit_bytes // 1024) * 1024
                if limit_bytes <= offset:
                    raise RuntimeError(
                        f"限制 {self._limit_pct}% ({limit_bytes} 字节) "
                        f"小于已发送偏移 ({offset})")
                send_size = limit_bytes - offset
                send_data = firmware_data[offset:offset + send_size]
                # 重算实际发送部分的 CRC
                expected_crc = precalc_firmware_crc(firmware_data, limit_bytes)
                self._is_limited = True
                self.log.emit(
                    f"限制发送: {self._limit_pct}% → "
                    f"{limit_bytes} 字节 (已对齐到 1024 页边界, "
                    f"CRC: 0x{expected_crc:08X})")

            # 截断到实际大小，避免最后一帧被填充 padding 撑到 128 字节
            self._send_firmware(send_data, send_size, expected_crc,
                                full_size, offset)

        finally:
            if self._ser and self._ser.is_open:
                self._ser.close()

    # ---- 内部方法 ----

    def _check_stop(self):
        if self._should_stop:
            raise InterruptedError("用户停止")

    def _send_raw(self, data):
        self._check_stop()
        crc_corrupted = False
        if self._ser and self._ser.is_open:
            # 模拟 CRC 错误：翻转数据帧（TYPE=0x01, LEN>0）的 CRC 首字节
            if self._corrupt_crc and len(data) >= 6 and data[1] == 0x01 and data[3] > 0:
                crc_pos = 4 + data[3]  # SOF+TYPE+SEQ+LEN+DATA → CRC 起始
                if crc_pos + 4 < len(data):
                    data = bytearray(data)
                    data[crc_pos] ^= 0xFF  # 翻转 CRC 首字节
                    data = bytes(data)
                    crc_corrupted = True

            self._ser.write(data)
            self._ser.flush()
            # 识别帧类型并格式化
            prefix = "TX"
            if len(data) >= 2:
                if data[1] == 0x01:
                    prefix = "TX-DATA"
                elif data[1] == 0x02 and len(data) > 2:
                    cmd_name = {
                        0x12: "QUERY_OFFSET",
                        0x13: "RESET_UPGRADE",
                        0x14: "QUERY_STATE",
                    }.get(data[2], f"0x{data[2]:02X}")
                    prefix = f"TX-CMD({cmd_name})"
                elif all(b == 0x21 for b in data):
                    prefix = "TX-TRIGGER(!)"
            if crc_corrupted:
                prefix += "(CRC-ERR)"
            hex_str = f"[{prefix}]  {bytes(data).hex(' ').upper()}"
            self.packet_sent.emit(hex_str)

    def _emit_rx_packet(self, type_byte, cmd, param_len, params_bytes, crc_val):
        """根据解析结果重构 RX 帧 hex 并发射 packet_received 信号"""
        try:
            frame = bytearray()
            frame.extend([0xAA, type_byte, cmd, param_len])
            frame.extend(params_bytes)
            from .protocol import crc32_calc as _c
            calc = _c(bytes([type_byte, cmd, param_len]) + params_bytes)
            frame.extend(struct.pack('<I', calc))
            frame.append(0x55)
            # 识别应答命令名
            rsp_name = {
                0x20: "ACK",
                0x21: "NAK",
                0x22: "OFFSET",
                0x23: "STATE",
            }.get(cmd, f"0x{cmd:02X}")
            hex_str = f"[RX-{rsp_name}]  {bytes(frame).hex(' ').upper()}"
            self.packet_received.emit(hex_str)
        except Exception:
            pass  # 日志不是关键路径，忽略异常

    def _read_byte(self):
        """读取一个字节，超时返回 None。"""
        self._check_stop()
        if not self._ser or not self._ser.is_open:
            return None
        b = self._ser.read(1)
        return b[0] if b else None

    def _recv_frame(self):
        """
        接收应答帧。
        返回 (cmd, params) 或 (None, None)。

        帧: AA 02 CMD LEN [PARAM...] CRC32(4B,LE) 55
        按 LEN 长度读取，不会与数据中的 0x55 冲突。
        """
        # 找 SOF
        while True:
            b = self._read_byte()
            if b is None:
                return None, None
            if b == 0xAA:
                break

        # TYPE
        b = self._read_byte()
        type_byte = b
        if b is None or b != 0x02:
            return None, None

        # CMD
        cmd = self._read_byte()
        if cmd is None:
            return None, None

        # PARAM_LEN
        param_len = self._read_byte()
        if param_len is None:
            return None, None

        # 读 PARAM（精确长度）
        params = bytearray()
        for _ in range(param_len):
            b = self._read_byte()
            if b is None:
                return None, None
            params.append(b)

        # 读 CRC（4 字节）
        crc_bytes = bytearray()
        for _ in range(4):
            b = self._read_byte()
            if b is None:
                return None, None
            crc_bytes.append(b)

        crc_recv = struct.unpack('<I', bytes(crc_bytes))[0]

        # EOF
        b = self._read_byte()
        if b is None or b != 0x55:
            return None, None

        # 校验 CRC
        from .protocol import crc32_calc

        crc_calc = crc32_calc(bytes([type_byte, cmd, param_len]) + bytes(params))
        if crc_calc != crc_recv:
            return None, None

        # 记录 RX 包
        if None not in (type_byte, cmd, param_len):
            self._emit_rx_packet(type_byte, cmd, param_len, bytes(params), crc_recv)
        return cmd, bytes(params)

    def _wait_ack_v3(self, timeout=5):
        """
        等 ACK 帧。
        返回 (status, seq, pages) 或 None 超时。
        status: 0=CRC_OK, 1=CRC_ERR
        seq:    ACK 对应的帧序号
        pages:  已写入的 1024 页数
        """
        from .protocol import RSP_ACK

        deadline = time.time() + timeout
        while time.time() < deadline:
            cmd, params = self._recv_frame()
            if cmd == RSP_ACK and len(params) >= 3:
                status = params[0]  # 0=OK, 1=CRC_ERR
                seq = params[1]     # 帧序号
                pages = params[2]   # 已写页数
                return status, seq, pages
        return None

    def _query_offset(self):
        """查已写入页数"""
        from .protocol import CMD_QUERY_OFFSET, RSP_OFFSET

        self._send_raw(build_cmd_frame(CMD_QUERY_OFFSET))
        deadline = time.time() + 3
        while time.time() < deadline:
            cmd, params = self._recv_frame()
            if cmd == RSP_OFFSET and len(params) >= 2:
                return struct.unpack('<H', params)[0]
        return None

    def _wait_device_ready(self, max_wait=20):
        """轮询等设备进入 RX 循环"""
        # 调短串口超时做快速轮询
        old_to = self._ser.timeout
        self._ser.timeout = 0.5
        deadline = time.time() + max_wait
        while time.time() < deadline:
            pages = self._query_offset()
            if pages is not None:
                self._ser.timeout = old_to
                return pages
            time.sleep(0.1)
        self._ser.timeout = old_to
        return None

    def _auto_connect(self):
        """
        自动连接设备：
          1. 先尝试查询（设备已在 bootloader）
          2. 失败则发 'U' 触发 APP → bootloader
          3. 轮询等就绪
          4. 返回已写入字节偏移（续传）或 0（从头）
        """
        # 先快速试一次
        pages = self._query_offset()

        if pages is None:
            # 触发模式按 profile 区分（v1.1 决策, config.py）
            #   AT32 (bang5) : 发 5 连 '!' — APP ota_app_hook 需 5 连计数触发
            #                  停机链(写READY+复位) → Boot 读 READY 直接擦除进升级
            #   STM32(single): 发单 '!' ×10 — 源工程 APP 侧 UART 中断扫单 '!'
            if _PROFILE["trigger"] == "bang5":
                self.log.emit("发送 '!!!!!' 触发 AT32 升级链...")
                for _ in range(3):
                    self._send_raw(b'!!!!!')
                    time.sleep(0.8)  # 首次触发 APP 停机链, 后续给 Boot 窗口/擦除留时间
            else:
                self.log.emit("发送 '!' 触发 APP 进入升级模式...")
                for _ in range(10):
                    self._send_raw(b'!')
                    time.sleep(0.4)  # 400ms 间隔，与 APP 主循环同步

            self.log.emit("等待设备进入升级模式...")
            pages = self._wait_device_ready(20)

        if pages is None:
            raise RuntimeError("无法进入升级模式，请检查接线或按 KEY 键")

        return pages * 1024 if pages > 0 else 0

    def _do_reset(self):
        """发 CMD_RESET_UPGRADE，从头开始"""
        self.log.emit("重置升级进度...")
        self._send_raw(build_cmd_frame(CMD_RESET_UPGRADE))
        ack = self._wait_ack_v3(timeout=10)
        if ack is None or ack[0] != 0:
            raise RuntimeError("重置升级失败")

    def _send_firmware(self, data, actual_size, expected_crc,
                       total_for_progress=None, offset_for_progress=0):
        """发送固件数据"""
        from .protocol import PACKET_SIZE

        if total_for_progress is None:
            total_for_progress = actual_size

        MAX_RETRY = 3
        total_frames = (actual_size + PACKET_SIZE - 1) // PACKET_SIZE
        sent_bytes = 0

        for frame_idx in range(total_frames):
            self._check_stop()

            chunk = data[sent_bytes:sent_bytes + PACKET_SIZE]
            is_last = (len(chunk) < PACKET_SIZE)
            seq = frame_idx & 0xFF

            # 发帧，支持重传
            sent = False
            for retry in range(MAX_RETRY):
                self._send_raw(build_data_frame(chunk, seq))
                ack = self._wait_ack_v3()
                if ack is None:
                    raise RuntimeError(
                        f"帧 {frame_idx+1}/{total_frames} ACK 超时")
                status, ack_seq, pages = ack
                if status == 0 and ack_seq == seq:
                    sent = True
                    break
                # CRC_ERR 或序号不匹配 → 重传
                if status == 1:
                    self.log.emit(
                        f"帧 {frame_idx+1} SEQ={seq} CRC 错误，重传第 {retry+1} 次")
                time.sleep(0.05)  # 重传前短暂等待
            if not sent:
                raise RuntimeError(
                    f"帧 {frame_idx+1}/{total_frames} 重传 {MAX_RETRY} 次仍失败")

            sent_bytes += len(chunk)
            progress_val = sent_bytes + offset_for_progress
            pct = progress_val * 100 // total_for_progress
            self.progress.emit(progress_val, total_for_progress)
            self.pages.emit(pages)
            self.status.emit(
                f"{pct}% ({progress_val}/{total_for_progress} 字节)")

            if is_last:
                break

        # 限制模式不发结束帧，让 MCU 保持 STATE_UPGRADING
        if not self._is_limited and actual_size % PACKET_SIZE == 0:
            self.log.emit("发送升级结束标记...")
            self._send_raw(build_end_frame())
            ack = self._wait_ack_v3()
            if ack is not None and ack[0] == 0:
                self.log.emit("结束标记已确认")

        self.log.emit(f"发送完成: {sent_bytes} 字节")
        self.log.emit(f"CRC: 0x{expected_crc:08X}")

        if self._is_limited:
            pct_done = sent_bytes * 100 // total_for_progress
            self.finished.emit(True,
                f"已发送 {pct_done}% ({sent_bytes} 字节)，"
                "MCU 仍处于升级等待状态\n"
                "断电后可测试「续传」")
        else:
            self.finished.emit(True, "升级完成，请复位设备跳转 APP")
