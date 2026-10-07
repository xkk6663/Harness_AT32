# OTA 升级异常排查报告：Boot 跳 APP HardFault（PC=0）

> 文档版本：v2.0（基于 STM32 原工程源码对比重写）
> 日期：2026-10-07
> 涉及提交：e410ad2（修复）
> 关联踩坑指南：§35
> 对比参考工程：`C:\Users\xiao1\Desktop\STM32-OTA-QT`（STM32F103C8 原工程）

---

## 一、问题概述

### 1.1 触发条件

OTA 升级完成后（或正常上电时），Bootloader 执行到 `STATE_RUNNING` 分支，2 秒升级窗口内无触发信号，串口输出：

```
No upgrade request, jumping to APP...
```

随后 **APP 永远起不来**——串口无心跳、电机无响应。

### 1.2 异常表现（GDB 实锤）

| 观测维度 | 实测值 |
|---|---|
| PC | `HardFault_HandlerC+210` |
| 异常栈帧返回地址 | **PC = 0x00000000** |
| LR | `0x20003fe0`（SRAM 地址，异常） |
| 调用栈 | #2/#3 在 Boot 的 `OtaJump_ToApp` 附近 |

---

## 二、AT32 侧源码与反汇编

### 2.1 修复前的 C 代码（`ota/port/at32/ota_jump.c`）

```c
void OtaJump_ToApp(void)
{
    uint32_t app_sp = *(volatile uint32_t *)APP_START_ADDRESS;
    if ((app_sp & 0x2FFE0000) == 0x20000000) {
        __disable_irq();
        crm_reset();
        SCB->VTOR = APP_START_ADDRESS;
        uint32_t jump_addr = *(volatile uint32_t *)(APP_START_ADDRESS + 4);

        __set_MSP(app_sp);        // ← MSP 被改成 APP 栈顶
        jump_to_app();             // ← C 函数指针调用
    }
}
```

### 2.2 APP 栈顶值（链接脚本 `AT32F421x8_APP.ld`）

AT32F421G8U7 的 SRAM = **16KB**：

```
RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 16K
_estack = ORIGIN(RAM) + LENGTH(RAM) = 0x20004000
```

APP 向量表第一个 word = 初始 MSP = **`0x20004000`**（SRAM 末尾+1，越界 1 字节）。

### 2.3 修复前反汇编的关键指令

GCC 为 `OtaJump_ToApp` 函数返回生成：

```asm
ldmia.w sp!, {r4, r5, r6, lr}    ; 从当前 SP 读 16 字节恢复寄存器
bx lr                              ; 返回
```

此时 SP（MSP）已被 `__set_MSP()` 改成 `0x20004000`，`ldmia` 从 `0x20004000` 读 16 字节 → **越界读 → BusFault → HardFault**。

---

## 三、STM32 原工程源码对比（基于 `STM32-OTA-QT/IAP-Bootloader`）

### 3.1 STM32 的 `Jump_To_App` C 代码（`User/boot.c` 第 22-36 行）

```c
static void Jump_To_App(void)
{
    /* 检查栈顶地址是否合法（是否在 SRAM 范围内） */
    if (((*(__IO uint32_t *)APP_START_ADDRESS) & 0x2FFE0000) == 0x20000000) {

        __disable_irq();                    /* 关全局中断 */
        RCC_DeInit();                       /* 关闭外设时钟 */

        uint32_t JumpAddress = *(__IO uint32_t *)(APP_START_ADDRESS + 4);
        pFunction Jump_To_Application = (pFunction)JumpAddress;

        __set_MSP(*(__IO uint32_t *)APP_START_ADDRESS);  /* 初始化 APP 堆栈 */
        Jump_To_Application();                            /* 跳转 */
    }
}
```

**与 AT32 修复前的代码结构完全相同**：校验栈顶 → `__disable_irq()` → 复位外设 → 读 Reset_Handler → `__set_MSP(app_sp)` → C 函数指针调用。

### 3.2 STM32 APP 栈顶值（链接脚本 `STM32F103C8_APP.ld`）

STM32F103C8 的 SRAM = **20KB**：

```
RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 20K
_estack = ORIGIN(RAM) + LENGTH(RAM) = 0x20005000
```

APP 向量表第一个 word = 初始 MSP = **`0x20005000`**（SRAM 末尾+1，越界 1 字节）。

### 3.3 STM32 `boot.o` 反汇编（`Objects/boot.o`，`arm-none-eabi-objdump -d` 实锤）

