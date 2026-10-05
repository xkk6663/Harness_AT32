# AT32F421G8U7 BLDC 电调开发台架（Harness_AT32）

无刷直流电机（BLDC）电调开发台架，主控 **AT32F421G8U7**（Cortex-M4F，120MHz，64KB Flash，16KB SRAM）。
电机算法库：`SguanESC/`。图形化配置由 **AT32 Work Bench**（`.ATWP`）生成，其余全部是
**纯命令行开源工具链**：CMake + Ninja + GCC + OpenOCD + GDB，可脚本化、可进 CI、可被 AI 直接操作。

**OTA 能力已平台化移植**：IAP-Bootloader + APP 双区，上位机无 GUI 端到端升级（`cli_flash.py`），
从 STM32 老工程（STM32-OTA-QT）抽出的纯 C 核心库 `ota/core` 双平台共享。

---

## 一、核心能力

### 1. 全 CI 化（push 即构建）
`.github/workflows/build.yml`：每次 push / PR 自动在 Ubuntu 装 ARM 工具链 → 构建固件 →
**校验日志/崩溃符号**（`log_write`/`g_hardfault`/`HardFault_Handler` 必须在固件里）→ 上传 elf 产物。
本地同样一条命令复现：`cmake --preset Debug -DTOOLCHAIN_DIR= -DCMAKE_MAKE_PROGRAM=ninja && cmake --build build/Debug`。

### 2. 可接入 AI，零插件依赖
插件只是"按钮外壳"，背后就是几条命令——本工程把这些命令全部显式化、脚本化：
- 构建 `tools/build.ps1`、烧录 `tools/flash.ps1`、调试 `tools/debug.ps1`、串口 `tools/serial.ps1`
- 所有操作在终端可见、可记录、可回放 → AI（或 CI）可以端到端复现每一步，无需图形界面
- 调试后端 = 开源 cortex-debug + OpenOCD，配置全在 `.vscode/` 且已进 Git
- **可视化驾驶舱** `tools/dashboard.ps1`：编译/烧录/调试/串口日志四路输出实时汇到一个网页

### 3. 闭环调试能力（编译 → 烧录 → 日志 → 崩溃现场）
| 环节 | 工具 | 能力 |
|---|---|---|
| 编译 | `tools/build.ps1` | 一条命令，FLASH/RAM 占用、全部警告可见 |
| 烧录 | `tools/flash.ps1` | 校验 + 复位运行，换调试器只改 interface cfg |
| 串口反馈 | `tools/serial.ps1` + `log/` | 1s 一条心跳：tick + 高速环计数 + 三相/母线 ADC |
| 崩溃定位 | `crash/crash.c` | HardFault 现场（PC/LR/栈帧/CFSR/BFAR）记录 + 串口打印 + GDB 离线分析 |
| 调试 | `tools/debug.ps1` + GDB | 断点/观察点/寄存器/栈回溯/coredump |

### 4. OTA 升级（平台化移植，M0~M3 完成）
- **纯 C 核心库** `ota/core/`：protocol / crc32 / flash_store / upgrade_state / offset / transport，
  零芯片依赖，STM32 老工程与 AT32 新工程共享同一份代码（双构建哈希一致验证）
- **AT32 Bootloader**（`bootloader/` 独立 target，@0x08000000，18KB）+ **APP 重定位**
  （`AT32F421x8_APP.ld`，@0x08004800，44KB + `SCB->VTOR` + 停机保护链 `ota_app_hook`）
- **上位机双 profile**（`config.py`：AT32 / STM32）+ **无 GUI CLI**（`cli_flash.py`）：
  触发 → 逐帧发送（ACK/NAK 重传）→ CRC 校验 → 硬件复位 → 心跳验证，供 CI/AI 自动调用

---

## 二、架构总览

