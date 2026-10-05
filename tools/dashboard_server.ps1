# ============================================================
# dashboard_server.ps1 — 工具链驾驶舱数据服务（Step 8 + 交互增强）
#
# 用法: pwsh -File tools\dashboard_server.ps1 [-Port 8080] [-SerialPort COM10]
#   Ctrl+C 结束。服务只绑定 127.0.0.1（本机安全）。
#   -SerialPort: 指定后服务端直接持有串口(115200), 前端串口面板走实时数据;
#                不指定则自动探测(DAPLink/CMSIS-DAP 优先)。
#
# API:
#   GET /                      → dashboard.html
#   GET /api/status            → 进程/elf/日志元信息/串口连接状态/运行中动作
#   GET /api/serial            → 串口实时数据(自上次请求以来的新增文本)
#   GET /api/log?name=<x>&lines=N → 日志尾部 N 行（name ∈ build|flash|debug|serial|reset）
#   GET /api/action?cmd=<x>    → 执行动作（cmd ∈ build|flash|reset|reconnect_serial）
#
# 交互增强(本轮):
#   - 串口自动重连: 拔插 DAPLink / COM 号变化 / 透传卡死(>15s 无数据)
#     都会自动恢复, 前端无需重启服务
#   - /api/action 白名单动作: 编译/烧录/复位运行/重连串口, 异步执行不阻塞服务
# ============================================================
param(
    [int]$Port = 8080,
    [string]$SerialPort = ""
)

$Root = Split-Path $PSScriptRoot -Parent
$LogDir = Join-Path $Root "logs"
$HtmlPath = Join-Path $PSScriptRoot "dashboard.html"
$AllowedLogs = @("build", "flash", "debug", "serial", "reset", "ota")
$AllowedActions = @("build", "flash", "reset", "reconnect_serial", "ota_upgrade", "ota_query", "ota_reset")
$OpenOcdPath = "$env:LOCALAPPDATA\at32-tools\OpenOCD\V2.0.9\bin\openocd.exe"

function Send-Json($ctx, $obj) {
    $body = $obj | ConvertTo-Json -Depth 6 -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($body)
    $ctx.Response.ContentType = "application/json; charset=utf-8"
    # 禁止缓存: 前端轮询拿到的必须是实时数据(否则旧 HTML/旧日志停更)
    $ctx.Response.Headers["Cache-Control"] = "no-store, no-cache, must-revalidate"
    $ctx.Response.Headers["Pragma"] = "no-cache"
    $ctx.Response.ContentLength64 = $bytes.Length
    try { $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length) } catch { }
}

function Send-File($ctx, $path, $contentType) {
    if (-not (Test-Path $path)) { $ctx.Response.StatusCode = 404; return }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $ctx.Response.ContentType = $contentType
    # 禁止缓存: HTML 每次重新验证, 改版后用户无需手动清缓存
    $ctx.Response.Headers["Cache-Control"] = "no-store, no-cache, must-revalidate"
    $ctx.Response.Headers["Pragma"] = "no-cache"
    $ctx.Response.ContentLength64 = $bytes.Length
    try { $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length) } catch { }
}

# ============================================================
# 串口管理: 自动连接 / 自动重连 / 自动探测
# ============================================================
$script:Serial = $null
$script:SerialTargetPort = ""                        # 上次成功连接的端口(重连优先)
$script:SerialConnected = $false
$script:SerialErr = ""
$script:SerialBuf = New-Object System.Collections.Generic.List[string]
$script:SerialTail = ""
$script:LastSerialData = [DateTime]::MinValue         # 最近一次收到数据的时间(判新鲜度)
$script:PortScanCache = $null                          # 端口探测结果缓存(30s)
$script:PortScanAt = [DateTime]::MinValue

function Open-SerialPort([string]$port) {
    try {
        $sp = New-Object System.IO.Ports.SerialPort($port, 115200, 'None', 8, 'One')
        $sp.ReadTimeout = 200
        $sp.DtrEnable = $true
        $sp.RtsEnable = $true
        $sp.Open()
        $script:Serial = $sp
        $script:SerialTargetPort = $port
        $script:SerialConnected = $true
        $script:SerialErr = ""
        $script:SerialBuf.Clear()
        $script:SerialTail = ""
        return $true
    } catch {
        $script:Serial = $null
        $script:SerialConnected = $false
        $script:SerialErr = $_.Exception.Message
        return $false
    }
}

