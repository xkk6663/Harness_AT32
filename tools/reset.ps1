# ============================================================
# reset.ps1 — 目标芯片软复位（不重新烧录）
#
# 用法: 在工程根目录运行
#   pwsh -File tools\reset.ps1              # 默认 SYSRESETREQ 软复位
#   pwsh -File tools\reset.ps1 -Mode hard   # nRESET 硬复位
#
# 原理:
#   - soft: 通过 SWD 写 SCB->AIRCR (0xE000ED0C) 的 SYSRESETREQ 位
#           → 触发内核软件复位, 不依赖 nRESET 引脚, 不动 Flash
#   - hard: openocd reset run (走 nRESET, 部分板子复位不彻底)
# ============================================================
param(
    [ValidateSet("soft", "hard")] [string]$Mode = "soft"
)

$OpenOcd = "$env:LOCALAPPDATA\at32-tools\OpenOCD\V2.0.9\bin\openocd.exe"
$Project = Split-Path $PSScriptRoot -Parent
$Interface = Join-Path $Project "openocd\interface\cmsis-dap.cfg"
$Target    = Join-Path $Project "openocd\target\at32f421xx.cfg"

if (-not (Test-Path $OpenOcd)) { Write-Error "找不到 OpenOCD: $OpenOcd"; exit 1 }

if ($Mode -eq "soft") {
    # 软复位: AIRCR 写 0x05FA0004 (VECTKEY=0x05FA + SYSRESETREQ=1)
    Write-Host "===== 软复位 (SYSRESETREQ) ====="
    & $OpenOcd -f $Interface -f $Target -c "init; mww 0xE000ED0C 0x05FA0004; sleep 100; shutdown"
} else {
    Write-Host "===== 硬复位 (nRESET) ====="
    & $OpenOcd -f $Interface -f $Target -c "init; reset run; sleep 100; shutdown"
}

if ($LASTEXITCODE -eq 0) {
    Write-Host "===== 复位完成 ====="
} else {
    Write-Host "===== 复位失败 (exit=$LASTEXITCODE) — 检查调试器连接 ====="
    exit $LASTEXITCODE
}
