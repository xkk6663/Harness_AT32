# STM32 OTA 平台化移植到 AT32 BLDC 电调 — 技术可行性方案

> 版本：v1.1 ｜ 日期：2026-10-04
> 范围：将 `C:\Users\xiao1\Desktop\STM32-OTA-QT`（STM32F103C8 IAP 串口升级工程）**功能零阉割**地平台化移植到 `AT32F421G8U7_WorkBench`（AT32F421G8U7 BLDC 电调工程），本文只讨论可行性、架构与实施路线，不含代码实现。
> **v1.1 更新**：确认四项现场决策——① Bootloader **不保留按键**，纯软件请求触发 OTA；② 上位机配置化采用 **`config.py` 双 profile**；③ Bootloader **保留打印**（printf）；④ 升级期间**不要求电机硬件断电**，软件停机保护链为唯一保障。正文已按决策同步修订。

---

## 0. 结论先行

**结论：完全可行，工程量可控。** 三个核心理由：

1. **源工程天生就是"可平台化"的**：STM32-OTA-QT 已做了传输层抽象（`Transport` 5 函数接口）、协议层独立（`protocol.c` 纯 C 状态机）、CRC32 纯软件实现、升级状态/断电续传逻辑只依赖极薄的 Flash 原语（`FLASH_Unlock / ErasePage / ProgramWord`）。这些核心模块**零硬件依赖**，可直接原样复用。
2. **芯片层面高度同构**：AT32F421G8U7（Cortex-M4F，64KB Flash / 16KB RAM）与 STM32F103C8（Cortex-M3，64KB Flash / 20KB RAM）同为 Cortex-M 系、同为 1KB 粒度的页式 Flash（待实测确认，见 §5.1），**原 Flash 布局可以 1:1 镜像**，上位机除两处常量外无需改动。
3. **容量预算充足**：BLDC 固件当前实测占用约 **35.8KB / 64KB Flash、3.3KB / 16KB RAM**（见 §1.2），移植 OTA 挂钩预计仅增加 1~2KB，44KB 的 APP 区余量约 8KB，RAM 余量约 12.7KB。

**两大关键风险点**（都有明确对策，见 §5）：
- AT32F421 Flash 扇区粒度需上板实测（预期 1KB）；
- APP 触发升级时必须先**停机、关 PWM**再复位，防止电机带电失控（这是 BLDC 场景独有的新增安全要求）。

---

## 1. 项目现状盘点

### 1.1 源项目 STM32-OTA-QT（被移植方）

| 项目 | 内容 |
|---|---|
| 芯片 | STM32F103C8（Cortex-M3，72MHz，64KB Flash，20KB RAM） |
| 固件 | `IAP-Bootloader/`（18KB）+ `APP-LED闪烁/`（44KB，LED 演示 APP） |
| 上位机 | `iap_host_tool/`（PyQt6，跨平台，支持 BIN/HEX） |
| 构建 | CMake + Ninja + ARM GCC（bash / PowerShell / cmd 三套脚本） |
| 通信 | USART1 115200 8N1，DMA 循环缓冲接收 |

**Flash 布局（现役）**：

| 区域 | 地址 | 大小 | 说明 |
|---|---|---|---|
| Bootloader | `0x08000000` | 18KB (0x4800) | 协议解析、Flash I/O、跳转 |
| APP | `0x08004800` | 44KB (0xB000) | 用户应用 |
| 偏移记录页 | `0x0800F800` | 1KB（第 62 页） | 断电续传断点（每 1024B 存一次） |
| 升级状态页 | `0x0800FC00` | 1KB（第 63 页） | 5 状态机 + 256 槽磨损均衡 |