```asm
00000000 <Jump_To_App>:
   0:   b570            push    {r4, r5, r6, lr}     ; 入口保存寄存器
   2:   480a            ldr     r0, [pc, #40]          ; APP_START_ADDRESS
   4:   6800            ldr     r0, [r0, #0]           ; 读 APP 栈顶
   6:   490a            ldr     r1, [pc, #40]          ; mask 0x2FFE0000
   8:   4008            ands    r0, r1
   a:   f1b0 5f00       cmp.w   r0, #0x20000000
   e:   d10c            bne.n   2a                      ; 不合法则跳返回
  10:   b672            cpsid   i                       ; __disable_irq()
  12:   f7ff fffe       bl      RCC_DeInit              ; RCC_DeInit()
  16:   4805            ldr     r0, [pc, #20]           ; APP_START_ADDRESS
  18:   1d00            adds    r0, r0, #4               ; APP_START_ADDRESS + 4
  1a:   6804            ldr     r4, [r0, #0]             ; 读 Reset_Handler → r4
  1c:   4625            mov     r5, r4                    ; r5 = Reset_Handler
  1e:   1f01            subs    r1, r0, #4                ; r1 = APP_START_ADDRESS
  20:   6808            ldr     r0, [r1, #0]              ; 读 APP 栈顶 → r0
  22:   f7ff fffe       bl      __set_MSP                  ; __set_MSP(app_sp) ← MSP 越界
  26:   47a8            blx     r5                         ; Jump_To_Application() ← C 函数调用
  28:   bf00            nop
  2a:   bd70            pop     {r4, r5, r6, pc}          ; ★ 函数返回：从 SP 恢复寄存器
  2c:   08004800        .word   0x08004800                 ; APP_START_ADDRESS 常量
  30:   2ffe0000        .word   0x2ffe0000                 ; mask 常量
```

**关键指令序列与 AT32 完全一致**：
- 入口 `push {r4, r5, r6, lr}`
- `bl __set_MSP`（MSP 被改成越界的 APP 栈顶）
- `blx r5`（C 函数指针调用跳 APP）
- 函数返回 `pop {r4, r5, r6, pc}`（从越界 SP 读 16 字节）

### 3.4 源码对比结论

| 对比维度 | STM32F103C8 原工程 | AT32F421G8U7（修复前） |
|---|---|---|
| C 代码结构 | 校验→关中断→复位外设→`__set_MSP`→C函数调用 | 完全相同 |
| 编译器 | GCC（工程含 `CMakeLists.txt` + `cmake/arm-none-eabi.cmake`） | GCC arm-none-eabi |
| 反汇编指令序列 | `push`→`bl __set_MSP`→`blx`→`pop {r4,r5,r6,pc}` | 完全相同 |
| SRAM 大小 | 20KB | 16KB |
| APP 栈顶 | `0x20005000`（越界 1 字节） | `0x20004000`（越界 1 字节） |
| `pop` 读地址范围 | `0x20005000 ~ 0x2000500F`（越界） | `0x20004000 ~ 0x2000400F`（越界） |
| 实际运行结果 | **正常跳转，APP 可运行** | **HardFault，PC=0，APP 起不来** |

**代码层面两者完全相同，编译器生成的指令序列也完全相同，APP 栈顶都越界。差异不在代码，而在芯片硬件对 SRAM 边界外读操作的行为。**

---

## 四、根因分析

### 4.1 AT32 侧的完整故障链路

1. `__set_MSP(0x20004000)` 执行后，MSP = `0x20004000`（AT32F421 SRAM 范围 `0x20000000~0x20003FFF`，越界）
2. `blx r5` 跳转到 APP Reset_Handler
3. APP Reset_Handler 执行过程中，MSP 已是越界地址。任何依赖 MSP 的操作（局部变量压栈、异常压栈、函数返回出栈）都会访问越界地址
4. 越界读/写触发 **BusFault**，BusFault 默认升级为 **HardFault**
5. HardFault_Handler 死循环，GDB 打断发现 PC 在 HardFault_Handler，异常栈帧因压栈时 MSP 越界而内容无效（返回 PC=0）

### 4.2 为什么 `pop {r4, r5, r6, pc}` 是致命指令

Cortex-M 的 `pop {r4, r5, r6, pc}` 等价于 `ldmia sp!, {r4, r5, r6, pc}`：
- 从当前 SP 指向的地址**连续读 16 字节**到 r4/r5/r6/pc
- 然后 SP += 16

