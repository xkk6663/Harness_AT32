# AT32F421G8U7 BLDC 电调工程（Harness）

无刷直流电机（BLDC）电调，主控 **AT32F421G8U7**（Cortex-M4F，120MHz，64KB Flash，16KB SRAM）。
电机算法库：`SguanESC/`。图形化配置由 **AT32 Work Bench**（`.ATWP`）生成，其余全部是
**纯命令行开源工具链**：CMake + Ninja + GCC + OpenOCD + GDB，可脚本化、可进 CI、可被 AI 直接操作。

## 核心能力

### 1. 全 CI 化（push 即构建）
`.github/workflows/build.yml`：每次 push / PR 自动在 Ubuntu 装 ARM 工具链 → 构建固件 →
**校验日志/崩溃符号**（`log_write`/`g_hardfault`/`HardFault_Handler` 必须在固件里）→ 上传 elf 产物。
本地同样一条命令复现：`cmake --preset Debug -DTOOLCHAIN_DIR= && cmake --build build/Debug`。

### 2. 可接入 AI，零插件依赖
插件只是"按钮外壳"，背后就是几条命令——本工程把这些命令全部显式化、脚本化：
- 构建 `tools/build.ps1`、烧录 `tools/flash.ps1`、调试 `tools/debug.ps1`、串口 `tools/serial.ps1`
- 所有操作在终端可见、可记录、可回放 → AI（或 CI）可以端到端复现每一步，无需图形界面
- 调试后端 = 开源 cortex-debug + OpenOCD，配置全在 `.vscode/` 且已进 Git
- **可视化驾驶舱** `tools/dashboard.ps1`：编译/烧录/调试/串口日志四路输出实时汇到一个网页，开发过程一目了然

### 3. 闭环调试能力（编译 → 烧录 → 日志 → 崩溃现场）
| 环节 | 工具 | 能力 |
|---|---|---|
| 编译 | `tools/build.ps1` | 一条命令，FLASH/RAM 占用、全部警告可见 |
| 烧录 | `tools/flash.ps1` | 校验 + 复位运行，换调试器只改 interface cfg |
| 串口反馈 | `tools/serial.ps1` + `log/` | 1s 一条心跳：tick + 高速环计数 + 三相/母线 ADC |
| 崩溃定位 | `crash/crash.c` | HardFault 现场（PC/LR/栈帧/CFSR/BFAR）记录 + 串口打印 + GDB 离线分析 |
| 调试 | `tools/debug.ps1` + GDB | 断点/观察点/寄存器/栈回溯/coredump |

## 目录结构

```
AT32F421G8U7_WorkBench/
├── CMakeLists.txt                # 工程总菜单（用户源码挂载点）
├── CMakePresets.json             # 一键参数包（Ninja + toolchain + TOOLCHAIN_DIR）
├── cmake/
│   ├── gcc-arm-none-eabi.cmake   # 交叉编译工具链（显式锁编译器 + PATH 回退）
│   └── at32_workbench/           # BSP 子工程（生成代码 + 驱动库）
├── project/                      # ATWP 生成代码（main.c、wk_*.c）+ 用户钩子区
├── libraries/                    # CMSIS + AT32 标准外设库
├── SguanESC/                     # 电机库（BLDC 算法，勿动）
├── Hardware/                     # 硬件层（Timer.c）
├── crash/crash.c                 # 崩溃现场记录器（naked 强符号接管 4 个 fault handler）
├── log/log.c + log.h             # 串口日志模块（等级裁剪 + 周期日志 + 时间戳）
├── openocd/
│   ├── interface/cmsis-dap.cfg   # 调试器配置（HID 后端 + cmsis_dap_backend hid）
│   └── target/at32f421xx.cfg     # 芯片配置（SWD / Cortex-M4）
├── svd/AT32F421xx_v2.svd         # 外设寄存器描述（调试用）
├── tools/
│   ├── build.ps1                 # 构建入口（输出落盘 logs/build.log）
│   ├── flash.ps1                 # 烧录入口（输出落盘 logs/flash.log）
│   ├── debug.ps1                 # OpenOCD GDB server 入口（输出落盘 logs/debug.log）
│   ├── serial.ps1                # 串口监视器（自动探测 + 时间戳 + 默认落盘 logs/serial.log）
│   ├── dashboard.ps1             # 驾驶舱启动器（起服务 + 开浏览器）
│   ├── dashboard_server.ps1      # 驾驶舱数据服务（HttpListener，127.0.0.1 安全绑定）
│   └── dashboard.html            # 驾驶舱前端（四路日志实时面板 + 全局状态）
├── logs/                         # 工具链输出日志（git 忽略，dashboard 数据源）
├── docs/                         # 学习路线 / 踩坑指南 / 经验总结 / cortex-debug 教程
├── .github/workflows/build.yml   # CI：push 自动构建 + 符号校验 + 产物上传
├── startup_at32f421.s            # 启动文件
└── AT32F421x8_FLASH.ld           # 链接脚本
```

