# ============================================================
# dashboard.ps1 — 一键启动工具链驾驶舱（Step 8）
#
# 用法: 在工程根目录运行
#   pwsh -File tools\dashboard.ps1 [-Port 8080]
#
# 启动本机数据服务（127.0.0.1 安全绑定）+ 自动打开浏览器。
# 服务日志: logs\dashboard_server.log
# 停止服务: Stop-Process -Name pwsh? 见下（用固定标题窗口便于查找）
# ============================================================
param(
    [int]$Port = 8080
)

$Project = Split-Path $PSScriptRoot -Parent
$Server = Join-Path $PSScriptRoot "dashboard_server.ps1"
$SrvLog = Join-Path $Project "logs\dashboard_server.log"
New-Item -ItemType Directory -Force (Split-Path $SrvLog) | Out-Null

# 检查是否已在运行
$alive = $false
try {
    $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/api/status" -TimeoutSec 2 -UseBasicParsing
    if ($r.StatusCode -eq 200) { $alive = $true }
} catch { }

if ($alive) {
    Write-Host "服务已在运行: http://127.0.0.1:$Port"
} else {
    # 独立进程启动（隐藏窗口），不随本会话结束
    Start-Process pwsh -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$Server`" -Port $Port" `
        -WindowStyle Hidden -RedirectStandardOutput $SrvLog -RedirectStandardError "$SrvLog.err"
    Start-Sleep -Milliseconds 1200
    Write-Host "服务已启动: http://127.0.0.1:$Port  (日志: $SrvLog)"
}

Start-Process "http://127.0.0.1:$Port"
Write-Host "已在浏览器打开驾驶舱。关闭服务: Get-CimInstance Win32_Process -Filter `"Name='pwsh.exe'`" | Where-Object { `$_.CommandLine -like '*dashboard_server*' } | ForEach-Object { Stop-Process -Id `$_.ProcessId -Force }"