**功能特性清单**（移植时逐项核对，见 §7）：
- 自定义串口协议：SOF/EOF 帧界定 + 长度字段解析（免疫 `0x55` 冲突）；
- CRC32 帧级校验 + CRC 错误自动重传（≤3 次，带 SEQ 序号）；
- 断电续传：每写满 1024B 保存偏移至专用页，上电自动恢复运行 CRC、擦除剩余页、续传；
- 双重 CRC：帧级 CRC32 + 固件级运行 CRC 端到端校验；
- 升级状态机：`RUNNING / UPGRADE_READY / UPGRADING / CRC_FAIL / UPGRADE_SUCCESS`，slot 磨损均衡（256×4B/页，擦写寿命 ×256）；
- 2 秒等待窗口 + 纯软件串口触发（`!` 5 连 / 协议命令，v1.1：不保留按键）；APP 端 5 连 `!` → 写状态 → 复位；
- 传输层抽象：`Transport` 5 函数指针接口，可切换 USART/CAN/SPI；
- 跨平台 PyQt6 上位机：固件拖放、实时进度、TX/RX 包着色、百分比限制发送、CRC 破坏模拟（测试重传）、查询/重置/续传。

### 1.2 目标工程 AT32F421G8U7_WorkBench（移植落点）

| 项目 | 内容 |
|---|---|
| 芯片 | AT32F421G8U7（Cortex-M4F，120MHz，64KB Flash，16KB SRAM） |
| 电机库 | `SguanESC/`（BLDC 六步换相：SWPWM、PID、PLL、LPF、状态机，勿动） |
| 构建 | CMake + Ninja + GCC + OpenOCD + GDB，`tools/*.ps1` 脚本化，GitHub CI |
| 日志/崩溃 | `log/log.c`（串口心跳）+ `crash/crash.c`（HardFault 现场） |

**外设占用现状**（从 WorkBench 生成代码 + 电机库实测）：

| 外设 | 用途 | 关键事实 |
|---|---|---|
| TMR1（高级定时器） | 六步换相互补 PWM | CH1~CH3 + CH1C~CH3C 共 6 路互补输出，周期 `2999`@120MHz ≈ 40kHz 载波；OVF 中断内跑 `SguanESC_High_Loop / Low_Loop` |
| TMR15 | 555 PWM 调速输入捕获 | PA2 = TMR15_CH1，零中断，40kHz 快照 |
| USART1 | 串口日志 + 电机遥测 | PB6/PB7，115200 8N1；DMA1 CH2=TX、CH3=RX（IDLE 空闲中断 + DMA，进 `SguanESC_Printf_Loop`） |
| ADC1 | 三相电压 + 母线电压 | 注入组 4 通道，TMR1 OVF 触发 |
| SysTick | 时基 `wk_timebase` / `wk_delay_ms` | 主循环心跳用 |
| DMA1 | CH2 TX / CH3 RX | 已配置 |

**当前固件体积（实测，`arm-none-eabi-size` on build/Debug）**：

```
text    data     bss     dec
36076    560    2776   39412
```

→ Flash 占用 ≈ 36076 + 560 = **36,636B ≈ 35.8KB**；RAM 占用 ≈ 560 + 2776 = **3,336B ≈ 3.3KB**。

### 1.3 关键差异对照表

| 维度 | STM32F103C8 | AT32F421G8U7 | 移植影响 |
|---|---|---|---|
| 内核 | Cortex-M3 | Cortex-M4F | 中断/向量表/跳转机制相同；APP 用 `SCB->VTOR` 重定位（M4 原生支持） |
| 主频 | 72MHz | 120MHz | CRC32 等纯软件计算更快，无性能瓶颈 |
| Flash | 64KB（1KB 页） | 64KB（扇区擦除，粒度预期 1KB） | 布局可镜像；**扇区粒度需上板实测** |
| RAM | 20KB | 16KB | 预算仍充足（见 §3） |
| 外设库 | STM32 StdPeriph | AT32 FW Lib（`at32f421_*`） | 仅芯片层 HAL 需要替换（映射表见 §4.2） |
| Flash 原语 | `FLASH_Unlock/ErasePage/ProgramWord` | `flash_unlock / flash_sector_erase / flash_word_program` | 薄封装，1:1 映射 |
| 时钟复位 | `RCC_DeInit()` | 无等价一键 API，需逐个 `crm_periph_clock_enable(…, FALSE)` | 跳转前外设清理逻辑需重写 |
| 向量表 API | `NVIC_SetVectorTable()` | 直接写 `SCB->VTOR` | 一行差异 |
| 构建 | CMake（每固件独立子目录） | CMake（单 target） | 需增加 Bootloader target 与双链接脚本 |
| 上位机 | PyQt6 | PyQt6（复用） | 仅 `protocol.py` / `firmware.py` 两处硬编码常量参数化 |

