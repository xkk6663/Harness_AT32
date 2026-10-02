# ============================================================
# debug.ps1 — 启动 OpenOCD GDB server（Step 3）
#
# 用法（两个终端）:
#   终端1: powershell -ExecutionPolicy Bypass -File tools\debug.ps1
#           → Info : Listening on port 3333 for gdb connections
#   终端2: arm-none-eabi-gdb build/Debug/AT32F421G8U7_WorkBench.elf
#          (gdb) target remote :3333
#          (gdb) monitor reset halt
#          (gdb) break main
#          (gdb) continue
#
#   终端1 Ctrl+C 结束 server（client 可随时重连，不影响 server）
# ============================================================
$OpenOcd = "$env:LOCALAPPDATA\at32-tools\OpenOCD\V2.0.9\bin\openocd.exe"
$Project = Split-Path $PSScriptRoot -Parent

if (-not (Test-Path $OpenOcd)) { Write-Error "找不到 OpenOCD: $OpenOcd"; exit 1 }

Write-Host "===== OpenOCD GDB server: :3333 (Ctrl+C 退出) ====="
& $OpenOcd -f (Join-Path $Project "openocd\interface\cmsis-dap.cfg") -f (Join-Path $Project "openocd\target\at32f421xx.cfg")
