"""read_boot.py — 先打开串口再触发复位, 不遗漏 Bootloader 启动打印"""
import serial, time, sys, subprocess, os

port = sys.argv[1] if len(sys.argv) > 1 else "COM10"

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OPENOCD = os.path.join(os.environ.get("LOCALAPPDATA", ""), "at32-tools", "OpenOCD", "V2.0.9", "bin", "openocd.exe")
IFACE = os.path.join(PROJECT, "openocd", "interface", "cmsis-dap.cfg")
TGT = os.path.join(PROJECT, "openocd", "target", "at32f421xx.cfg")

ser = serial.Serial(port, 115200, timeout=1)
ser.reset_input_buffer()
time.sleep(0.2)

# 触发软复位（SYSRESETREQ, 写 AIRCR）
print(">>> 触发软复位 ...")
subprocess.run([OPENOCD, "-f", IFACE, "-f", TGT,
                "-c", "init; mww 0xE000ED0C 0x05FA0004; sleep 200; shutdown"],
               capture_output=True, timeout=30)

buf = b""
t0 = time.time()
while time.time() - t0 < 8:
    n = ser.in_waiting
    if n:
        data = ser.read(n)
        buf += data
    time.sleep(0.02)
print(f"=== {len(buf)} bytes ===")
print(buf.decode(errors="replace"))
ser.close()