---

## 2. "平台化"的目标定义与总体架构

### 2.1 平台化目标

"平台化"在这里定义为：**把 OTA 的能力抽成"核心逻辑（与芯片无关）+ 移植层（每芯片一份）"，一次移植、多平台复用。**

- 核心逻辑（协议、CRC、状态机、续传、传输接口）只写一份，STM32 与 AT32 共用；
- 芯片差异全部收敛到 `port/` 目录；
- STM32 老工程保留为参考实现（行为基准），后续可选切换到共享核心。

### 2.2 目标目录结构（建议）

```
AT32F421G8U7_WorkBench/
├── ota/                            # ★ 新增：OTA 平台化核心（本方案的落点）
│   ├── core/                       # 平台无关层（纯 C，零芯片头文件依赖）
│   │   ├── ota_protocol.c/.h       # ← 源 protocol.c/h（帧状态机）
│   │   ├── ota_crc32.c/.h          # ← 源 crc32.c/h
│   │   ├── ota_flash_store.c/.h    # ← 源 flash_store.c/h（slot 磨损均衡）
│   │   ├── ota_upgrade_state.c/.h  # ← 源 upgrade_state.c/h
│   │   ├── ota_offset.c/.h         # ← 源 offset.c/h
│   │   └── ota_transport.c/.h      # ← 源 transport.c/h（接口 + 注册）
│   └── port/
│       ├── at32/                   # ★ AT32 移植层
│       │   ├── ota_flash_hal.c     # flash_unlock/sector_erase/word_program 封装
│       │   ├── ota_usart_hal.c     # USART1 + DMA 循环缓冲（Transport_USART1）
│       │   ├── ota_delay.c         # SysTick 延时（或复用 wk_delay_ms）
│       │   ├── ota_key.c           # (v1.1: 不保留按键 → 不实现, 触发走纯软件串口请求)
│       │   ├── ota_printf.c        # printf 重定向到 USART1
│       │   ├── ota_jump.c          # 跳转 APP（外设清理 + 校验 SP + __set_MSP）
│       │   └── ota_app_hook.c      # ★ APP 侧集成：'!' 触发 + 停机保护 + 状态写入
│       └── stm32f103/              # （可选）STM32 侧迁移到共享核心
├── bootloader/                     # ★ Bootloader 固件（独立 target）
│   ├── CMakeLists.txt
│   ├── AT32F421x8_BOOT.ld          # FLASH: 0x08000000, 18K
│   └── src/boot_main.c             # 原 boot.c/main.c 逻辑（用 ota_* 接口）
├── project/ ...                    # 现有 APP 工程（挂 ota_core + ota_app_hook）
├── AT32F421x8_APP.ld               # ★ APP 链接脚本：FLASH: 0x08004800, 44K
├── CMakeLists.txt                  # 增加两个 target
└── tools/build.ps1 等              # 双固件构建/烧录
```

**总体架构（Mermaid）**：

