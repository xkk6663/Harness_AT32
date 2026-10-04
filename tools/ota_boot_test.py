#!/usr/bin/env python3
"""
ota_boot_test.py — M1 Bootloader 验收测试
========================================
自动验证 AT32F421 Bootloader 的协议行为（v1.1 决策: 纯软件触发, 无按键）：

  场景A: 复位 → 2s 窗口发 '!!!!!' → 触发升级 (Signal detected + Erase done)
  场景B: 升级模式下协议命令应答:
         QUERY_OFFSET(0x12) → RSP_OFFSET(0x22, 页数)
         QUERY_STATE (0x14) → RSP_STATE (0x23, 状态值)
         RESET_UPGRADE(0x13) → RSP_ACK(0x20)
  场景C: 复位(状态=UPGRADING) → 断电续传路径 → "No valid resume point, starting fresh"

用法:
  python ota_boot_test.py            # 自动探测串口
  python ota_boot_test.py --port COM10
  python ota_boot_test.py --no-flash # 跳过 openocd 复位（已有 bootloader 在跑）
"""
import argparse
import glob
import os
import struct
import subprocess
import sys
import time

import serial
import serial.tools.list_ports

# ── 协议常量（与 ota/core/ota_protocol.h / ota_common.h 一致） ──
FRAME_SOF, FRAME_EOF = 0xAA, 0x55
FRAME_TYPE_CMD = 0x02
CMD_QUERY_OFFSET, CMD_RESET_UPGRADE, CMD_QUERY_STATE = 0x12, 0x13, 0x14
RSP_ACK, RSP_NAK, RSP_OFFSET, RSP_STATE = 0x20, 0x21, 0x22, 0x23
STATE_RUNNING, STATE_UPGRADE_READY, STATE_UPGRADING = 0xA5A5A5A0, 0xA5A5A5A1, 0xA5A5A5A2
STATE_CRC_FAIL, STATE_UPGRADE_SUCCESS = 0xA5A5A5A3, 0xA5A5A5A4

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OPENOCD = os.path.join(os.environ.get("LOCALAPPDATA", ""),
                       "at32-tools", "OpenOCD", "V2.0.9", "bin", "openocd.exe")
INTERFACE = os.path.join(PROJECT, "openocd", "interface", "cmsis-dap.cfg")
TARGET = os.path.join(PROJECT, "openocd", "target", "at32f421xx.cfg")


# ── CRC32 (反射多项式 0xEDB88320, 与 ota_crc32.c 一致) ──
def crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB88320 if crc & 1 else crc >> 1
    return crc ^ 0xFFFFFFFF


# ── 帧构建 / 应答解析 ──
def cmd_frame(cmd: int) -> bytes:
    body = bytes([FRAME_TYPE_CMD, cmd])
    return bytes([FRAME_SOF]) + body + struct.pack("<I", crc32(body)) + bytes([FRAME_EOF])


def find_serial_port() -> str:
    ports = serial.tools.list_ports.comports()
    if not ports:
        return ""
    # ① 优先 DAPLink: VID_0D28 (mbed/ARM), 名称或 hwid 均可
    for p in ports:
        hw = p.hwid or ""
        name = p.description or ""
        if "VID_0D28" in hw.upper() or "DAPLink" in name or "CMSIS-DAP" in name:
            return p.device
    # ② USB 转串口 (CP210x/FTDI/CH340/AT32/通用 USB 串行设备), 排除蓝牙
    for p in ports:
        hw = p.hwid or ""
        name = p.description or ""
        if "BTHENUM" in hw.upper() or "bluetooth" in name.lower():
            continue
        if hw.startswith("USB") or "VID_" in hw.upper():
            return p.device
    return ""  # 蓝牙虚拟串口一律不认


def soft_reset():
    """软复位（SYSRESETREQ 写 AIRCR）——串口须已打开, 否则错过启动打印"""
    cmd = [OPENOCD, "-f", INTERFACE, "-f", TARGET,
           "-c", "init; mww 0xE000ED0C 0x05FA0004; sleep 200; shutdown"]
    subprocess.run(cmd, capture_output=True, timeout=30)
    time.sleep(0.2)


def read_until(ser: serial.Serial, markers, timeout=6.0):
    """读串口直到任一标记出现, 返回累计文本"""
    buf = b""
    t0 = time.time()
    while time.time() - t0 < timeout:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
            for m in markers:
                if m.encode() in buf:
                    return buf.decode(errors="replace")
        time.sleep(0.02)
    return buf.decode(errors="replace")


