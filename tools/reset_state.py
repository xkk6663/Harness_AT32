"""reset_state.py — openocd 写升级状态字 + 读回验证"""
import subprocess, os, sys, struct

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OPENOCD = os.path.join(os.environ.get("LOCALAPPDATA", ""), "at32-tools", "OpenOCD", "V2.0.9", "bin", "openocd.exe")
INTERFACE = os.path.join(PROJECT, "openocd", "interface", "cmsis-dap.cfg")
TARGET = os.path.join(PROJECT, "openocd", "target", "at32f421xx.cfg")
STATE_ADDR = 0x0800FC00  # 页 63, UPGRADE_STATE_ADDR

state = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0xA5A5A5A0

cmds = [
    "init",
    "halt",
    f"flash erase_sector 0 63 63",
    f"flash fillw 0x{STATE_ADDR:08X} {state} 1",
    f"flash read_bank 0 {PROJECT}\\logs\\stater.bin 0x{STATE_ADDR:08X} 0x400",
    "reset run",
    "shutdown",
]
cmd = [OPENOCD, "-f", INTERFACE, "-f", TARGET]
for c in cmds:
    cmd += ["-c", c]

r = subprocess.run(cmd, capture_output=True, timeout=60)
out = (r.stdout.decode(errors="replace") + r.stderr.decode(errors="replace")).strip()
print(out[-1500:] if len(out) > 1500 else out)
print(f"exit={r.returncode}")

# 读回验证
try:
    with open(os.path.join(PROJECT, "logs", "stater.bin"), "rb") as f:
        d = f.read(4)
        got = struct.unpack("<I", d)[0]
    print(f"state readback: 0x{got:08X} {'OK' if got == state else 'MISMATCH'}")
except Exception as e:
    print(f"readback failed: {e}")