```mermaid
flowchart LR
    subgraph core["ota/core — 平台无关层（原样复用，纯 C）"]
        P[ota_protocol 帧状态机] --- C[ota_crc32]
        C --- F[ota_flash_store 磨损均衡]
        F --- S[ota_upgrade_state 状态机]
        S --- O[ota_offset 续传断点]
        O --- T[ota_transport 传输抽象]
    end

    subgraph port["ota/port/at32 — 芯片差异收敛点"]
        H[flash_hal<br/>USART1+DMA HAL<br/>delay / printf / jump<br/>(v1.1: 无按键)]
        A[ota_app_hook<br/>'!'触发 + 停机保护]
    end

    subgraph fw["两个固件 target"]
        B["Bootloader<br/>AT32F421G8U7_Boot<br/>@0x08000000 · 18KB"]
        APP["APP 电调固件<br/>AT32F421G8U7_WorkBench<br/>@0x08004800 · 44KB<br/>（电机库 + ota_app_hook）"]
    end

    subgraph host["PyQt6 上位机（复用，2 常量参数化）"]
        H1[iap_host_tool]
    end

    core --> port
    port --> B
    A --> APP
    H --> B
    H1 <--USART1 115200--> B
```

### 2.3 与现有 CMake 的整合

- 现有顶层 `CMakeLists.txt` 只生成一个 target `AT32F421G8U7_WorkBench`；
- 新增 `ota/` 静态库（`ota_core`，纯 C）→ 被两个固件 target 链接；
- 新增 `bootloader/` 子目录 → 生成 `AT32F421G8U7_Boot` target（独立链接脚本、独立 startup、**不链接电机库**）；
- APP target 追加 `ota_core` + `ota/port/at32/ota_app_hook.c`，链接脚本换成 `AT32F421x8_APP.ld`；
- `tools/build.ps1` 依次构建两个 target；`tools/flash.ps1` 支持 `-All`（先 Boot 后 APP）与 `-Boot` / `-App` 单独烧录；
- CI（`.github/workflows/build.yml`）改为双固件构建 + 体积断言（Boot ≤ 18KB、APP ≤ 44KB）+ 符号校验沿用。

---

## 3. Flash 布局与容量预算

### 3.1 布局方案（镜像 F103，推荐）

| 区域 | 地址 | 大小 | 说明 |
|---|---|---|---|
| Bootloader | `0x08000000` | 18KB | AT32 Bootloader 固件 |
| APP | `0x08004800` | 44KB | BLDC 电调固件（含 OTA 挂钩） |
| 偏移记录页 | `0x0800F800` | 1KB | 断电续传（每 1024B 存一次） |
| 升级状态页 | `0x0800FC00` | 1KB | 升级状态机 + 磨损均衡 |

**为什么镜像而不重新划分**：与 F103 完全相同的地址/页号 → 上位机除常量外零改动、协议文档直接沿用、偏移粒度（1024B）与 PACKET_SIZE（128B×8 包/页）语义不变。

### 3.2 APP 容量预算

| 项 | 数值 |
|---|---|
| BLDC 固件现状（实测） | ≈ 35.8KB |
| OTA 挂钩增量（触发检测 + 停机保护 + 状态写入 + VTOR） | ≈ 1~2KB（预估） |
| APP 区总需求 | ≈ 37~38KB |
| APP 区容量 | 44KB |
| **余量** | **≈ 6~7KB** |

> 若日后 APP 膨胀超限：Bootloader 可压缩到 16KB（原 STM32 Boot 实际编译产物 <18KB，AT32 端可再瘦身），把空间让给 APP；或改用 20KB Boot / 42KB APP 的布局（代价是上位机常量需同步改）。

### 3.3 RAM 预算

| 项 | 数值 |
|---|---|
| BLDC 固件现状（实测） | ≈ 3.3KB |
| 总 RAM | 16KB |
| 余量 | ≈ 12.7KB |

- Bootloader 是**独立固件**，运行时不与 APP 共存，其 DMA 环形缓冲（1KB）+ 收包缓冲（128B）+ 解析器 + 栈（0x400 预留）总量 < 2.5KB，无压力。

---

## 4. 逐模块移植方案（功能零阉割映射）

### 4.1 直接复用（零改动）模块