def recv_frame(ser: serial.Serial, timeout=3.0) -> bytes:
    """收一帧应答 (AA 02 CMD LEN [PARAM] CRC32 55), 返回原始字节; 超时返回 b''"""
    t0 = time.time()
    buf = b""
    while time.time() - t0 < timeout:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
            # 找 SOF
            idx = buf.find(bytes([FRAME_SOF]))
            while idx >= 0 and idx + 2 < len(buf):
                if buf[idx + 1] == FRAME_TYPE_CMD and buf[idx + 2] in (RSP_ACK, RSP_NAK, RSP_OFFSET, RSP_STATE):
                    param_len = buf[idx + 3] if len(buf) >= idx + 4 else 0
                    total = 4 + param_len + 4 + 1  # SOF TYPE CMD LEN + PARAM + CRC + EOF
                    if len(buf) >= idx + total:
                        # 检查帧尾（idx+total-1）而不是缓冲尾
                        if buf[idx + total - 1] == FRAME_EOF:
                            return buf[idx:idx + total]
                idx = buf.find(bytes([FRAME_SOF]), idx + 1)
        time.sleep(0.01)
    return b""


def parse_frame(frame: bytes):
    """解析应答帧 → (cmd, param_bytes); 校验 CRC, 失败返回 (None, None)"""
    if len(frame) < 9 or frame[0] != FRAME_SOF or frame[-1] != FRAME_EOF:
        return None, None
    param_len = frame[3]
    body = frame[1:4 + param_len]           # TYPE CMD LEN PARAM
    crc_recv = struct.unpack("<I", frame[4 + param_len:8 + param_len])[0]
    if crc32(body) != crc_recv:
        return None, None
    return frame[2], frame[4:4 + param_len]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="", help="串口, 默认自动探测")
    ap.add_argument("--no-reset", action="store_true", help="跳过 openocd 复位")
    args = ap.parse_args()

    port = args.port or find_serial_port()
    if not port:
        print("✗ 未发现串口"); sys.exit(1)
    print(f"串口: {port} @ 115200")

    ser = serial.Serial(port, 115200, timeout=1)
    ser.reset_input_buffer()
    passed = []

    def check(name, ok, detail=""):
        print(f"  {'✓' if ok else '✗'} {name} {detail}")
        passed.append(ok)

    # ══ 场景A: 复位 → 2s 窗口发 !!!!! → 触发升级 ══
    print("\n═══ 场景A: 纯软件触发升级 (2s 窗口 '!!!!!') ═══")
    if not args.no_reset:
        # 前置: 状态页写 RUNNING, 确保走 Ready+2s 窗口分支（上次测试可能留下 UPGRADING）
        subprocess.run([sys.executable, os.path.join(PROJECT, "tools", "reset_state.py"),
                        "0xA5A5A5A0"], capture_output=True, timeout=60)
        time.sleep(0.3)
        soft_reset()
    text = read_until(ser, ["Bootloader Ready"], timeout=6)
    print("  Bootloader 打印:")
    for ln in text.splitlines()[-8:]:
        print(f"    | {ln}")
    check("启动打印", "Bootloader Ready" in text)

    # 2s 窗口内发 !!!!!
    ser.write(b"!!!!!")
    text = read_until(ser, ["entering upgrade mode", "ready to receive firmware"], timeout=6)
    check("触发升级", "entering upgrade mode" in text or "ready to receive firmware" in text)

    # ══ 场景B: 升级模式下协议命令应答 ══
    print("\n═══ 场景B: 协议命令应答 ═══")
    ser.write(cmd_frame(CMD_QUERY_STATE))
    f = recv_frame(ser)
    cmd, param = parse_frame(f)
    state = struct.unpack("<I", param)[0] if param and len(param) >= 4 else 0
    check("QUERY_STATE→RSP_STATE", cmd == RSP_STATE and state == STATE_UPGRADING,
          f"state=0x{state:08X}")

    ser.write(cmd_frame(CMD_QUERY_OFFSET))
    f = recv_frame(ser)
    cmd, param = parse_frame(f)
    pages = struct.unpack("<H", param)[0] if param and len(param) >= 2 else 0xFF
    check("QUERY_OFFSET→RSP_OFFSET", cmd == RSP_OFFSET, f"pages={pages}")

    ser.write(cmd_frame(CMD_RESET_UPGRADE))
    f = recv_frame(ser)
    cmd, param = parse_frame(f)
    check("RESET_UPGRADE→RSP_ACK", cmd == RSP_ACK)

    # ══ 场景C: 复位 → 续传路径 (状态=UPGRADING) ══
    print("\n═══ 场景C: 断电续传路径 (复位后状态=UPGRADING) ═══")
    if not args.no_reset:
        soft_reset()
    text = read_until(ser, ["starting fresh", "Bootloader Ready"], timeout=6)
    check("续传分支", "No valid resume point" in text or "starting fresh" in text)

    # 续传后主循环可收命令
    ser.write(cmd_frame(CMD_QUERY_STATE))
    f = recv_frame(ser)
    cmd, param = parse_frame(f)
    state = struct.unpack("<I", param)[0] if param and len(param) >= 4 else 0
    check("续传后 QUERY_STATE", cmd == RSP_STATE and state == STATE_UPGRADING,
          f"state=0x{state:08X}")

    ser.close()
    print(f"\n===== 结果: {sum(passed)}/{len(passed)} 通过 =====")
    sys.exit(0 if all(passed) else 1)


if __name__ == "__main__":
    main()