当 SP = `0x20004000`（越界）时，读 `0x20004000~0x2000400F` → AT32F421 总线矩阵对该地址范围**严格触发 BusFault**。

### 4.3 为什么 STM32F103 同样的代码不触发

基于源码对比已确认：STM32 的代码、编译器、指令序列、栈顶越界情况与 AT32 完全相同。STM32 能正常运行的唯一解释是 **STM32F103（Cortex-M3，意法半导体总线矩阵）对 SRAM 边界外（`0x20005000` 之后）的读操作不触发 BusFault**（可能返回垃圾值或 0，或总线矩阵做了地址回绕）。

这是**芯片硬件行为差异**，不是代码差异。具体行为需查阅两款芯片的 Reference Manual 中"内存映射"和"总线错误"章节确认。AT32F421（雅特力，Cortex-M4F）对 SRAM 边界外访问更严格。

> **注意**：即使 STM32F103 上这段代码能工作，它也是**不安全的**——依赖了芯片对越界访问的宽容行为。如果换一款严格的芯片（如 AT32F421），同样的代码立即 HardFault。正确的做法是用内联汇编原子跳转，不依赖芯片的越界宽容。

---

## 五、修复方案（已提交 e410ad2）

### 5.1 修复代码（`ota/port/at32/ota_jump.c`）

```c
void OtaJump_ToApp(void)
{
    uint32_t app_sp = *(volatile uint32_t *)APP_START_ADDRESS;
    if ((app_sp & 0x2FFE0000) == 0x20000000) {
        __disable_irq();
        crm_reset();
        SCB->VTOR = APP_START_ADDRESS;
        uint32_t jump_addr = *(volatile uint32_t *)(APP_START_ADDRESS + 4);

        /* 内联汇编：msr msp 后直接 bx，中间无任何 C 栈操作 */
        __asm volatile(
            "msr msp, %0\n"
            "bx %1\n"
            : : "r"(app_sp), "r"(jump_addr)
        );
        /* 不会到达这里 */
    }
}
```

### 5.2 修复原理

- `msr msp, %0`：设置 MSP 为 APP 栈顶（`0x20004000`）
- `bx %1`：直接跳转到 APP Reset_Handler
- 两条指令之间**没有任何 C 代码、没有函数调用、没有栈操作**
- 跳转后 APP Reset_Handler 自己管理栈（满递减栈，第一次压栈写 `0x20003FFC`，合法）

### 5.3 为什么 APP 侧用 `0x20004000` 栈顶本身没问题

Cortex-M 是**满递减栈**（Full Descending）：
- 栈顶指针初始指向 SRAM 末尾+1（`0x20004000`）
- 压栈：先 `SP -= 4`，再写 `[SP]` → 第一次写 `0x20003FFC`（合法）
- 出栈：先读 `[SP]`，再 `SP += 4` → 读的是之前压入的合法地址

所以 APP 正常运行时，栈操作不会越界。**只有 Boot 侧在 `__set_MSP()` 之后、APP 接管栈之前，有 C 级别的从 SP 读操作（`pop`/`ldmia`），才会读越界地址。**

---

## 六、验证方法

### 6.1 GDB 验证

1. 烧录修复后的 Bootloader + APP
2. 复位，等 5 秒
3. GDB 连接打断：
   ```
   (gdb) target remote :3333
   (gdb) interrupt
   (gdb) info registers pc
   pc = 0x080091ca    ← 在 APP 的 wk_timebase_get()，合法
   (gdb) bt
   #0  wk_timebase_get ()
   #1  0x08009xxx in main ()
   ```

### 6.2 串口验证

复位后串口应输出：
```
=== Bootloader Start ===
Current upgrade state: 0xA5A5A5A0
=== Bootloader Ready ===
Send "!!!!!" within 2s to enter upgrade mode...
No upgrade request, jumping to APP...
[I][1000] heartbeat tick=1000 high_loop=40010    ← APP 心跳
[I][2000] heartbeat tick=2000 high_loop=80020
```

### 6.3 OTA 全链路验证

1. 上位机发 `!!!!!` 触发升级
2. 完整升级 APP 固件
3. 升级完成 → Boot 设 `STATE_UPGRADE_SUCCESS` → 复位
4. 复位 → Boot 读 SUCCESS → 设回 RUNNING → 跳 APP
5. APP 心跳正常 → **PASS**

---

## 七、个人负责的查错部分

### 7.1 排查过程（三轮）

