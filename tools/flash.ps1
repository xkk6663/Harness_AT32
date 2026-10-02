# ============================================================
# flash.ps1 — OpenOCD 命令行烧录脚本（Step 2）
#
# 用法: 在工程根目录运行
#   powershell -ExecutionPolicy Bypass -File tools\flash.ps1
#   可选: -Elf <路径> 指定其他固件
#
# 自动检测固件: build/Debug/ 优先, 回退 build/（兼容插件旧构建目录）
# ============================================================
param(
    [string]$Elf = ""
)

$OpenOcd = "$env:LOCALAPPDATA\at32-tools\OpenOCD\V2.0.9\bin\openocd.exe"
$Project = Split-Path $PSScriptRoot -Parent

# 输出落盘（dashboard 驾驶舱读取）
$LogPath = Join-Path $Project "logs\flash.log"
New-Item -ItemType Directory -Force (Split-Path $LogPath) | Out-Null
Start-Transcript -Path $LogPath -Append -Force | Out-Null

try {
# ---- 自动检测固件 ----
$candidates = @(
    "build/Debug/AT32F421G8U7_WorkBench.elf",
    "build/AT32F421G8U7_WorkBench.elf"
)
if ($Elf -eq "") {
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $Project $c)) { $Elf = (Join-Path $Project $c); break }
    }
}

# ---- 前置检查 ----
if (-not (Test-Path $OpenOcd)) { Write-Error "找不到 OpenOCD: $OpenOcd（请确认 at32 插件已安装）"; exit 1 }
if (-not $Elf -or -not (Test-Path $Elf)) {
    Write-Error "找不到固件 elf。先运行: cmake --preset Debug && cmake --build build/Debug"
    exit 1
}

# ---- OpenOCD 配置 ----
$Interface = Join-Path $Project "openocd\interface\cmsis-dap.cfg"
$Target    = Join-Path $Project "openocd\target\at32f421xx.cfg"

Write-Host "===== 烧录: $Elf ====="
# TCL 命令字符串里反斜杠是转义符，Windows 路径必须转成正斜杠
$ElfOcd = $Elf -replace '\\', '/'
& $OpenOcd -f $Interface -f $Target -c "program `"$ElfOcd`" verify reset exit"

if ($LASTEXITCODE -eq 0) {
    Write-Host "===== 烧录成功（已校验 + 复位运行）====="
} else {
    Write-Host "===== 烧录失败 (exit=$LASTEXITCODE) ====="
    exit $LASTEXITCODE
}
} finally {
    Stop-Transcript | Out-Null
}