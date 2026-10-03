# ============================================================
# dashboard_server.ps1 — 工具链驾驶舱数据服务（Step 8）
#
# 用法: pwsh -File tools\dashboard_server.ps1 [-Port 8080] [-SerialPort COM10]
#   Ctrl+C 结束。服务只绑定 127.0.0.1（本机安全）。
#   -SerialPort: 指定后服务端直接持有串口(115200), 前端串口面板走实时数据;
#                不指定则回退显示 logs/serial.log(由 tools/serial.ps1 落盘)。
#
# API:
#   GET /                      → dashboard.html
#   GET /api/status            → 进程/elf/日志元信息/最后心跳/串口连接状态
#   GET /api/serial            → 串口实时数据(自上次请求以来的新增文本)
#   GET /api/log?name=<x>&lines=N → 日志尾部 N 行（name ∈ build|flash|debug|serial）
# ============================================================
param(
    [int]$Port = 8080,
    [string]$SerialPort = ""
)

$Root = Split-Path $PSScriptRoot -Parent
$LogDir = Join-Path $Root "logs"
$HtmlPath = Join-Path $PSScriptRoot "dashboard.html"
$AllowedLogs = @("build", "flash", "debug", "serial")

function Send-Json($ctx, $obj) {
    $body = $obj | ConvertTo-Json -Depth 6 -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($body)
    $ctx.Response.ContentType = "application/json; charset=utf-8"
    # 禁止缓存: 前端轮询拿到的必须是实时数据(否则旧 HTML/旧日志停更)
    $ctx.Response.Headers["Cache-Control"] = "no-store, no-cache, must-revalidate"
    $ctx.Response.Headers["Pragma"] = "no-cache"
    $ctx.Response.ContentLength64 = $bytes.Length
    $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
}

function Send-File($ctx, $path, $contentType) {
    if (-not (Test-Path $path)) { $ctx.Response.StatusCode = 404; return }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $ctx.Response.ContentType = $contentType
    # 禁止缓存: HTML 每次重新验证, 改版后用户无需手动清缓存
    $ctx.Response.Headers["Cache-Control"] = "no-store, no-cache, must-revalidate"
    $ctx.Response.Headers["Pragma"] = "no-cache"
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
        serial     = @{ connected = ($script:Serial -ne $null); port = $SerialPort }
    }
}

# ---- 串口直连（服务端持有串口, 行缓冲后按"最近 N 行"供前端覆盖式刷新）----
$script:Serial = $null
$script:SerialBuf = New-Object System.Collections.Generic.List[string]   # 完整行缓冲(上限 300 行)
$script:SerialTail = ""                                                  # 未换行的残尾(拼到下次)
# 未显式指定串口时自动探测(DAPLink/CMSIS-DAP 优先, 其次常见 USB 串口)——COM 号会因 USB 重枚举变化
if ($SerialPort -eq "") {
    foreach ($c in @([System.IO.Ports.SerialPort]::GetPortNames())) {
        try {
            $dev = Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
                Where-Object { $_.Name -match "\($c\)" } | Select-Object -First 1
            if ($dev -and $dev.Name -match 'DAPLink|CMSIS-DAP|CP210x|FTDI|CH340|USB Serial') {
                $SerialPort = $c
                break
            }
        } catch { }
    }
}
if ($SerialPort -ne "") {
    try {
        $sp = New-Object System.IO.Ports.SerialPort($SerialPort, 115200, 'None', 8, 'One')
        $sp.ReadTimeout = 300
        $sp.DtrEnable = $true
        $sp.RtsEnable = $true
        $sp.Open()
        $script:Serial = $sp
        Write-Host "串口已连接: $SerialPort @115200 (dashboard 实时模式)"
    } catch {
        Write-Host "串口连接失败: $SerialPort ($_)  → 回退显示 logs/serial.log"
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
            } elseif ($path -eq "/api/serial") {
                if ($script:Serial -ne $null) {
                    # 读新数据 → 拼残尾 → 按行入缓冲(上限 300 行) → 返回最近 60 行(前端覆盖式刷新)
                    $new = ""
                    try { $new = $script:Serial.ReadExisting() } catch { }
                    if ($new) {
                        $text = $script:SerialTail + $new
                        $parts = $text -split "`n"
                        $script:SerialTail = $parts[$parts.Count - 1]
                        for ($i = 0; $i -lt $parts.Count - 1; $i++) {
                            $l = $parts[$i].TrimEnd("`r")
                            if ($l -ne "") { [void]$script:SerialBuf.Add($l) }
                        }
                        if ($script:SerialBuf.Count -gt 300) {
                            $script:SerialBuf.RemoveRange(0, $script:SerialBuf.Count - 300)
                        }
                    }
                    $lines = @($script:SerialBuf | Select-Object -Last 60)
                    Send-Json $ctx @{ live = $true; port = $SerialPort; lines = $lines }
                } else {
                    Send-Json $ctx @{ live = $false; lines = @() }
                }
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