**第一轮：排除 APP 固件本身问题**
- `arm-none-eabi-objdump -h APP.elf` 确认 APP 链接地址 `0x08004800`
- `arm-none-eabi-nm APP.elf | grep g_pfnVectors` 确认向量表位置
- OpenOCD `mdw 0x08004800` 读 Flash：`0x08004800`=初始 MSP=`0x20004000`，`0x08004804`=Reset_Handler=`0x080097e0`（合法）
- 结论：APP 固件没问题

**第二轮：排除跳转前外设/中断残留**
- 确认 `__disable_irq()` 已调用
- 确认 `SCB->VTOR = APP_START_ADDRESS` 已设置
- 确认 `crm_reset()` 已调用
- GDB 在 `OtaJump_ToApp()` 入口打断，单步到 `__set_MSP(app_sp)` 之后 → **立即 HardFault**
- 结论：问题出在 `__set_MSP()` 之后

**第三轮：反汇编实锤**
- `arm-none-eabi-objdump -d bootloader.elf | grep -A 30 OtaJump_ToApp`
- 发现函数返回前的 `ldmia.w sp!, {r4, r5, r6, lr}`
- 此时 SP = `0x20004000`（越界），从越界地址读 16 字节 → BusFault
- 结论：根因确认

### 7.2 STM32 原工程对比（本轮补充）

- 读取 `STM32-OTA-QT/IAP-Bootloader/User/boot.c` 第 22-36 行，确认 `Jump_To_App` 代码结构与 AT32 修复前完全相同
- `arm-none-eabi-objdump -d STM32-OTA-QT/IAP-Bootloader/Objects/boot.o` 反汇编，确认指令序列 `push`→`bl __set_MSP`→`blx`→`pop {r4,r5,r6,pc}` 与 AT32 完全相同
- 读取 `STM32-OTA-QT/APP-LED闪烁/STM32F103C8_APP.ld`，确认 RAM 20KB，栈顶 `0x20005000`（也越界）
- 结论：代码和编译器层面无差异，差异在芯片硬件对越界访问的行为

---

## 八、经验教训

1. **Bootloader 跳 APP 是"换栈+换 PC"的原子操作，`__set_MSP()` 之后绝对不能有 C 级别的栈操作**（函数调用、返回、局部变量、异常处理）。必须用内联汇编在 `msr msp` 后直接 `bx`。这是 Cortex-M 通用最佳实践，与芯片型号、编译器无关。

2. **"在 STM32 上能跑"不代表"代码是对的"**。STM32F103 对越界访问的宽容行为掩盖了这个 bug。换一款严格的芯片（AT32F421）立即暴露。移植代码时不能只验证"能跑"，还要审查关键路径（尤其是涉及内核寄存器操作的路径）是否依赖了芯片的非标准宽容行为。

3. **遇到 HardFault 先看异常栈帧的返回 PC**：PC=0 几乎一定是"函数指针/向量表读出来是 0 或栈指针非法"；LR 是 SRAM 地址说明异常发生在栈操作时。

4. **反汇编是定位编译器生成代码问题的唯一可靠手段**，不要只看 C 源码。C 源码里看起来没问题的代码，编译器可能生成了有问题的指令序列（比如函数返回前的 `ldmia`）。

5. **APP 栈顶 = SRAM 末尾+1 是标准做法**（满递减栈，第一次压栈写末尾-4，合法），但**在 Bootloader 侧设置 MSP 后不能有任何从 SP 读的操作**（`pop`/`ldmia`），因为读的是越界地址。

---

## 九、相关文件

| 文件 | 作用 |
|---|---|
| `ota/port/at32/ota_jump.c` | AT32 跳转实现（已修复为内联汇编） |
| `ota/port/at32/ota_jump.h` | 跳转接口声明 |
| `bootloader/boot.c` | Boot_CheckState() 状态机，调用 OtaJump_ToApp() |
| `bootloader/main.c` | Boot 入口，时钟/串口初始化 |
| `docs/踩坑指南.md` | §35 本条踩坑的精简版记录 |
| `docs/STM32-OTA平台化移植到AT32-BLDC可行性方案.md` | §4.3 Bootloader 移植要点 |
| `STM32-OTA-QT/IAP-Bootloader/User/boot.c` | STM32 原工程 Jump_To_App（第 22-36 行） |
| `STM32-OTA-QT/IAP-Bootloader/Objects/boot.o` | STM32 编译产物（可反汇编对比） |
| `STM32-OTA-QT/APP-LED闪烁/STM32F103C8_APP.ld` | STM32 APP 链接脚本（RAM 20KB，栈顶 0x20005000） |
