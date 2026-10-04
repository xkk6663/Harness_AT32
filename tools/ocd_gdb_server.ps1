# ============================================================
# ocd_gdb_server.ps1 — 后台启动 OpenOCD GDB Server (3333)
# 供 gdb 调试 / PC 读取诊断使用
# ============================================================
$OpenOcd = "$env:LOCALAPPDATA\at32-tools\OpenOCD\V2.0.9\bin\openocd.exe"
$Project = Split-Path $PSScriptRoot -Parent
$iface = Join-Path $Project "openocd\interface\cmsis-dap.cfg"
$tgt   = Join-Path $Project "openocd\target\at32f421xx.cfg"
$log   = Join-Path $Project "logs\ocd_gdb.log"
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
& $OpenOcd -f $iface -f $tgt -c "gdb_port 3333" *> $log
