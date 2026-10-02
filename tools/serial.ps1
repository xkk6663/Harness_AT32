# ============================================================
# serial.ps1 — 串口监视器（Step 7: 串口闭环观察）
#
# 用法: 在工程根目录运行
#   pwsh -File tools\serial.ps1                              # 自动探测串口
#   pwsh -File tools\serial.ps1 -Port COM9                   # 指定端口
#   可选: -Baud 115200  -Duration 8(秒; 0=一直读到 Ctrl+C)  -LogFile out.log
# ============================================================
param(
    [string]$Port = "",
    [int]$Baud = 115200,
    [int]$Duration = 0,
    [string]$LogFile = ""
)

$Project = Split-Path $PSScriptRoot -Parent
if ($LogFile -eq "") { $LogFile = Join-Path $Project "logs\serial.log" }
New-Item -ItemType Directory -Force (Split-Path $LogFile) | Out-Null
Write-Host "串口日志落盘: $LogFile"

function Find-SerialPort {
    $ports = @([System.IO.Ports.SerialPort]::GetPortNames())
    if ($ports.Count -eq 0) { return $null }

    $scores = @()
    foreach ($c in $ports) {
        $score = 0; $name = $c
        try {
            $dev = Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
                Where-Object { $_.Name -match "\($c\)" } | Select-Object -First 1
            if ($dev) {
                $name = $dev.Name
                if ($dev.PNPDeviceID -like 'BTHENUM*')  { $score = -10 }  # 蓝牙虚拟串口排除
                if ($name -match 'CP210x|FTDI|CH340|USB Serial') { $score = 10 }
                if ($name -match 'DAPLink|CMSIS-DAP') { $score = 12 }
                if ($name -match 'AT32|Artery')       { $score = 15 }
            }
        } catch { }
        $scores += [PSCustomObject]@{ Com = $c; Score = $score; Name = $name }
    }
    return ($scores | Sort-Object Score -Descending | Select-Object -First 1)
}

# ---- 端口解析 ----
if ($Port -eq "") {
    $found = Find-SerialPort
    if ($null -eq $found -or $null -eq $found.Com) {
        Write-Error "未发现串口。请 -Port 指定, 可用端口: $([System.IO.Ports.SerialPort]::GetPortNames() -join ', ')"
        exit 1
    }
    $Port = $found.Com
    Write-Host "自动探测串口: $Port ($($found.Name)) @ $Baud"
} else {
    Write-Host "打开串口: $Port @ $Baud"
}

# ---- 打开串口 ----
$sp = New-Object System.IO.Ports.SerialPort($Port, $Baud, 'None', 8, 'One')
$sp.ReadTimeout = 500
try { $sp.Open() } catch { Write-Error "打开 $Port 失败: $_"; exit 1 }
Write-Host "===== 串口监听开始 (Ctrl+C 退出) ====="

$start = Get-Date
try {
    while ($true) {
        if ($Duration -gt 0 -and ((Get-Date) - $start).TotalSeconds -ge $Duration) { break }
        $data = ""
        try { $data = $sp.ReadExisting() } catch { }
        if ($data) {
            $line = "[$((Get-Date).ToString('HH:mm:ss.fff'))] $data"
            Write-Host $line
            if ($LogFile) { Add-Content -Path $LogFile -Value $line -Encoding utf8 }
        }
        Start-Sleep -Milliseconds 50
    }
} finally {
    $sp.Close()
    Write-Host "===== 串口监听结束 ====="
}