## 快速开始

```powershell
# 1. 构建（等价于插件"编译"）
pwsh -File tools\build.ps1

# 2. 烧录（等价于插件"烧录"，已校验 + 复位运行）
pwsh -File tools\flash.ps1

# 3. 串口观察（心跳日志：tick / 高速环计数 / ADC 采样，Ctrl+C 退出）
pwsh -File tools\serial.ps1 -Port COM10          # 或 -Port 自动探测

# 4. GDB 命令行调试（终端1: server；终端2: gdb 连 :3333）
pwsh -File tools\debug.ps1
arm-none-eabi-gdb build/Debug/AT32F421G8U7_WorkBench.elf
(gdb) target remote :3333

# 5. 可视化驾驶舱（四路日志实时面板，自动开浏览器）
pwsh -File tools\dashboard.ps1
```

## 驾驶舱（可选，纯本地）

`tools/dashboard.ps1` 一键起服务（`127.0.0.1:8080`）+ 打开浏览器：
- **四个实时面板**：编译 / 烧录 / 调试 / 串口日志，数据来自 `logs/` 下各脚本的落盘输出（2.5s 轮询）
- **全局状态栏**：openocd / gdb 进程占用、最新心跳（tick / high_loop / ADC）、elf 大小与时间
- 服务离线时面板自动降级为空态提示；关服务后所有按钮失效为纯展示
- 停止服务：`Get-CimInstance Win32_Process -Filter "Name='pwsh.exe'" | Where-Object { $_.CommandLine -like '*dashboard_server*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }`

前置要求：AT32 插件已安装（提供 `%LOCALAPPDATA%\at32-tools` 的 cmake/ninja/OpenOCD），
`arm-none-eabi-gcc` 在 PATH 或 `TOOLCHAIN_DIR` 指定（本机 `C:\DevEnv\GNU-tools-for-STM32`）。

## 闭环演示（崩溃定位，插件时代做不到）

```powershell
# 终端1: 起 GDB server
pwsh -File tools\debug.ps1

# 终端2: 故意触发一次 BusFault（PC 指向地址 0 → 指令总线错误）
arm-none-eabi-gdb build/Debug/AT32F421G8U7_WorkBench.elf
(gdb) target remote :3333
(gdb) monitor reset halt
(gdb) set $pc = 0
(gdb) continue

# 串口同时打出崩溃现场：
#   *** CRASH *** pc=0xFFFFFFFE lr=... cfsr=0x1 hfsr=0x40000000 ...
# GDB 里离线分析：
(gdb) p g_hardfault          # 现场结构体（PC/LR/8 寄存器/CFSR/HFSR/MMFAR/BFAR/SP）
(gdb) bt                     # 栈回溯定位崩溃函数
```

## CI 说明

- 触发：`push` / `pull_request` 到任意分支
- 流程：Ubuntu → `gcc-arm-none-eabi + cmake + ninja` → `cmake --preset Debug -DTOOLCHAIN_DIR=`（走 PATH 分支）→ build → **符号校验** → upload elf
- 本地模拟 CI 环境：`cmake --preset Debug -DTOOLCHAIN_DIR= && cmake --build build/Debug`

## 学习文档（重要）

| 文档 | 内容 |
|---|---|
| `docs/学习路线.md` | 从插件到命令行工具链的 commit 式迁移路线（Step 0~6 + 设计决策） |
| `docs/踩坑指南.md` | 真实踩坑记录（编码/调试器接口/串口链路/openocd 占用...） |
| `docs/经验总结.md` | 认知升级与收益清单 |
| `docs/cortex-debug教程.md` | F5 调试完整教程（快捷键/外设视图/排障） |

## 状态

- [x] Step 0~6：构建 / 烧录 / GDB / cortex-debug / 环境统一 / coredump+CI 全完成
- [x] Step 7：串口日志闭环（log 模块 + 心跳 + 崩溃串口输出 + serial.ps1）+ pwsh 统一
- [x] Step 8：可视化驾驶舱（四路日志落盘 logs/ + dashboard 服务 + 实时面板）
- [x] 全链路脚本化、CI 化、可被 AI 端到端操作
