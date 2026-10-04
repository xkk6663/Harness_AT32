"""trigger_test.py — 复位后发 !!!!!, 验证触发升级 + 协议命令应答"""
import serial, time, subprocess, os, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OPENOCD = os.path.join(os.environ.get("LOCALAPPDATA", ""), "at32-tools", "OpenOCD", "V2.0.9", "bin", "openocd.exe")

ser = serial.Serial(port, 115200, timeout=1)
ser.reset_input_buffer()
time.sleep(0.2)

print(">>> 软复位 ...")
subprocess.run([OPENOCD, "-f", PROJECT + r"\openocd\interface\cmsis-dap.cfg",
                "-f", PROJECT + r"\openocd\target\at32f421xx.cfg",
                "-c", "init; mww 0xE000ED0C 0x05FA0004; sleep 100; shutdown"],
               capture_output=True, timeout=30)
time.sleep(0.4)

ser.write(b"!!!!!")
print(">>> 已发 !!!!!")

buf = b""
t0 = time.time()
while time.time() - t0 < 6:
    n = ser.in_waiting
    if n:
        buf += ser.read(n)
    time.sleep(0.02)
print(f"=== {len(buf)} bytes ===")
print(buf.decode(errors="replace"))
ser.close()
