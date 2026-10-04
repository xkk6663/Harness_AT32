# ============================================================
# dashboard.ps1 — 一键启动工具链驾驶舱（Step 8）
#
# 用法: 在工程根目录运行
#   pwsh -File tools\dashboard.ps1 [-Port 8080] [-SerialPort COM10]
#   -SerialPort 缺省时自动探测(DAPLink/CP210x/FTDI/CH340 优先)。
#   串口直连模式: dashboard 服务持有串口, 前端串口面板实时刷新;
#   此模式下不要再同时运行 tools\serial.ps1(端口独占)。
#
# 启动本机数据服务（127.0.0.1 安全绑定）+ 自动打开浏览器。
# 服务日志: logs\dashboard_server.log
# ============================================================
param(
    [int]$Port = 8080,
    [string]$SerialPort = ""
)

$Project = Split-Path $PSScriptRoot -Parent
$Server = Join-Path $PSScriptRoot "dashboard_server.ps1"
$Watchdog = Join-Path $PSScriptRoot "watchdog.ps1"
$SrvLog = Join-Path $Project "logs\dashboard_server.log"
New-Item -ItemType Directory -Force (Split-Path $SrvLog) | Out-Null

# 自动探测串口(DAPLink 优先, 其次常见 USB 串口)
if ($SerialPort -eq "") {
    foreach ($c in @([System.IO.Ports.SerialPort]::GetPortNames())) {
        try {
            $dev = Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
                Where-Object { $_.Name -match "\($c\)" } | Select-Object -First 1
            if ($dev -and $dev.Name -match 'DAPLink|CMSIS-DAP|CP210x|FTDI|CH340|USB Serial') {
                $SerialPort = $c; break
            }
        } catch { }
    }
}
if ($SerialPort -ne "") { Write-Host "串口直连: $SerialPort (dashboard 实时模式)" }
else { Write-Host "未探测到串口 → 串口面板回退显示 logs/serial.log" }

# 检查是否已在运行
$alive = $false
try {
    $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/api/status" -TimeoutSec 2 -UseBasicParsing
    if ($r.StatusCode -eq 200) { $alive = $true }
} catch { }

if ($alive) {
    Write-Host "服务已在运行: http://127.0.0.1:$Port (若要启用串口直连, 先停旧服务再启动)"
} else {
    # 独立进程启动（隐藏窗口），不随本会话结束
    $args = "-NoProfile -ExecutionPolicy Bypass -File `"$Server`" -Port $Port"
    if ($SerialPort -ne "") { $args += " -SerialPort $SerialPort" }
    Start-Process pwsh -ArgumentList $args `
        -WindowStyle Hidden -RedirectStandardOutput $SrvLog -RedirectStandardError "$SrvLog.err"
    Start-Sleep -Milliseconds 1200
    Write-Host "服务已启动: http://127.0.0.1:$Port  (日志: $SrvLog)"
}

# 看门狗保活: 已有看门狗进程则跳过, 否则拉起(服务挂了自动重启, 防拔插/误杀)
$wdAlive = @(Get-CimInstance Win32_Process -Filter "Name='pwsh.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -like '*watchdog.ps1*' })
if ($wdAlive.Count -eq 0) {
    $wdArgs = "-NoProfile -ExecutionPolicy Bypass -File `"$Watchdog`" -Port $Port"
    if ($SerialPort -ne "") { $wdArgs += " -SerialPort $SerialPort" }
    Start-Process pwsh -ArgumentList $wdArgs -WindowStyle Hidden
    Write-Host "看门狗已启动 (服务保活, 每 30s 探测, 日志: logs\watchdog.log)"
} else {
    Write-Host "看门狗已在运行 (PID=$($wdAlive[0].ProcessId))"
}

Start-Process "http://127.0.0.1:$Port"
Write-Host "已在浏览器打开驾驶舱。关闭服务: Get-CimInstance Win32_Process -Filter `"Name='pwsh.exe'`" | Where-Object { `$_.CommandLine -like '*dashboard_server*' } | ForEach-Object { Stop-Process -Id `$_.ProcessId -Force }"
