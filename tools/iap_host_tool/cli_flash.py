#!/usr/bin/env python3
"""
CLI 端到端 OTA 升级验证（无 GUI, 供 CI / AI 自动化调用）

用法（AT32 profile 示例; profile 由 config.py 的 APP_PROFILE 决定）:
    python cli_flash.py --port COM10 \\
        --firmware C:/Users/xiao1/Desktop/AT32/AT32F421G8U7_WorkBench/build/Debug/AT32F421G8U7_WorkBench.bin

流程: 触发(按 profile) → 轮询 offset → 逐帧发送(ACK/重传) → 结束帧 →
      查询 STATE=SUCCESS → 复位 → 串口监听 Boot+APP 心跳闭环
"""

import argparse
import struct
import sys
import time

import serial

# profile 切换: 改 config.py 的 APP_PROFILE（v1.1 决策, 默认 STM32）;
#   亦可用 --profile 命令行覆盖（驾驶舱 OTA 面板固定传 AT32）
import config as _config
from config import PROFILES, get_profile, profile_name
from core.firmware import parse_firmware
from core.protocol import (
    PACKET_SIZE,
    RSP_ACK,
    RSP_OFFSET,
    RSP_STATE,
    build_cmd_frame,
    build_data_frame,
    build_end_frame,
    precalc_firmware_crc,
)

# 状态码（与 ota/core 的 upgrade_state 一致）
STATE_RUNNING = 0xA5A5A5A0
STATE_READY = 0xA5A5A5A1
STATE_UPGRADING = 0xA5A5A5A2
STATE_CRC_FAIL = 0xA5A5A5A3
STATE_SUCCESS = 0xA5A5A5A4

MAX_RETRY = 3