function Find-SerialPort {
    # 扫描当前全部串口, 挑 DAPLink/CMSIS-DAP/USB 串口特征(COM 号会随拔插变化)
    # 缓存 30s: Get-CimInstance Win32_PnPEntity 全枚举实测 ~4s, 重连不能每次都跑
    if ($script:PortScanCache -and ((Get-Date) - $script:PortScanAt).TotalSeconds -lt 30) {
        return $script:PortScanCache
    }
    $result = $null
    $ports = @([System.IO.Ports.SerialPort]::GetPortNames())
    if ($ports.Count -gt 0) {
        try {
            # 一次 CIM 枚举建立快速判定表(不要对每个端口各查一次全表)
            # 关键: 必须同时满足 [特征设备] 与 [带COM号], 且排除蓝牙虚拟串口(蓝牙也带 COM 号, 误连会打开成功却收不到数据)
            $entities = Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
                Where-Object {
                    $_.Name -match 'DAPLink|CMSIS-DAP|CP210x|FTDI|CH340|USB Serial' -and
                    $_.Name -match '\(COM\d+\)' -and
                    $_.Name -notmatch '蓝牙|Bluetooth'
                }
            foreach ($c in $ports) {
                $hit = $entities | Where-Object { $_.Name -match "\($c\)" } | Select-Object -First 1
                if ($hit) { $result = $c; break }
            }
        } catch { }
        # CIM 失败兜底: 排除蓝牙后取第一个串口(不连蓝牙虚拟串口)
        if (-not $result) {
            try {
                $bt = Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
                    Where-Object { $_.Name -match '蓝牙|Bluetooth' -and $_.Name -match '\(COM(\d+)\)' }
                $btPorts = @($bt | ForEach-Object {
                    if ($_.Name -match '\(COM(\d+)\)') { $matches[1] }
                })
                $result = $ports | Where-Object { $_ -notin $btPorts } | Select-Object -First 1
            } catch {
                $result = $ports[0]
            }
        }
    }
    $script:PortScanCache = $result
    $script:PortScanAt = Get-Date
    return $result
}

function Close-Serial {
    if ($script:Serial) {
        try { $script:Serial.Close() } catch { }
        $script:Serial = $null
    }
    $script:SerialConnected = $false
}

# 保证串口在线: 已连→OK; 断开→按 目标端口→自动探测 顺序重连
function Ensure-SerialConnected([switch]$Force) {
    if ($Force) { Close-Serial }
    if ($script:Serial -and $script:Serial.IsOpen) { return $true }

    # 1) 优先重试上次成功端口(最快路径, 拔插恢复后 COM 号通常不变)
    if ($script:SerialTargetPort -ne "" -and (Open-SerialPort $script:SerialTargetPort)) {
        Write-Host "串口重连成功: $($script:SerialTargetPort)"
        return $true
    }
    # 2) 目标端口失败 → 自动探测(缓存内直接复用, 避免 4s CIM 枚举)
    $detected = Find-SerialPort
    if ($detected -and (Open-SerialPort $detected)) {
        Write-Host "串口重连成功(自动探测): $detected"
        return $true
    }
    $script:SerialErr = "无可用串口(拔插后自动重试中)"
    return $false
}

# ---- 串口初始化(启动时) ----
if ($SerialPort -ne "") {
    if (-not (Open-SerialPort $SerialPort)) {
        Write-Host "串口连接失败: $SerialPort ($($script:SerialErr)) → 自动重连模式"
    } else {
        Write-Host "串口已连接: $SerialPort @115200 (dashboard 实时模式)"
    }
} else {
    $detected = Find-SerialPort
    if ($detected -and (Open-SerialPort $detected)) {
        Write-Host "串口已连接(自动探测): $detected @115200"
    } else {
        Write-Host "未找到串口 → 自动重连模式(插上 DAPLink 后自动恢复)"
    }
}

# ============================================================
# 动作执行: 白名单 + 异步 + pid 跟踪(防重复触发)
# ============================================================
$script:Running = @{}   # cmd → @{ pid; start }

function Get-RunningCmds {
    $res = @()
    # 防御: Running 可能被求值为 null(空 Keys 边界) → 重置
    if ($null -eq $script:Running -or $script:Running -isnot [System.Collections.IDictionary]) {
        $script:Running = @{}
        return $res
    }
    foreach ($k in @($script:Running.Keys)) {
        $r = $script:Running[$k]
        if (Get-Process -Id $r.pid -ErrorAction SilentlyContinue) { $res += $k }
        else { $script:Running.Remove($k) }
    }
    return $res
}

