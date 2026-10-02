# ============================================================
# dashboard_server.ps1 — 工具链驾驶舱数据服务（Step 8）
#
# 用法: pwsh -File tools\dashboard_server.ps1 [-Port 8080]
#   Ctrl+C 结束。服务只绑定 127.0.0.1（本机安全）。
#
# API:
#   GET /                      → dashboard.html
#   GET /api/status            → 进程/elf/日志元信息/最后心跳
#   GET /api/log?name=<x>&lines=N → 日志尾部 N 行（name ∈ build|flash|debug|serial）
# ============================================================
param(
    [int]$Port = 8080
)

$Root = Split-Path $PSScriptRoot -Parent
$LogDir = Join-Path $Root "logs"
$HtmlPath = Join-Path $PSScriptRoot "dashboard.html"
$AllowedLogs = @("build", "flash", "debug", "serial")

function Send-Json($ctx, $obj) {
    $body = $obj | ConvertTo-Json -Depth 6 -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($body)
    $ctx.Response.ContentType = "application/json; charset=utf-8"
    $ctx.Response.ContentLength64 = $bytes.Length
    $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
}

function Send-File($ctx, $path, $contentType) {
    if (-not (Test-Path $path)) { $ctx.Response.StatusCode = 404; return }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $ctx.Response.ContentType = $contentType
    $ctx.Response.ContentLength64 = $bytes.Length
    $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
}

function Get-Status {
    $openocd = @(Get-Process -Name openocd -ErrorAction SilentlyContinue).Count
    $gdb     = @(Get-Process -Name arm-none-eabi-gdb -ErrorAction SilentlyContinue).Count
    $elfPath = Join-Path $Root "build\Debug\AT32F421G8U7_WorkBench.elf"
    $elf = $null
    if (Test-Path $elfPath) {
        $fi = Get-Item $elfPath
        $elf = @{ sizeBytes = $fi.Length; modified = $fi.LastWriteTime.ToString("HH:mm:ss") }
    }
    $logs = @{}
    foreach ($n in $AllowedLogs) {
        $p = Join-Path $LogDir "$n.log"
        if (Test-Path $p) {
            $fi = Get-Item $p
            $logs[$n] = @{ sizeBytes = $fi.Length; modified = $fi.LastWriteTime.ToString("HH:mm:ss") }
        } else {
            $logs[$n] = $null
        }
    }
    # 最后一条心跳（从 serial.log 尾部提取）
    $heartbeat = $null
    $serialPath = Join-Path $LogDir "serial.log"
    if (Test-Path $serialPath) {
        $tail = [System.IO.File]::ReadLines($serialPath, [System.Text.Encoding]::UTF8) |
            Select-Object -Last 30
        foreach ($line in $tail) {
            if ($line -match "heartbeat tick=(\d+) high_loop=(\d+) adc\[([^\]]+)\]") {
                $heartbeat = @{ tick = [long]$Matches[1]; highLoop = [long]$Matches[2]; adc = $Matches[3] }
            }
        }
    }
    return @{
        serverTime = (Get-Date).ToString("HH:mm:ss")
        processes  = @{ openocd = $openocd; gdb = $gdb }
        elf        = $elf
        logs       = $logs
        heartbeat  = $heartbeat
    }
}

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://127.0.0.1:$Port/")
$listener.Start()
Write-Host "===== dashboard 服务: http://127.0.0.1:$Port (Ctrl+C 退出) ====="

try {
    while ($true) {
        $ctx = $listener.GetContext()
        try {
            $path = $ctx.Request.Url.AbsolutePath
            if ($path -eq "/") {
                Send-File $ctx $HtmlPath "text/html; charset=utf-8"
            } elseif ($path -eq "/api/status") {
                Send-Json $ctx (Get-Status)
            } elseif ($path -eq "/api/log") {
                $name = $ctx.Request.QueryString["name"]
                $lines = [int]$ctx.Request.QueryString["lines"]
                if ($lines -le 0 -or $lines -gt 500) { $lines = 200 }
                if ($AllowedLogs -notcontains $name) {
                    $ctx.Response.StatusCode = 400
                } else {
                    $logPath = Join-Path $LogDir "$name.log"
                    $content = if (Test-Path $logPath) {
                        [System.IO.File]::ReadAllText($logPath, [System.Text.Encoding]::UTF8)
                    } else { "" }
                    $arr = $content -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 }
                    $tail = $arr | Select-Object -Last $lines
                    Send-Json $ctx @{ name = $name; lines = @($tail) }
                }
            } else {
                $ctx.Response.StatusCode = 404
            }
        } catch {
            $ctx.Response.StatusCode = 500
        } finally {
            $ctx.Response.Close()
        }
    }
} finally {
    $listener.Stop()
}