| 模块 | 说明 |
|---|---|
| `protocol.c/h`（帧状态机） | 纯 C，无芯片依赖，原样搬入 `ota/core` |
| `crc32.c/h` | 纯软件 CRC32（多项式 0xEDB88320），原样搬入 |
| `transport.c/h`（接口 + 注册） | 纯 C 抽象层，原样搬入 |
| `offset.c` / `upgrade_state.c` | 逻辑不变，仅底层 Flash 调用换 `ota_flash_hal` |
| `flash_store.c` | slot 磨损均衡逻辑不变，底层换成 AT32 Flash 原语 |
| 上位机 `core/iap_worker.py`、`ui/`、`widgets/` | 与芯片无关，全部原样复用 |

### 4.2 芯片层替换（HAL 映射）

| STM32 StdPeriph | AT32 FW Lib | 位置 |
|---|---|---|
| `FLASH_Unlock() / FLASH_Lock()` | `flash_unlock() / flash_lock()` | `ota_flash_hal.c` |
| `FLASH_ErasePage(addr)` | `flash_sector_erase(addr)` | 同上 |
| `FLASH_ProgramWord(addr, data)` | `flash_word_program(addr, data)` | 同上 |
| `RCC_APB2PeriphClockCmd(...)` | `crm_periph_clock_enable(...)` | `ota_usart_hal.c` |
| `GPIO_Init(...)` | `gpio_init(...)` + `gpio_pin_mux_config(...)` | 同上 |
| `USART_Init/Cmd/SendData/GetFlagStatus` | `usart_init / usart_enable / usart_data_transmit / usart_flag_get` | 同上 |
| `DMA_Init / DMA_GetCurrDataCounter` | `dma_...` 系列（CH2 TX / CH3 RX） | 同上 |
| `NVIC_SetVectorTable(...)` | `SCB->VTOR = APP_BASE`（Cortex-M4 直接写） | `ota_app_hook.c` |
| `RCC_DeInit()` | 逐个 `crm_periph_clock_enable(…, FALSE)` / `crm_periph_reset` | `ota_jump.c` |
| `Delay_ms()` | `wk_delay_ms()` 或独立 SysTick | `ota_delay.c` |
| `GPIO_ReadInputDataBit`（KEY） | ~~`gpio_input_data_bit_read(...)`~~ （v1.1：不保留按键，此项不实现） | ~~`ota_key.c`~~ |

> 说明：AT32 的 USART1 在 BLDC 工程里已由 `wk_usart1_init()` 配好（PB6/PB7、115200、DMA1 CH2 TX + CH3 RX）。Bootloader 是独立固件，会自己重新做一遍同样的初始化，不依赖 APP 代码。

### 4.3 Bootloader 移植要点（`bootloader/` 独立 target）

- **不初始化任何电机外设**：Bootloader 不碰 TMR1 互补 PWM、不碰 ADC 注入组，上电即处于"输出关闭"的安全态；
- 初始化顺序沿用原设计：`Transport_Attach(&Transport_USART1) → Transport_Init() → Boot_CheckState() → 主循环 Boot_ProcessRX()`（v1.1：移除 `Key_init()`，不保留按键）；
- 2 秒等待窗口、纯软件请求触发（串口 `!` 5 连 / 上位机协议命令，v1.1 决策：不保留按键）、`STATE_UPGRADING` 断电续传恢复逻辑、`STATE_CRC_FAIL` 等待重试——全部原样保留；
- `Jump_To_App`：校验 `*(uint32_t*)APP_START` 栈顶在 SRAM 范围 → `__disable_irq()` → 清理外设（USART1/DMA/SysTick 复位，消除 DMA 残留——原工程踩过的坑，见其 docs/README）→ 取 `APP_START+4` 复位向量 → `__set_MSP` → 跳转；
- printf 重定向：AT32 用 `__io_putchar` 或自带轮询发送，复用 `at32f421_int.c` 同款实现（v1.1 确认：**Bootloader 保留打印**，日志与协议帧共用 USART1，测试时用上位机包监视器区分）。

### 4.4 APP（BLDC 固件）集成要点