```
                        ┌────────────────────────────────────────────┐
  上位机(Windows)       │  AT32F421G8U7 芯片 Flash                   │
  ┌──────────────┐      │  ┌──────────────────────────────┐          │
  │ cli_flash.py │──┐   │  │ 0x08000000 Bootloader(18KB)  │          │
  │ (CLI/CI/AI)  │  │   │  │  - 状态机: READY→擦除/SUCCESS→ │          │
  │ config.py    │  │   │  │    跳APP/CRC_FAIL→等'!'重触发/ │          │
  │ (双profile)  │  │   │  │    UPGRADING→断电续传          │          │
  └──────────────┘  │   │  │  - 协议: AA+body+CRC32+55    │          │
    串口(UART1)     │   │  ├──────────────────────────────┤          │
  ┌──────────────┐  └───┼──┤ 0x08004800 APP(44KB)         │          │
  │ 驾驶舱        │      │   │  - BLDC 电机库 SguanESC      │          │
  │ dashboard.ps1│      │   │  - ota_app_hook: '!!!!!'扫描 │          │
  └──────────────┘      │   │    → 停机保护链 → READY      │          │
    串口(UART1)         │  ├──────────────────────────────┤          │
  ┌──────────────┐      │   │ 0x0800F800 OFFSET 页(62)    │          │
  │ OpenOCD+GDB  │──SWD─┼───┤ 0x0800FC00 STATE 页(63)     │          │
  └──────────────┘      │  └──────────────────────────────┘          │
                        └────────────────────────────────────────────┘
  工具链: CMake+Ninja+GCC 13.3.1  |  CI: GitHub Actions(Ubuntu)
  驱动: DAPLink(HID后端)          |  脚本: pwsh7 (统一)
```

---

## 三、目录结构

```
AT32F421G8U7_WorkBench/
├── CMakeLists.txt                # 工程总菜单（APP 源 + bootloader + ota 核心）
├── CMakePresets.json             # 一键参数包（Ninja + toolchain + TOOLCHAIN_DIR + CMAKE_MAKE_PROGRAM）
├── AT32F421x8_APP.ld             # APP 链接脚本（FLASH 0x08004800 / 44KB）
├── cmake/
│   ├── gcc-arm-none-eabi.cmake   # 交叉编译工具链（显式锁编译器 + PATH 回退）
│   └── at32_workbench/           # BSP 子工程（生成代码 + 驱动库）
├── bootloader/                   # 【OTA】Bootloader 独立 target（@0x08000000，18KB）
│   ├── boot.c                    #   状态机 + 协议主循环 + 升级/续传/CRC_FAIL 分支
│   ├── AT32F421x8_BOOT.ld        #   Boot 链接脚本（叠在基础 FLASH.ld 上，REPLACE -T）
│   └── CMakeLists.txt            #   双 -T 处理（string REPLACE 去掉 APP.ld）
├── ota/                          # 【OTA】平台化核心 + 移植层
│   ├── core/                     #   纯 C 核心库（protocol/crc32/flash_store/upgrade_state/offset/transport）
│   └── port/at32/                #   AT32 移植层（flash_hal/usart_hal/delay/printf/jump/ota_app_hook）
├── project/                      # ATWP 生成代码（main.c、wk_*.c）+ 用户钩子区
│   └── src/
│       ├── main.c                # VTOR 重定位 + __enable_irq + OtaAppHook_HandleTrigger
│       └── at32f421_int.c        # USART1 IDLE + DMA 接收（OTA 触发扫描）
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
├── logs/                         # 工具链输出日志 + 回归/诊断脚本（git 忽略）
├── docs/                         # 方案 / 学习路线 / 踩坑指南 / 经验总结 / 教程
├── .github/workflows/build.yml   # CI：push 自动构建 + 符号校验 + 产物上传
├── startup_at32f421.s            # 启动文件
└── AT32F421x8_FLASH.ld           # 基础链接脚本（Boot 叠 BOOT.ld，APP 叠 APP.ld）
```

---

## 四、快速开始

```powershell
# 0. 前置：pwsh 7 + DAPLink 已插（COM10 示例；串口/烧录异常先查 openocd 残留进程）

# 1. 构建（等价于插件"编译"）
pwsh -File tools\build.ps1

# 2. 烧录（单固件：APP 或 Boot，见 tools/flash.ps1 -Help；双固件一键见下）
pwsh -File tools\flash.ps1

# 3. 串口观察（心跳日志：tick / 高速环计数 / ADC 采样，Ctrl+C 退出）
pwsh -File tools\serial.ps1 -Port COM10          # 或 -Port 自动探测

# 4. GDB 命令行调试（终端1: server；终端2: gdb 连 :3333）
pwsh -File tools\debug.ps1
arm-none-eabi-gdb build/Debug/AT32F421G8U7_WorkBench.elf
(gdb) target remote :3333

# 5. 可视化驾驶舱（四路日志实时面板，自动开浏览器）
pwsh -File tools\dashboard.ps1                    # → http://127.0.0.1:8080
```

