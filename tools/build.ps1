# ============================================================
# build.ps1 — 命令行构建（Step 1 的固化入口）
#
# 用法: 在工程根目录运行
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1
#   可选: -Preset Debug|Release （默认 Debug）
#
# 等价于插件"编译"按钮: cmake --preset <preset> && cmake --build build/<preset>
# ============================================================
param(
    [string]$Preset = "Debug"
)

$Project = Split-Path $PSScriptRoot -Parent

# 输出落盘（dashboard 驾驶舱读取）
$LogPath = Join-Path $Project "logs\build.log"
New-Item -ItemType Directory -Force (Split-Path $LogPath) | Out-Null
Start-Transcript -Path $LogPath -Append -Force | Out-Null

# 确保 at32-tools 的 cmake/ninja 可用（与插件同款，跨机器行为一致）
$env:PATH = "$env:LOCALAPPDATA\at32-tools\cmake\V3.28.1\bin;$env:LOCALAPPDATA\at32-tools\ninja\V1.11.1;" + $env:PATH

Push-Location $Project
try {
    Write-Host "===== [1/2] 配置: cmake --preset $Preset ====="
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败 (exit=$LASTEXITCODE)" }

    Write-Host "===== [2/2] 构建: cmake --build build/$Preset ====="
    cmake --build build/$Preset
    if ($LASTEXITCODE -ne 0) { throw "构建失败 (exit=$LASTEXITCODE)" }

    Write-Host "===== 构建成功: build/$Preset/AT32F421G8U7_WorkBench.elf ====="
} finally {
    Pop-Location
    Stop-Transcript | Out-Null
}