1. **链接脚本**：新增 `AT32F421x8_APP.ld`（FLASH ORIGIN=`0x08004800` LENGTH=44K，RAM 不变），启动文件 `startup_at32f421.s` 复用（向量表随链接地址自动落到 APP 基址）；
2. **向量表重定位**：`main()` 开头（`wk_system_clock_config()` 之后）执行 `SCB->VTOR = 0x08004800;`——M4 原生支持，一行搞定；
3. **触发检测接入现有串口路径（v1.1：纯软件请求的唯一通道）**：BLDC 的 USART1 已走 IDLE+DMA → `SguanESC_Printf_Loop`（电机遥测 ASCII 协议，如 `AO=16.8?`）。在 IDLE 中断里把收到的字节先过一遍轻量扫描：统计连续 `!`（0x21）次数，≥5 置 `ota_trigger_flag`；**不满足时不干扰**电机协议解析（`!` 不在电机协议字符集内）；
4. **主循环响应触发**（不在中断里做重活）：停机保护链——
   ```
   ota_trigger_flag == 1
     → Sguan.Func_Stop()                // 电机状态机停机
     → tmr_output_enable(TMR1, FALSE)   // 关互补 PWM 输出（或 BRK 制动）
     → UPGRADE_SetState(STATE_UPGRADE_READY)   // 写状态页（slot 写入）
     → 延时 ~2s（与 F103 行为一致）
     → NVIC_SystemReset()
   ```
5. **复位后**：Bootloader 读到 `STATE_UPGRADE_READY` → 擦除 APP 区 → 进入接收，链路与 F103 完全一致；
6. 心跳日志、崩溃记录、TMR15 调速等功能全部保留——它们只是"APP 应用逻辑"，与 OTA 无关。

> 与原 F103 APP（LED 闪烁）相比，BLDC 版**唯一新增的差异**就是第 3、4 步的"停机保护"：因为电调复位瞬间若 PWM 仍输出，可能导致桥臂直通/电机冲击，这是电机场景的硬性安全要求，属于"移植增强"，不是功能阉割。

### 4.5 上位机参数化（两处常量）

| 文件 | 现状（硬编码） | 改造 |
|---|---|---|
| `core/protocol.py` | `APP_START_ADDRESS = 0x08004800`、`APP_SIZE = 44*1024` | **config.py 双 profile（v1.1 决策）**：`PROFILE_AT32`（`0x08004800` / 44KB）/ `PROFILE_STM32`（`0x08004800` / 44KB），`APP_PROFILE` 一键切换，默认保持 STM32 值 |
| `core/firmware.py` | `_parse_hex()` 内字面量 `app_start = 0x08004800` | 改为引用 `protocol.APP_START_ADDRESS`（随 profile 自动取值） |

其余（帧构建/解析/CRC/重传/续传/进度/包监视/CRC 破坏模拟）零改动。BIN 固件直接由 `build.ps1` 的 objcopy 产物提供。

### 4.6 构建 / 烧录 / CI

- `tools/build.ps1`：先 `AT32F421G8U7_Boot` 后 `AT32F421G8U7_WorkBench`，两个 `.bin` + `.map` + 体积摘要落盘 `logs/`；
- `tools/flash.ps1`：`-All` 顺序烧 Boot → APP（烧录后自动复位验证），或分体烧录；
- `tools/dashboard.ps1`：编译/烧录面板可展示两个固件体积；
- CI：`build.yml` 构建双 target → 断言 Boot ≤ 18KB、APP ≤ 44KB → 符号校验 → 上传两个 elf/bin 产物。

---

## 5. 关键技术点与风险