### 双固件烧录（Boot + APP + 状态页）
```powershell
# 完整恢复/首刷：Boot@0x08000000 + APP@0x08004800 + RUNNING 状态页@0x0800FC00
python logs\flash_restore.py
# 等价手动命令（路径必须正斜杠，反斜杠被 openocd 当转义符吞掉）：
openocd -s . -f openocd/interface/cmsis-dap.cfg -f openocd/target/at32f421xx.cfg \
        -c "program build/Debug/bootloader/at32f421_boot.bin 0x08000000 verify exit"
openocd -s . -f openocd/interface/cmsis-dap.cfg -f openocd/target/at32f421xx.cfg \
        -c "program build/Debug/AT32F421G8U7_WorkBench.bin 0x08004800 verify exit"
```

---

## 五、OTA 升级子系统（使用说明）

### 状态机与地址表（`ota/core/upgrade_state.h`、`ota_common.h`）
| 项 | 值 |
|---|---|
| FLASH_BASE_ADDR | 0x08000000 |
| BOOTLOADER_SIZE | 0x4800（18KB，0x08000000~0x080047FF） |
| APP_START_ADDRESS | 0x08004800（44KB，0x08004800~0x0800F7FF） |
| OFFSET_PAGE_ADDR | 0x0800F800（页 62，断电续传已写页数） |
| UPGRADE_STATE_ADDR | 0x0800FC00（页 63，状态字） |
| APP_PAGE_COUNT | 44 / PACKET_SIZE 128 / DMA_RX_BUF 1024 |
| 状态码 | RUNNING=0xA5A5A5A0 / READY=A1 / UPGRADING=A2 / CRC_FAIL=A3 / SUCCESS=A4 |

### 协议帧
`SOF 0xAA + TYPE(0x01数据/0x02命令) + [CMD/LEN] + PAYLOAD + CRC32(LE, poly 0xEDB88320) + EOF 0x55`
- 数据帧：TYPE_DATA + len + 载荷 + CRC32 + 55；满 128 字节写一页 Flash
- 命令帧：`QUERY_OFFSET(0x12) / RESET_UPGRADE(0x13) / QUERY_STATE(0x14)`
- 应答：`ACK(0x20){status,seq,pages} / NAK(0x21) / OFFSET(0x22) / STATE(0x23)`

### 端到端升级（CLI，供 CI/AI 自动化）
```powershell
# 前置：config.py 的 APP_PROFILE（默认 STM32，验证 AT32 时改 "AT32"）
cd C:\Users\xiao1\Desktop\STM32-OTA-QT\iap_host_tool
python cli_flash.py --port COM10 \
    --firmware C:\Users\xiao1\Desktop\AT32\AT32F421G8U7_WorkBench\build\Debug\AT32F421G8U7_WorkBench.bin \
    --listen 10 --reset-dir C:\Users\xiao1\Desktop\AT32\AT32F421G8U7_WorkBench
# 流程: 触发('!!!!!'x3 AT32/'!'x10 STM32) → offset → 逐帧(ACK/NAK重传) →
#       Boot 'Upgrade complete. Final CRC: 0x...' → openocd 硬件复位 → APP 心跳 ≥2 → PASS
# 回归参数: --throttle 0.02(限制发送) / --corrupt-frame 100(CRC 破坏→NAK→重传)
```

### M4 全功能回归（部分完成，状态页注入测试法）
```powershell
python logs\m4_regression.py   # T1 查询重置 / T2 断电续传 / T3 CRC破坏 / T4 CRC_FAIL回退 / T5 限制发送
# 已完成: T3 ✅（帧101破坏→NAK→重传→升级完成 CRC 0x9EE5BE44）、T5 ✅（慢速升级）
# 待续: T1/T2/T4（被 DAPLink CDC 假死 + 重插后 Flash 非本工程固件打断，见 docs/踩坑指南.md §31）
```

### Boot 三个反直觉行为（踩坑预警）
1. **置 SUCCESS 后 `while(1)` 死循环**：防串口噪声冲写，命令帧不再响应 → 判定升级完成靠
   监听串口 `Upgrade complete. Final CRC: 0x...` 打印 + **硬件复位**（openocd `reset run`）