class CliIAP:
    def __init__(self, port, baud, profile, reset_dir=None):
        self._port = port
        self._baud = baud
        self._profile = profile
        self._reset_dir = reset_dir
        self._ser = None

    # ---- 串口 ----
    def _read_byte(self):
        b = self._ser.read(1)
        return b[0] if b else None

    def _recv_frame(self):
        """接收应答帧，返回 (cmd, params) 或 (None, None)

        防御（驾驶舱集成实测踩坑）: APP 运行时串口是持续心跳数据流, 数据中
        假 0xAA 帧头会让扫描循环不断消费、外层 deadline 无法打断（卡死）。
        单次最多消费 MAX_FRAME_SCAN 字节, 超限视为无有效应答返回。"""
        MAX_FRAME_SCAN = 64
        scanned = 0
        while True:
            b = self._read_byte()
            scanned += 1
            if b is None or scanned > MAX_FRAME_SCAN:
                return None, None
            if b == 0xAA:
                break
        b = self._read_byte()
        scanned += 1
        if b is None or b != 0x02 or scanned > MAX_FRAME_SCAN:
            return None, None
        cmd = self._read_byte()
        scanned += 1
        if cmd is None or scanned > MAX_FRAME_SCAN:
            return None, None
        param_len = self._read_byte()
        scanned += 1
        if param_len is None or scanned > MAX_FRAME_SCAN:
            return None, None
        params = bytearray()
        for _ in range(param_len):
            b = self._read_byte()
            scanned += 1
            if b is None or scanned > MAX_FRAME_SCAN:
                return None, None
            params.append(b)
        crc_bytes = bytearray()
        for _ in range(4):
            b = self._read_byte()
            scanned += 1
            if b is None or scanned > MAX_FRAME_SCAN:
                return None, None
            crc_bytes.append(b)
        crc_recv = struct.unpack("<I", bytes(crc_bytes))[0]
        b = self._read_byte()
        scanned += 1
        if b is None or b != 0x55 or scanned > MAX_FRAME_SCAN:
            return None, None
        from core.protocol import crc32_calc
        crc_calc = crc32_calc(bytes([0x02, cmd, param_len]) + bytes(params))
        if crc_calc != crc_recv:
            return None, None
        return cmd, bytes(params)

    def _send_raw(self, data):
        self._ser.write(data)
        self._ser.flush()

    def _wait_ack(self, timeout=5):
        deadline = time.time() + timeout
        while time.time() < deadline:
            cmd, params = self._recv_frame()
            if cmd == RSP_ACK and len(params) >= 3:
                return params[0], params[1], params[2]  # status, seq, pages
        return None

    def _query_offset(self):
        from core.protocol import CMD_QUERY_OFFSET
        self._send_raw(build_cmd_frame(CMD_QUERY_OFFSET))
        deadline = time.time() + 3
        while time.time() < deadline:
            cmd, params = self._recv_frame()
            if cmd == RSP_OFFSET and len(params) >= 2:
                return struct.unpack("<H", params)[0]
        return None

    def _query_state(self):
        from core.protocol import CMD_QUERY_STATE
        self._send_raw(build_cmd_frame(CMD_QUERY_STATE))
        deadline = time.time() + 3
        while time.time() < deadline:
            cmd, params = self._recv_frame()
            if cmd == RSP_STATE and len(params) >= 4:
                return struct.unpack("<I", params)[0]
        return None

    def _wait_device_ready(self, max_wait=20):
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
        """触发（按 profile）+ 轮询等设备进升级模式"""
        pages = self._query_offset()
        if pages is None:
            if self._profile["trigger"] == "bang5":
                print("[trigger] 发 '!!!!!' x3 (AT32 停机链+Boot 窗口)", flush=True)
                for _ in range(3):
                    self._send_raw(b"!!!!!")
                    time.sleep(0.8)
            else:
                print("[trigger] 发 '!' x10 (STM32)", flush=True)
                for _ in range(10):
                    self._send_raw(b"!")
                    time.sleep(0.4)
            print("[wait] 等待设备进入升级模式...", flush=True)
            pages = self._wait_device_ready(20)
        if pages is None:
            raise RuntimeError("无法进入升级模式（接线 / 触发时序 / CDC 假死?）")
        return pages * 1024 if pages > 0 else 0

    # ---- 主流程 ----
    def run(self, firmware_path, listen_after=8, throttle=0.0, corrupt_frame=-1):
        print(f"[cfg] profile={profile_name()} label={self._profile['label']}",
              flush=True)
        print(f"[cfg] APP_START=0x{self._profile['app_start_address']:X} "
              f"APP_SIZE={self._profile['app_size']} "
              f"PACKET={PACKET_SIZE}", flush=True)
        print(f"[open] {self._port} @ {self._baud}", flush=True)
        self._ser = serial.Serial(port=self._port, baudrate=self._baud, timeout=3,
                                  write_timeout=3)  # write 超时: CDC 写方向挂死防卡
        self._ser.reset_input_buffer()
        self._ser.reset_output_buffer()
        time.sleep(0.3)

        try:
            fw, actual_size = parse_firmware(firmware_path)
            print(f"[fw] {actual_size} bytes ({actual_size/1024:.1f} KB)", flush=True)
            expected_crc = precalc_firmware_crc(fw, actual_size)
            print(f"[crc] expected 0x{expected_crc:08X}", flush=True)

            offset = self._auto_connect()
            print(f"[go] offset={offset} 开始发送固件", flush=True)

            total_frames = (actual_size - offset + PACKET_SIZE - 1) // PACKET_SIZE
            sent_bytes = 0
            for frame_idx in range(total_frames):
                start = offset + frame_idx * PACKET_SIZE
                # 截断到实际固件大小: parse_firmware 返回 44KB 填充版,
                # 若取满 128 字节, Boot 的"最后帧不满 128 自动结束"永不触发
                chunk = fw[start:min(start + PACKET_SIZE, actual_size)]
                is_last = (len(chunk) < PACKET_SIZE)
                seq = frame_idx & 0xFF
                sent = False
                for retry in range(MAX_RETRY):
                    frame = build_data_frame(chunk, seq)
                    if corrupt_frame == frame_idx and retry == 0:
                        # CRC 破坏测试: 改帧 CRC 最后一个字节(与正文不符 -> Boot NAK)
                        frame = frame[:-1] + bytes([frame[-1] ^ 0xFF])
                        print(f"[corrupt] 帧 {frame_idx+1} CRC 已破坏", flush=True)
                    self._send_raw(frame)
                    ack = self._wait_ack()
                    if ack is None:
                        raise RuntimeError(
                            f"帧 {frame_idx+1}/{total_frames} ACK 超时")
                    status, ack_seq, pages = ack
                    if status == 0 and ack_seq == seq:
                        sent = True
                        break
                    if status == 1:
                        print(f"[retry] 帧 {frame_idx+1} CRC_ERR 重传 {retry+1}", flush=True)
                    time.sleep(0.05)
                if not sent:
                    raise RuntimeError(
                        f"帧 {frame_idx+1}/{total_frames} 重传 {MAX_RETRY} 次失败")
                if throttle > 0:
                    time.sleep(throttle)
                sent_bytes += len(chunk)
                pct = sent_bytes * 100 // actual_size
                if frame_idx % 40 == 0 or is_last:
                    print(f"[tx] {pct}% ({sent_bytes}/{actual_size})", flush=True)
                if is_last:
                    break

            # 结束帧（最后帧不满 128 时 Boot 自动结束, 满 128 需显式结束帧）
            if actual_size % PACKET_SIZE == 0:
                print("[end] 发送升级结束标记...", flush=True)
                self._send_raw(build_end_frame())
                ack = self._wait_ack()
                print("[end]", "OK" if ack and ack[0] == 0 else "FAIL", flush=True)
                time.sleep(0.5)

            # Boot 置 SUCCESS 后 while(1) 死循环(防串口噪声冲写), QUERY_STATE 不再响应。
            # 判定升级完成: 监听串口找 "Upgrade complete. Final CRC" 打印(CRC 校验通过)。
            print("[wait] 监听升级完成打印...", flush=True)
            upgrade_ok = False
            listen_buf = bytearray()
            deadline = time.time() + 5
            while time.time() < deadline:
                n = self._ser.in_waiting
                if n:
                    listen_buf.extend(self._ser.read(n))
                if b"Upgrade complete" in bytes(listen_buf):
                    upgrade_ok = True
                    break
                time.sleep(0.05)
            print("[upgrade]",
                  "complete (CRC verified)" if upgrade_ok
                  else "NO 'Upgrade complete' printed!", flush=True)
            if upgrade_ok:
                for line in bytes(listen_buf).decode(errors="replace").splitlines()[-4:]:
                    if line.strip():
                        print("[boot]", line.strip(), flush=True)

            # 复位跳 APP:
            #   Boot 置 SUCCESS 后 while(1) 死循环(防串口噪声冲写), 命令帧不再响应,
            #   必须硬件复位。--reset-dir 提供 openocd 脚本目录则用 openocd reset,
            #   否则退化为 CMD_RESET_UPGRADE(仅对"升级未完成/仍在升级模式"有效)。
            if self._reset_dir:
                print(f"[reset] 硬件复位 (openocd reset run)...", flush=True)
                import os
                ocd = os.path.join(os.environ.get("LOCALAPPDATA", ""),
                                   "at32-tools", "OpenOCD", "V2.0.9", "bin", "openocd.exe")
                if not os.path.exists(ocd):
                    print(f"[warn] 找不到 openocd: {ocd}, 退化为串口复位命令", flush=True)
                else:
                    import subprocess
                    r = subprocess.run(
                        [ocd, "-s", self._reset_dir.replace("\\", "/"),
                         "-f", "openocd/interface/cmsis-dap.cfg",
                         "-f", "openocd/target/at32f421xx.cfg",
                         "-c", "init; reset run; shutdown"],
                        capture_output=True, text=True, timeout=60)
                    print(f"[reset] openocd exit={r.returncode}", flush=True)
            else:
                from core.protocol import CMD_RESET_UPGRADE
                self._send_raw(build_cmd_frame(CMD_RESET_UPGRADE))
            buf = bytearray()
            t0 = time.time()
            while time.time() - t0 < listen_after:
                n = self._ser.in_waiting
                if n:
                    buf.extend(self._ser.read(n))
                time.sleep(0.02)
            text = bytes(buf).decode(errors="replace")
            heartbeat = text.count("heartbeat")
            print(f"[post] 收到 {len(buf)}B, 心跳条数={heartbeat}", flush=True)
            tail = "\n".join(text.strip().splitlines()[-6:])
            print("[post tail]", tail, flush=True)
            return upgrade_ok and heartbeat >= 2

        finally:
            if self._ser and self._ser.is_open:
                self._ser.close()

    # ---- 仅查询模式（驾驶舱 OTA 面板"查询状态"） ----
    def run_query(self):
        """发 QUERY_STATE 打印 Boot 升级状态（不触发升级）"""
        print(f"[cfg] profile={profile_name()}", flush=True)
        print(f"[open] {self._port} @ {self._baud}", flush=True)
        self._ser = serial.Serial(port=self._port, baudrate=self._baud, timeout=3,
                                  write_timeout=3)  # write 超时: CDC 写方向挂死防卡
        print("[dbg] serial opened", flush=True)
        self._ser.reset_input_buffer()
        print("[dbg] buffer reset", flush=True)
        time.sleep(0.3)
        print("[dbg] call _query_state", flush=True)
        try:
            state = self._query_state()
            names = {STATE_RUNNING: "RUNNING", STATE_READY: "READY",
                     STATE_UPGRADING: "UPGRADING", STATE_CRC_FAIL: "CRC_FAIL",
                     STATE_SUCCESS: "SUCCESS"}
            if state is None:
                print("[state] 无应答（未进入升级模式 / Boot SUCCESS 死循环 / CDC 假死）",
                      flush=True)
                return False
            print(f"[state] 0x{state:08X} ({names.get(state, '?')})", flush=True)
            # 可升级/可续传状态视为"有效应答"
            return state in (STATE_READY, STATE_UPGRADING, STATE_CRC_FAIL)
        finally:
            if self._ser and self._ser.is_open:
                self._ser.close()

    # ---- 仅重置模式（驾驶舱 OTA 面板"重置续传"） ----
    def run_reset(self):
        """发 RESET_UPGRADE 命令帧：升级状态复擦除 → READY（断电续传起点清零）"""
        print(f"[cfg] profile={profile_name()}", flush=True)
        print(f"[open] {self._port} @ {self._baud}", flush=True)
        self._ser = serial.Serial(port=self._port, baudrate=self._baud, timeout=3,
                                  write_timeout=3)  # write 超时: CDC 写方向挂死防卡
        self._ser.reset_input_buffer()
        time.sleep(0.3)
        try:
            from core.protocol import CMD_RESET_UPGRADE
            self._send_raw(build_cmd_frame(CMD_RESET_UPGRADE))
            ack = self._wait_ack()
            if ack is None:
                print("[reset-upgrade] 无应答（Boot SUCCESS 死循环 / 未进入升级模式 / CDC 假死）",
                      flush=True)
                return False
            print(f"[reset-upgrade] ACK status={ack[0]} seq={ack[1]} pages={ack[2]}",
                  flush=True)
            return ack[0] == 0
        finally:
            if self._ser and self._ser.is_open:
                self._ser.close()

    # ---- 仅触发模式（驾驶舱 OTA 面板"进入升级"） ----
    def run_trigger(self):
        """只发送触发信号让 APP 停机进入 Boot 升级窗口，不发送固件"""
        print(f"[cfg] profile={profile_name()}", flush=True)
        print(f"[open] {self._port} @ {self._baud}", flush=True)
        self._ser = serial.Serial(port=self._port, baudrate=self._baud, timeout=3,
                                  write_timeout=3)
        self._ser.reset_input_buffer()
        time.sleep(0.3)
        try:
            if self._profile["trigger"] == "bang5":
                print("[trigger] 发 '!!!!!' x3 (AT32 停机链+Boot 窗口)", flush=True)
                for _ in range(3):
                    self._send_raw(b"!!!!!")
                    time.sleep(0.8)
            else:
                print("[trigger] 发 '!' x10 (STM32)", flush=True)
                for _ in range(10):
                    self._send_raw(b"!")
                    time.sleep(0.4)
            # 触发后轮询等 Boot 窗口就绪(QUERY_OFFSET 有应答)
            pages = self._wait_device_ready(20)
            if pages is None:
                print("[state] 触发后 20s 内未进入升级模式（停机链 / 时序 / CDC 假死?）",
                      flush=True)
                return False
            print(f"[go] 已进入升级模式, 已写 {pages} 页 ({pages*1024} 字节)", flush=True)
            return True
        finally:
            if self._ser and self._ser.is_open:
                self._ser.close()

    # ---- 仅查询写入进度模式（驾驶舱 OTA 面板"查询进度"） ----
    def run_query_offset(self):
        """发 QUERY_OFFSET 打印 Boot 已写入页数（断电续传断点）"""
        print(f"[cfg] profile={profile_name()}", flush=True)
        print(f"[open] {self._port} @ {self._baud}", flush=True)
        self._ser = serial.Serial(port=self._port, baudrate=self._baud, timeout=3,
                                  write_timeout=3)
        self._ser.reset_input_buffer()
        time.sleep(0.3)
        try:
            pages = self._query_offset()
            if pages is None:
                print("[offset] 无应答（未进入升级模式 / Boot SUCCESS 死循环 / CDC 假死）",
                      flush=True)
                return False
            print(f"[offset] 已写 {pages} 页 ({pages*1024} 字节)", flush=True)
            return True
        finally:
            if self._ser and self._ser.is_open:
                self._ser.close()