| # | 风险/关键点 | 等级 | 说明与对策 |
|---|---|---|---|
| 5.1 | **Flash 扇区粒度** | 中 | AT32F421 驱动为 `flash_sector_erase(addr)`，文档预期 1KB/扇区（与 F103 相同）。**移植第 1 步先上板实测**：若确为 1KB，1024B 续传粒度、页号、256 槽磨损均衡全部原样；若非 1KB，仅需在 `ota_flash_hal` 里做"逻辑页 → 物理扇区"映射，上层不动 |
| 5.2 | **跳转与向量表** | 低 | M4 直接写 `SCB->VTOR`；跳转前按已开外设逐个关闭时钟并复位 DMA（沿用原工程"消除 DMA 残留"的教训），APP 全量重初始化接管 |
| 5.3 | **USART1 双角色共存** | 低 | 电机遥测是 ASCII 命令（`AO=…?`），`!`(0x21) 不在其字符集；触发需 5 连 `!`，与帧数据冲突概率可忽略（原工程已论证）。扫描放 IDLE 中断内仅做计数，重量动作放主循环 |
| 5.4 | **电机安全（新增）** | 高 | 触发升级必须先 `Func_Stop()` → 关 TMR1 输出 → 写状态 → 延时 → 复位；Bootloader 不初始化电机外设，保证升级期间六路 PWM 处于关闭态（v1.1 确认：**升级期间不要求电机硬件断电**，软件停机保护链 + Bootloader 安全态为唯一保障；"MOS 驱动使能脚可被 GPIO 关断"保留为可选硬件增强） |
| 5.5 | **断电续传语义** | 低 | 状态页 + 偏移页位置与 F103 完全一致；擦 APP 中途断电 → 上电进 `UPGRADING` + `OFFSET_Get()==0` → 全量重来（与原行为一致）；恢复续传时重算运行 CRC 逻辑不变 |
| 5.6 | **Flash 写入与中断** | 中 | 电机高环（TMR1 OVF 40kHz）在 APP 正常运行期活跃；**OTA 状态写入只发生在主循环停机后**，且写入前已关 PWM、无电机中断竞争；Bootloader 阶段无电机中断，安全 |
| 5.7 | **RAM 16KB** | 低 | 实测余量 12.7KB，Bootloader 独立运行 <2.5KB，充足 |
| 5.8 | **构建双固件** | 低 | 两 target 共用工具链与大部分源文件；注意 Bootloader 不能误链电机库/日志库（符号体积膨胀） |
| 5.9 | **烧录顺序** | 低 | `flash.ps1 -All` 强制先 Boot 后 APP；若只烧 APP 未烧 Boot，芯片上电进入的是旧代码或空白区——保持与原工程一致的运维约束 |

---

## 6. 实施路线图（阶段 × 交付 × 验收）

| 阶段 | 内容 | 交付物 | 验收标准 |
|---|---|---|---|
| **M0 核心库抽取** | 把 protocol / crc32 / flash_store / upgrade_state / offset / transport 抽到 `ota/core`（纯 C、去 STM32 头文件依赖） | `ota/core/*` | 在 STM32 工程里指向同一份代码编译，行为与原来完全一致（回归基准） |
| **M1 AT32 Bootloader** | `ota/port/at32`（flash_hal / usart_hal / delay / printf / jump，v1.1 无 key）+ `bootloader/` target + `AT32F421x8_BOOT.ld` | `AT32F421G8U7_Boot.bin` | 上位机 `QUERY_OFFSET / QUERY_STATE / RESET_UPGRADE` 命令能收到正确应答；烧空 APP 区后 2s 窗口 `!` 触发进升级（纯软件触发） |
| **M2 APP 集成** | `AT32F421x8_APP.ld` + `SCB->VTOR` + `ota_app_hook`（'!' 扫描 + 停机保护 + 状态写入） | 带 OTA 挂钩的 BLDC 固件 | 上位机「进入升级」→ APP 停机 → 复位 → Bootloader 打印 `Upgrade requested` |
| **M3 端到端升级** | 上位机切 `at32` 配置，完整升级 BLDC 固件 | 升级链路跑通 | 100% 进度 → `STATE_UPGRADE_SUCCESS` → 复位后 APP 心跳/遥测正常 |
| **M4 全功能回归** | 断电续传、CRC 破坏重传、限制发送、查询/重置、CRC_FAIL 回退 | 回归记录 | 每个功能与 F103 行为一一对应（逐项核对 §7） |
| **M5 工具链/CI 整合** | build.ps1 / flash.ps1 / dashboard / CI 双固件 + 体积断言 | 脚本 + CI 产物 | push 触发 CI 产出两个 bin；`flash.ps1 -All` 一键完成双固件烧录 |

