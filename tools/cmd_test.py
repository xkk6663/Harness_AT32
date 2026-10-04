"""cmd_test.py — 专项: 触发升级 → 逐个协议命令应答诊断（含完整串口输出）"""
import serial, time, subprocess, os, struct, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OPENOCD = os.path.join(os.environ.get("LOCALAPPDATA", ""), "at32-tools", "OpenOCD", "V2.0.9", "bin", "openocd.exe")

FRAME_SOF, FRAME_EOF = 0xAA, 0x55
FRAME_TYPE_CMD = 0x02
CMD_QUERY_OFFSET, CMD_RESET_UPGRADE, CMD_QUERY_STATE = 0x12, 0x13, 0x14


def crc32(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB88320 if crc & 1 else crc >> 1
    return crc ^ 0xFFFFFFFF


def cmd_frame(cmd):
    body = bytes([FRAME_TYPE_CMD, cmd])
    return bytes([FRAME_SOF]) + body + struct.pack("<I", crc32(body)) + bytes([FRAME_EOF])


def soft_reset():
    subprocess.run([OPENOCD, "-f", PROJECT + r"\openocd\interface\cmsis-dap.cfg",
                    "-f", PROJECT + r"\openocd\target\at32f421xx.cfg",
                    "-c", "init; mww 0xE000ED0C 0x05FA0004; sleep 100; shutdown"],
                   capture_output=True, timeout=30)
    time.sleep(0.3)


def drain(ser, seconds):
    buf = b""
    t0 = time.time()
    while time.time() - t0 < seconds:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
        time.sleep(0.02)
    return buf


ser = serial.Serial(port, 115200, timeout=1)
ser.reset_input_buffer()
time.sleep(0.2)

print(">>> 复位 (期望 RUNNING)")
soft_reset()
time.sleep(0.3)
ser.write(b"!!!!!")
print(">>> 已发 !!!!!")
buf = drain(ser, 3)
print(buf.decode(errors="replace"))
print(">>> 状态 (应=UPGRADING 主循环)")

# 逐个命令
for name, cmd in [("QUERY_STATE", CMD_QUERY_STATE),
                  ("QUERY_OFFSET", CMD_QUERY_OFFSET),
                  ("RESET_UPGRADE", CMD_RESET_UPGRADE)]:
    print(f"\n>>> 发 {name} (0x{cmd:02X}), 等应答 6s ...")
    ser.reset_input_buffer()
    ser.write(cmd_frame(cmd))
    buf = drain(ser, 6)
    print(f"--- {len(buf)} bytes ---")
    print(buf.decode(errors="replace"))
    # 应答帧 hex 定位: 找 0xAA 开头帧
    idx = buf.find(b"\xaa")
    while idx >= 0 and idx + 2 < len(buf):
        # 可能应答帧: AA 02 xx xx ...
        print(f"  hex@[{idx}]: {' '.join(f'{b:02x}' for b in buf[idx:idx+16])}")
        if buf[idx] == 0xAA and len(buf) - idx >= 9 and buf[idx + 1] == 0x02:
            plen = buf[idx + 3]
            total = 4 + plen + 4 + 1
            if idx + total <= len(buf):
                frame = buf[idx:idx + total]
                print(f"  FRAME({total}B): {' '.join(f'{b:02x}' for b in frame)}")
                break
        idx = buf.find(b"\xaa", idx + 1)
ser.close()
