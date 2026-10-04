# ============================================================
# watchdog.ps1 — 驾驶舱服务看门狗（保活）
#
# 背景: dashboard_server.ps1 在 DAPLink 拔插/掉电时可能被连带终止,
#       本脚本每 N 秒探测服务端口, 挂了自动拉起, 保证驾驶舱长期在线。
#
# 用法: pwsh -NoProfile -ExecutionPolicy Bypass -File tools\watchdog.ps1 [-Port 8080] [-SerialPort COM10] [-Interval 30]
#   与 dashboard.ps1 启动器集成: dashboard.ps1 会先拉起本看门狗再启动服务。
#   日志: logs\watchdog.log (追加)
# ============================================================
param(
    [int]$Port = 8080,
    [string]$SerialPort = "",
    [int]$Interval = 30
)

$Project = Split-Path $PSScriptRoot -Parent
$Server  = Join-Path $PSScriptRoot "dashboard_server.ps1"
$LogDir  = Join-Path $Project "logs"
$WdLog   = Join-Path $LogDir "watchdog.log"
New-Item -ItemType Directory -Force $LogDir | Out-Null

function Log([string]$msg) {
    $line = "{0} [watchdog] {1}" -f (Get-Date).ToString("yyyy-MM-dd HH:mm:ss"), $msg
    try {
        [System.IO.File]::AppendAllText($WdLog, $line + "`r`n", [System.Text.Encoding]::UTF8)
    } catch { }
    Write-Host $line
}

function Test-Server([int]$port) {
    try {
        $r = Invoke-WebRequest -Uri "http://127.0.0.1:$port/api/status" -TimeoutSec 3 -UseBasicParsing
        return $r.StatusCode -eq 200
    } catch { return $false }
}

function Start-Server {
    # 清掉残留服务进程(防多实例抢端口)
    Get-CimInstance Win32_Process -Filter "Name='pwsh.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like '*dashboard_server*' } |
        ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop } catch { } }
    Start-Sleep -Milliseconds 800
    # 用与看门狗相同的 pwsh 版本拉起服务(隐藏窗口, 独立常驻)
    $pwsh = (Get-Process -Id $PID).Path
    $args = "-NoProfile -ExecutionPolicy Bypass -File `"$Server`" -Port $Port"
    if ($SerialPort -ne "") { $args += " -SerialPort $SerialPort" }
    $srvLog = Join-Path $LogDir "dashboard_server.log"
    $proc = Start-Process $pwsh -ArgumentList $args -WindowStyle Hidden `
        -RedirectStandardOutput $srvLog -RedirectStandardError "$srvLog.err" -PassThru
    Log "服务已拉起 (PID=$($proc.Id) $args)"
    Start-Sleep -Milliseconds 1200
}

# ---- 防多开: 已有看门狗在跑则直接退出 ----
$dup = Get-CimInstance Win32_Process -Filter "Name='pwsh.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -like '*watchdog.ps1*' }
if ($dup) {
    Log "已有看门狗在运行 (PID=$($dup.ProcessId -join ',')) → 本实例退出"
    exit 0
}

# ---- 首次检查: 服务未起则拉起 ----
if (-not (Test-Server $Port)) {
    Log "服务不在线 → 首次拉起"
    Start-Server
} else {
    Log "服务在线 (http://127.0.0.1:$Port) → 进入保活循环"
}

# ---- 保活循环 ----
while ($true) {
    Start-Sleep -Seconds $Interval
    if (-not (Test-Server $Port)) {
        Log "探测失败(端口无响应) → 拉起服务"
        Start-Server
    }
}