预计总工作量：M0~M3 为主干（约 60%），M4 回归（约 25%），M5 收尾（约 15%）。

---

## 7. 附录：功能零阉割核对表

| # | 功能 | 移植方式 | 落点 |
|---|---|---|---|
| 1 | 自定义串口协议（SOF/EOF + 长度字段解析） | 核心库原样复用 | `ota/core/ota_protocol` |
| 2 | CRC32 帧级校验 | 核心库原样复用 | `ota/core/ota_crc32` |
| 3 | CRC 错误自动重传（≤3 次，SEQ 序号） | 核心库 + 上位机原样复用 | 固件协议层 + `iap_worker.py` |
| 4 | 断电续传（每 1024B 存偏移 + 磨损均衡） | 核心库逻辑 + AT32 Flash HAL | `ota/core/ota_offset` + `ota/port/at32/ota_flash_hal` |
| 5 | 双重 CRC（帧级 + 固件运行 CRC） | 核心库原样复用 | `ota/core` + boot 流程 |
| 6 | 5 状态升级状态机（slot 磨损均衡） | 核心库逻辑 + AT32 Flash HAL | `ota/core/ota_upgrade_state` |
| 7 | 2s 等待窗口 + 纯软件串口触发（v1.1：不保留按键） | 移植 | `ota_delay` + boot（`ota_key` 不实现） |
| 8 | APP 端 5 连 `!` 触发复位 | 移植 + 停机保护增强 | `ota/port/at32/ota_app_hook` |
| 9 | DMA 循环缓冲零中断接收 | 移植（AT32 DMA1 CH3） | `ota/port/at32/ota_usart_hal` |
| 10 | 传输层抽象（可换 CAN/SPI） | 接口原样复用 | `ota/core/ota_transport` |
| 11 | PyQt6 上位机全部功能（拖放/进度/包监视/百分比限制/CRC 破坏/查询/重置/续传） | 原样复用 + 2 处常量参数化 | `iap_host_tool/` |
| 12 | BIN/HEX 固件解析 | 原样复用 + 常量参数化 | `firmware.py` |
| 13 | 一键构建脚本（多平台） | 并入 AT32 pwsh 工具链 | `tools/build.ps1` 等 |
| 14 | 升级失败处理（CRC_FAIL 等待重试） | 原样保留 | boot 状态机 |

**唯一新增项**（非阉割、属增强）：APP 触发升级前的**电机停机保护链**（§4.4 第 4 步）。

---

## 8. 附：需要现场确认的问题清单

1. [ ] AT32F421 Flash 扇区大小实测是否为 1KB？（决定 §5.1 是否需逻辑页映射）
2. [x] Bootloader 按键是否保留？→ **v1.1 决策：不保留按键**，纯软件串口请求触发（`ota_key.c` 不实现；`ota/port/at32/ota_key` 目录标注占位）
3. [x] 上位机配置化方式 → **v1.1 决策：`config.py` 双 profile**（`PROFILE_AT32` / `PROFILE_STM32`，默认 STM32，双击即用）
4. [x] Bootloader 是否要保留打印（printf）→ **v1.1 决策：保留**（printf 重定向；日志与协议帧共存 USART1，测试时用上位机包监视器区分）
5. [x] 升级期间是否要求电机断电 → **v1.1 决策：不要求硬件断电**，软件停机保护链（Func_Stop → 关 PWM → 写状态 → 延时 → 复位）+ Bootloader 安全态为唯一保障；MOS 使能脚 GPIO 关断保留为可选增强
6. [ ] STM32 老工程是否也要切到共享 `ota/core`？（可选，不影响本方案验收）