def main():
    ap = argparse.ArgumentParser(description="CLI OTA 端到端升级 / 查询 / 重置")
    ap.add_argument("--mode", choices=["upgrade", "query", "reset", "trigger",
                                       "query_offset"],
                    default="upgrade", help="操作模式(默认完整升级)")
    ap.add_argument("--profile", default=None,
                    help="覆盖 config.APP_PROFILE (AT32/STM32, 默认读 config.py)")
    ap.add_argument("--port", required=True, help="串口号, 如 COM10")
    ap.add_argument("--firmware", required=False, default=None,
                    help="固件 .bin 路径(仅 upgrade 模式必填)")
    ap.add_argument("--listen", type=int, default=8, help="复位后监听秒数")
    ap.add_argument("--throttle", type=float, default=0.0,
                    help="每帧发送后延迟秒数(限制发送/慢速测试)")
    ap.add_argument("--corrupt-frame", type=int, default=-1,
                    help="破坏指定帧(0-based)的 CRC 后验证 NAK 重传")
    ap.add_argument("--reset-dir", default=None,
                    help="openocd 脚本目录(含 openocd/ 子目录), 提供则硬件复位"
                         "(Boot 置 SUCCESS 后命令帧失效必须硬件复位)")
    args = ap.parse_args()

    if args.mode == "upgrade" and not args.firmware:
        ap.error("--firmware 在 upgrade 模式必填")

    if args.profile:
        _config.APP_PROFILE = args.profile
    profile = get_profile()

    try:
        if args.mode == "query":
            runner = CliIAP(args.port, profile["baud"], profile)
            ok = runner.run_query()
        elif args.mode == "reset":
            runner = CliIAP(args.port, profile["baud"], profile)
            ok = runner.run_reset()
        elif args.mode == "trigger":
            runner = CliIAP(args.port, profile["baud"], profile)
            ok = runner.run_trigger()
        elif args.mode == "query_offset":
            runner = CliIAP(args.port, profile["baud"], profile)
            ok = runner.run_query_offset()
        else:
            runner = CliIAP(args.port, profile["baud"], profile, reset_dir=args.reset_dir)
            ok = runner.run(args.firmware, listen_after=args.listen,
                            throttle=args.throttle, corrupt_frame=args.corrupt_frame)
        print("[result]", "PASS" if ok else "FAIL", flush=True)
        sys.exit(0 if ok else 1)
    except Exception as e:
        # write_timeout / 串口占用 / CDC 假死等全部收敛为友好错误（不裸 traceback）
        print(f"[error] {type(e).__name__}: {e}", flush=True)
        print("[result] FAIL", flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