2. **固件最后帧必须截断到实际大小**：上位机按 APP_SIZE 填充 0xFF 打包时，最后帧取满 128 字节
   → Boot 的"最后帧不满 128 自动结束"永不触发 → 状态停留 UPGRADING
3. **CRC_FAIL 分支 `while(1)` 等 `'!'` 重触发**：不响应命令帧，重触发靠发 `'!!!!!'`

---

## 六、驾驶舱（可选，纯本地）

`tools/dashboard.ps1` 一键起服务（`127.0.0.1:8080`）+ 打开浏览器：
- **四个实时面板**：编译 / 烧录 / 调试 / 串口日志，数据来自 `logs/` 下各脚本的落盘输出（2.5s 轮询）
- **全局状态栏**：openocd / gdb 进程占用、最新心跳（tick / high_loop / ADC）、elf 大小与时间
- 每个面板带操作按钮（重编译 / 重烧录 / 刷新串口等）
- 停止服务：`Get-CimInstance Win32_Process -Filter "Name='pwsh.exe'" | Where-Object { $_.CommandLine -like '*dashboard_server*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }`

---

## 七、闭环演示（崩溃定位，插件时代做不到）

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

---

## 八、CI 说明

- 触发：`push` / `pull_request` 到任意分支
- 流程：Ubuntu → `gcc-arm-none-eabi + cmake + ninja` →
  `cmake --preset Debug -DTOOLCHAIN_DIR= -DCMAKE_MAKE_PROGRAM=ninja`（走 PATH 分支 + 覆盖本机 ninja 路径）→
  build → **符号校验** → upload elf
- **本机 vs CI 差异**：本机 `CMakePresets.json` 硬编码 `TOOLCHAIN_DIR`（GNU-tools-for-STM32）与
  `CMAKE_MAKE_PROGRAM`（at32-tools ninja.exe）保证可复现；CI 用命令行 `-DTOOLCHAIN_DIR= -DCMAKE_MAKE_PROGRAM=ninja`
  **覆盖**（runner 的 apt 工具链 + PATH ninja）——两条命令都能复现同一份固件
- 本地模拟 CI 环境：`cmake --preset Debug -DTOOLCHAIN_DIR= -DCMAKE_MAKE_PROGRAM=ninja && cmake --build build/Debug`

---

## 九、学习文档（重要）

| 文档 | 内容 |
|---|---|
| `docs/STM32-OTA平台化移植到AT32-BLDC可行性方案.md` | OTA 平台化 v1.1 方案（M0~M5 里程碑 + 四决策 + 协议/地址/验收） |
| `docs/学习路线.md` | 从插件到命令行工具链的 commit 式迁移路线（Step 0~6 + 设计决策） |
| `docs/踩坑指南.md` | 真实踩坑记录（§1~§31：编码/调试器接口/串口链路/openocd 占用/CDC 假死/OTA 三坑...） |
| `docs/经验总结.md` | 认知升级与收益清单（含 OTA 平台化九条认知） |
| `docs/cortex-debug教程.md` | F5 调试完整教程（快捷键/外设视图/排障） |
| `docs/边缘管理中控平台-技术选型.md` | Qt 上位机 + 驾驶舱合并的中控平台技术选型 |

---

## 十、里程碑进度（OTA 平台化 M0~M5）

| 里程碑 | 状态 |
|---|---|
| M0 核心库抽取（ota/core 纯 C 双平台共享） | ✅ 已推（196a7d2） |
| M1 AT32 Bootloader（7/7 验收） | ✅ 已推（c3b410a） |
| M2 APP 集成（VTOR + 停机保护链） | ✅ 已推（791482a） |
| M3 端到端升级（双 profile + CLI） | ✅ 已推（738d2a4） |
| M4 全功能回归（T3/T5 PASS，T1/T2/T4 待环境稳定续跑） | 🔶 部分 |
| M5 工具链/CI 整合（build.ps1 双固件 + 体积断言） | ⬜ 未开始 |

**环境提醒**：DAPLink CDC 假死（openocd 连续连接后串口 0 字节）→ 等 5s + 重开串口恢复；
重插后若串口出现未知 `[alive]` 打印 → 重烧 Boot+APP（`logs\flash_restore.py`）锚定固件身份。