function Start-CmdAction([string]$cmd, $Query) {
    if ($cmd -eq "build") {
        # 与插件"编译"按钮等价; 脚本内 Start-Transcript 落盘 logs/build.log
        $scriptPath = Join-Path $PSScriptRoot "build.ps1"
        $proc = Start-Process pwsh -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", $scriptPath, "-Preset", "Debug") -WorkingDirectory $Root -WindowStyle Hidden -PassThru
        return $proc.Id
    }
    elseif ($cmd -eq "flash") {
        # 烧录: 先杀残留 openocd(本硬件纪律), 再跑 flash.ps1(落盘 logs/flash.log)
        Get-Process -Name openocd -ErrorAction SilentlyContinue | Stop-Process -Force
        $scriptPath = Join-Path $PSScriptRoot "flash.ps1"
        $proc = Start-Process pwsh -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", $scriptPath) -WorkingDirectory $Root -WindowStyle Hidden -PassThru
        return $proc.Id
    }
    elseif ($cmd -eq "reset") {
        # 复位运行: openocd init → reset run → shutdown(落盘 logs/reset.log)
        Get-Process -Name openocd -ErrorAction SilentlyContinue | Stop-Process -Force
        $interface = Join-Path $Root "openocd\interface\cmsis-dap.cfg"
        $target    = Join-Path $Root "openocd\target\at32f421xx.cfg"
        $proc = Start-Process $OpenOcdPath -ArgumentList @("-f", $interface, "-f", $target,
            "-c", "init; reset run; shutdown") -WorkingDirectory $Root -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $LogDir "reset.log") `
            -RedirectStandardError (Join-Path $LogDir "reset.err.log")
        return $proc.Id
    }
    elseif ($cmd -eq "reconnect_serial") {
        Ensure-SerialConnected -Force | Out-Null
        return 0   # 无进程可跟踪
    }
    elseif ($cmd -like "ota_*") {
        # OTA: 调驾驶舱内置上位机 tools/iap_host_tool/cli_flash.py（自包含, 固定 AT32 profile）
        #   【串口独占】OTA 用独立 python 进程独占串口, 服务端先释放自己的串口
        #   （Windows 不允许两进程同时打开同一 COM; 完成后 /api/serial 的
        #    Ensure-SerialConnected 会自动重连, 无需人工干预）
        Close-Serial
        $port = $Query["port"]
        if (-not $port) { $port = $script:SerialTargetPort }
        if (-not $port) { $port = Find-SerialPort }
        if (-not $port) { Write-Host "OTA: 未指定端口且未探测到串口"; return 0 }
        $py = (Get-Command python).Source
        $cli = Join-Path $PSScriptRoot "iap_host_tool\cli_flash.py"
        $mode = $cmd -replace "^ota_", ""     # upgrade / query / reset
        $otaArgs = @($cli, "--profile", "AT32", "--mode", $mode, "--port", $port)
        if ($mode -eq "upgrade") {
            $fw = Join-Path $Root "build\Debug\AT32F421G8U7_WorkBench.bin"
            $otaArgs += @("--firmware", $fw, "--listen", "10", "--reset-dir", $Root)
            $th = $Query["throttle"]; if ($th) { $otaArgs += @("--throttle", $th) }
            $cr = $Query["corrupt"];  if ($cr) { $otaArgs += @("--corrupt-frame", $cr) }
        }
        $proc = Start-Process $py -ArgumentList $otaArgs -WorkingDirectory $Root `
            -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $LogDir "ota.log") `
            -RedirectStandardError (Join-Path $LogDir "ota.err.log")
        Write-Host "OTA $mode -> $port (pid $($proc.Id))"
        return $proc.Id
    }
    return 0
}

# ============================================================
# 状态
# ============================================================
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
    $serialAge = if ($script:LastSerialData -eq [DateTime]::MinValue) { -1 }
                 else { [int]((Get-Date) - $script:LastSerialData).TotalMilliseconds }
    return @{
        serverTime = (Get-Date).ToString("HH:mm:ss")
        processes  = @{ openocd = $openocd; gdb = $gdb }
        elf        = $elf
        logs       = $logs
        heartbeat  = $heartbeat
        running    = @(Get-RunningCmds)
        serial     = @{
            connected    = $script:SerialConnected
            port         = $script:SerialTargetPort
            targetPort   = $SerialPort
            err          = $script:SerialErr
            lastDataAgeMs = $serialAge
        }
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
                # 1) 透传卡死检测: 已连但 >15s 无数据(心跳 1s/条) → 强制重连
                if ($script:SerialConnected -and $script:LastSerialData -ne [DateTime]::MinValue -and
                    ((Get-Date) - $script:LastSerialData).TotalSeconds -gt 15) {
                    Write-Host "串口 15s 无数据(透传可能卡死) → 强制重连"
                    Close-Serial
                }
                if (Ensure-SerialConnected) {
                    # 读新数据 → 拼残尾 → 按行入缓冲(上限 300 行) → 返回最近 60 行(前端覆盖式刷新)
                    $new = ""
                    try { $new = $script:Serial.ReadExisting() } catch {
                        # 读异常: 端口已失效 → 标记断开, 下次请求自动重连
                        Close-Serial
                        $new = ""
                    }
                    if ($new) {
                        $script:LastSerialData = Get-Date
                        $text = $script:SerialTail + $new
                        $parts = $text -split "`n"
                        $script:SerialTail = $parts[$parts.Count - 1]
                        for ($i = 0; $i -lt $parts.Count - 1; $i++) {
                            $l = $parts[$i].TrimEnd("`r")
                            # 过滤控制字符(NUL/0x7F 等非可打印字节): 防固件遥测/杂散字节污染面板
                            $l = $l -replace '[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]', ''
                            if ($l -ne "") { [void]$script:SerialBuf.Add($l) }
                        }
                        if ($script:SerialBuf.Count -gt 300) {
                            $script:SerialBuf.RemoveRange(0, $script:SerialBuf.Count - 300)
                        }
                    }
                    $lines = @($script:SerialBuf | Select-Object -Last 60)
                    Send-Json $ctx @{
                        live = $true; port = $script:SerialTargetPort; lines = $lines;
                        err = $script:SerialErr
                    }
                } else {
                    Send-Json $ctx @{ live = $false; lines = @(); port = $script:SerialTargetPort; err = $script:SerialErr }
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
            } elseif ($path -eq "/api/action") {
                $query = $ctx.Request.QueryString
                $cmd = $query["cmd"]
                if ($AllowedActions -notcontains $cmd) {
                    $ctx.Response.StatusCode = 400
                    $ctx.Response.StatusDescription = "unknown action"
                } else {
                    $running = @(Get-RunningCmds)
                    if ($cmd -ne "reconnect_serial" -and ($running -contains $cmd)) {
                        # 同类动作已在执行 → 拒绝(防重复触发多个编译/烧录)
                        Send-Json $ctx @{ ok = $false; busy = $true; cmd = $cmd; running = $running }
                    } else {
                    $procId = Start-CmdAction $cmd $query
                        # 强制 $script:Running 为哈希表(防御: 历史上曾出现被求值为数组/空值的运行时异常)
                        if ($null -eq $script:Running -or $script:Running -isnot [System.Collections.IDictionary]) {
                            $script:Running = @{}
                        }
                        $procIdInt = 0
                        $parsed = [int]::TryParse([string]$procId, [ref]$procIdInt)
                        if ($procIdInt -gt 0) { $script:Running[$cmd] = @{ pid = $procIdInt; start = (Get-Date) } }
                        $runNow = @(Get-RunningCmds)
                        $respObj = @{ ok = $true; cmd = $cmd; pid = $procId; running = $runNow }
                        Send-Json $ctx $respObj
                    }
                }
            } else {
                $ctx.Response.StatusCode = 404
            }
        } catch {
            # 错误落盘可诊断(服务是隐藏窗口, 控制台看不到)
            try {
                $errLine = "{0} [500] path={1} err={2}`r`n{3}" -f (Get-Date).ToString("HH:mm:ss"), $path, $_.Exception.Message, $_.ScriptStackTrace
                [System.IO.File]::AppendAllText((Join-Path $LogDir "server.err.log"), $errLine + "`r`n", [System.Text.Encoding]::UTF8)
            } catch { }
            $ctx.Response.StatusCode = 500
        } finally {
            $ctx.Response.Close()
        }
    }
} finally {
    $listener.Stop()
}
