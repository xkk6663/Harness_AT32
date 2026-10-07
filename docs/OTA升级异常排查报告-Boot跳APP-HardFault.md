# OTA 升级异常排查报告：Boot 跳 APP 立即 HardFault（PC=0）

> 文档版本：v1.0  
> 日期：2026-10-07  
> 涉及提交：e410ad2（修复）  
> 关联踩坑指南：§35

---

## 一、问题概述

### 1.1 触发条件

OTA 升级完成后（或正常上电时），Bootloader 执行到 `STATE_RUNNING` 分支，2 秒升级窗口内无触发信号，输出：

```
No upgrade request, jumping to APP...
```

随后 **APP 永远起不来**——串口无心跳、电机无响应、调试器打断发现 CPU 停在 `HardFault_Handler`。

### 1.2 复现步骤

1. 烧录 Bootloader（@0x08000000）+ APP（@0x08004800）
2. 确保升级状态为 `STATE_RUNNING`（0xA5A5A5A0）
3. 复位芯片
4. Boot 正常启动 → 2 秒窗口 → 打印 "jumping to APP..." → **无下文**

### 1.3 异常表现

| 观测维度 | 现象 |
|---|---|
| 串口 | Boot 横幅正常，"jumping to APP..." 后无任何输出 |
| GDB 打断 | PC = `HardFault_HandlerC+210` |
| 异常栈帧返回地址 | **PC = 0x00000000** |
| LR | `0x20003fe0`（SRAM 地址，异常） |
| 调用栈 | #2/#3 在 Boot 的 `OtaJump_ToApp` 附近 |

---

## 二、排查过程（个人负责的查错部分）

### 2.1 第一轮：排除 APP 固件本身问题

**查了什么：**
- 用 `arm-none-eabi-objdump -h APP.elf` 确认 APP 链接地址是否为 `0x08004800`
- 用 `arm-none-eabi-nm APP.elf | grep g_pfnVectors` 确认向量表位置
- 用 OpenOCD `mdw 0x08004800` 读 Flash，确认 APP 向量表前两个 word：
  - `0x08004800` = 初始 MSP = `0x20004000`（APP 栈顶）
  - `0x08004804` = Reset_Handler = `0x080097e0`（合法地址，非 0）

**结论：** APP 固件本身没问题，向量表正确，Reset_Handler 地址合法。

### 2.2 第二轮：排除跳转前外设/中断残留

**查了什么：**
- 检查 `OtaJump_ToApp()` 里是否关了全局中断（`__disable_irq()`）—— 已关
- 检查是否切了 VTOR（`SCB->VTOR = APP_START_ADDRESS`）—— 已切
- 检查是否复位了外设（`crm_reset()`）—— 已调
- 用 GDB 在 `OtaJump_ToApp()` 入口打断，单步执行到 `__set_MSP(app_sp)` 之后

**关键发现：** 单步执行 `__set_MSP(app_sp)` 后，MSP 寄存器变为 `0x20004000`。继续单步执行下一行 C 代码时，**立即触发 HardFault**。

### 2.3 第三轮：反汇编实锤（定位根因）

**查了什么：**
- 用 `arm-none-eabi-objdump -d bootloader.elf | grep -A 30 OtaJump_ToApp` 反汇编 Bootloader
- 重点看 `__set_MSP(app_sp)` 之后、函数返回之前的指令序列

**反汇编关键片段（修复前）：**

```asm
OtaJump_ToApp:
    push    {r4, r5, r6, lr}        ; 函数入口保存寄存器
    ...
    bl      __set_MSP                ; MSP = 0x20004000 (APP栈顶)
    ...
    blx     r0                       ; jump_to_app() 函数指针调用
    ...
    ldmia.w sp!, {r4, r5, r6, lr}   ; ★ 函数返回前恢复寄存器
    bx      lr                       ; 返回
```

**根因定位：**

`ldmia.w sp!, {r4, r5, r6, lr}` 这条指令从当前 SP 指向的地址读 16 字节到 r4/r5/r6/lr。但此时 SP（MSP）已经被 `__set_MSP()` 改成了 **`0x20004000`**。

AT32F421G8U7 的 SRAM 范围是 `0x20000000 ~ 0x20003FFF`（16KB），`0x20004000` **正好是 SRAM 末尾+1，越界**。从越界地址读 16 字节触发 **BusFault → 升级为 HardFault**，PC 被异常处理改成 0，APP 永远起不来。

---

## 三、根因分析

### 3.1 为什么 `ldmia` 会执行？

`jump_to_app()` 是一个 C 函数指针调用，指向 APP 的 Reset_Handler。理论上 Reset_Handler 不会返回，那函数返回前的 `ldmia` 不应该执行到。

但实际执行流程是：

1. `__set_MSP(app_sp)` → MSP = 0x20004000
2. `blx r0` → 跳转到 APP Reset_Handler（0x080097e0）
3. APP Reset_Handler 开始执行，但此时 **VTOR 虽然切了，MSP 已经是 APP 栈顶（越界地址）**
4. APP Reset_Handler 里的第一条指令通常是设置栈（从向量表读 MSP），但在这之前如果有任何**异常触发**（比如 SysTick 滴答、外设中断），硬件会用当前 MSP（0x20004000）自动压栈 → 写越界 → BusFault
5. 或者，`blx` 跳转本身因为某些原因（如地址对齐问题、缓存问题）没有正确执行，CPU 取到非法指令 → HardFault

无论哪种路径，**核心问题都是：`__set_MSP()` 之后还有 C 级别的代码执行，而 C 编译器生成的任何栈操作（函数调用、返回、异常压栈）都会使用已经越界的 MSP**。

### 3.2 为什么 C 函数指针调用本身有问题？

即使 `jump_to_app()` 不返回，C 编译器在编译 `OtaJump_ToApp()` 时：
- 函数入口生成 `push {r4,r5,r6,lr}`（用 Boot 的栈，合法）
- 函数出口生成 `ldmia sp!, {r4,r5,r6,lr}` + `bx lr`（用当前 SP，已越界）

编译器不知道 `jump_to_app()` 不会返回，所以一定会生成函数返回的清理代码。虽然运行时可能走不到，但 **`blx` 跳转后如果 APP 侧有任何异常导致"返回"到 Boot 的函数返回代码，就会执行 `ldmia` 从越界地址读**。

更直接的路径是：**`blx` 指令执行时，LR 被设置为 `blx` 下一条指令的地址。如果 APP Reset_Handler 因为任何原因执行了 `bx lr`（比如异常返回），就会回到 Boot 的函数返回代码，执行 `ldmia`。**

---

## 四、STM32 原工程 vs AT32 移植：为什么 STM32 可以？

### 4.1 关键差异对照表

| 维度 | STM32F103C8（原工程） | AT32F421G8U7（本工程） |
|---|---|---|
| 内核 | Cortex-M3 | Cortex-M4F |
| SRAM | **20KB**（0x20000000~0x20004FFF） | **16KB**（0x20000000~0x20003FFF） |
| APP 栈顶 | 0x20005000（越界 1 字节） | 0x20004000（越界 1 字节） |
| 编译器 | 大概率 **Keil ARMCC** | **GCC arm-none-eabi** |
| 跳转实现 | STM32 官方 AN2557 标准写法 | 移植时"翻译"为 C 函数指针调用 |

### 4.2 STM32 能工作的可能原因（按可能性排序）

#### 原因 1（最可能）：编译器生成的函数返回代码不同

Keil ARMCC 和 GCC 在函数返回时生成的指令序列可能不同：

- **GCC**：倾向于生成 `ldmia.w sp!, {r4-r11,lr}` 然后 `bx lr`，**明确从 SP 读寄存器**
- **Keil ARMCC**：可能生成不同的序列，或者在优化级别较高时，因为 `jump_to_app()` 是 `__attribute__((noreturn))` 或编译器能推断出不返回，**直接省略函数返回的清理代码**（不生成 ldmia）

如果 Keil 省略了 `ldmia`，那即使 MSP 越界，也不会有从越界地址读的操作，APP 就能正常启动。

#### 原因 2：SRAM 边界行为不同

Cortex-M3（STM32F103）和 Cortex-M4F（AT32F421）的总线矩阵设计可能不同：

- **STM32F103**：SRAM 边界外（0x20005000 之后）可能映射到保留区域，读操作**返回垃圾值但不触发 BusFault**（或者触发的是 MemManage 而非 BusFault，而 MemManage 默认被升级为 HardFault 的行为可能不同）
- **AT32F421**：SRAM 边界外严格 **BusFault**，读操作立即触发异常

这需要查两款芯片的 Reference Manual 里的内存映射章节确认，但从现象上看，AT32 对越界读更严格。

#### 原因 3：原工程跳转实现本身就是内联汇编

STM32 原工程的 `Jump_To_App` 可能本身就是用内联汇编写的（`msr msp + bx` 原子跳转），而可行性方案文档里描述的"__set_MSP → 跳转"只是高层概括，不代表具体是 C 函数调用。

移植到 AT32 时，如果"翻译"成了先 `__set_MSP()` 再 C 函数指针调用，就引入了这个 bug。

### 4.3 结论

**无论 STM32 原工程是因为哪种原因能工作，AT32 上用"先 `__set_MSP()` 再 C 函数调用"的方式都是不安全的。** 正确的做法是用内联汇编原子完成"设 MSP + bx 跳转"，中间无任何 C 级别的栈操作。这是 Cortex-M Bootloader 跳 APP 的通用最佳实践，与芯片型号、编译器无关。

---

## 五、修复方案

### 5.1 修复代码

用内联汇编原子完成"设 MSP + bx 跳转"：

```c
void OtaJump_ToApp(void)
{
    uint32_t app_sp = *(volatile uint32_t *)APP_START_ADDRESS;
    if ((app_sp & 0x2FFE0000) == 0x20000000) {
        __disable_irq();
        crm_reset();
        SCB->VTOR = APP_START_ADDRESS;

        uint32_t jump_addr = *(volatile uint32_t *)(APP_START_ADDRESS + 4);

        /* 内联汇编：msr msp 后直接 bx，中间无任何栈操作 */
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

- `msr msp, %0`：设置 MSP 为 APP 栈顶（0x20004000）
- `bx %1`：直接跳转到 APP Reset_Handler（0x080097e0）
- 两条指令之间**没有任何 C 代码、没有函数调用、没有栈操作**
- 跳转后 APP Reset_Handler 会自己设置栈（从向量表读 MSP，虽然值还是 0x20004000，但 APP 侧的栈操作是从栈顶向下增长，第一次压栈写 0x20003FFC，合法）

### 5.3 为什么 APP 侧用 0x20004000 栈顶没问题？

Cortex-M 的栈是**满递减栈**（Full Descending）：
- 栈顶指针指向**最后一个已压入元素的地址**（或初始时指向 SRAM 末尾+1）
- 压栈操作：先 `SP -= 4`，再写 `[SP]`
- 所以初始 MSP = 0x20004000 时，第一次压栈写 0x20003FFC（合法），不会越界

而 `ldmia sp!, {r4,r5,r6,lr}` 是**从当前 SP 读**（先读后增），SP=0x20004000 时读 0x20004000（越界），所以触发 BusFault。

---

## 六、验证方法

### 6.1 GDB 验证

1. 烧录修复后的 Bootloader + APP
2. 复位芯片，等 5 秒（确保 Boot 已跳 APP）
3. GDB 连接，打断：
   ```
   (gdb) target remote :3333
   (gdb) interrupt
   ```
4. 检查 PC：
   ```
   (gdb) info registers pc
   pc = 0x080091ca  ← 在 APP 的 wk_timebase_get()，合法
   ```
5. 检查调用栈：
   ```
   (gdb) bt
   #0  wk_timebase_get ()
   #1  0x08009xxx in main ()
   ```

### 6.2 串口验证

复位后串口应输出：
```
========================================
 AT32F421 Bootloader v1.0 (OTA)
 USART1 115200 PB6/PB7 DMA+IDLE
========================================

=== Bootloader Start ===
Current upgrade state: 0xA5A5A5A0

=== Bootloader Ready ===
Send "!!!!!" within 2s to enter upgrade mode...
No upgrade request, jumping to APP...
[I][1000] heartbeat tick=1000 high_loop=40010    ← APP 心跳，每秒一条
[I][2000] heartbeat tick=2000 high_loop=80020
...
```

### 6.3 OTA 全链路验证

1. 上位机发 `!!!!!` 触发升级
2. 完整升级 APP 固件
3. 升级完成后 Boot 设 `STATE_UPGRADE_SUCCESS` + 复位
4. 复位后 Boot 读到 SUCCESS → 设回 RUNNING → 跳 APP
5. APP 心跳正常 → **PASS**

---

## 七、经验教训

1. **Bootloader 跳 APP 是"换栈+换 PC"的原子操作，绝对不能在 `__set_MSP()` 之后还有 C 级别的栈操作**（函数调用、返回、局部变量、异常处理）。必须用内联汇编在 `msr msp` 后直接 `bx`。

2. **遇到 HardFault 时先看异常栈帧的返回 PC**：PC=0 几乎一定是"函数指针/向量表读出来是 0 或栈指针非法"；LR 是 SRAM 地址说明异常发生在栈操作时。

3. **反汇编是定位编译器生成代码问题的唯一可靠手段**，不要只看 C 源码。C 源码里看起来没问题的代码，编译器可能生成了有问题的指令序列（比如函数返回前的 ldmia）。

4. **移植代码时不能只"翻译"语法，还要理解底层行为**。STM32 原工程能工作可能是因为编译器/芯片特性的巧合，移植到不同芯片/编译器时必须重新验证关键路径（尤其是 Bootloader 跳 APP 这种涉及内核寄存器的操作）。

5. **APP 栈顶 = SRAM 末尾+1 是标准做法**（满递减栈，第一次压栈写末尾-4，合法），但**在 Bootloader 侧设置 MSP 后不能有任何从 SP 读的操作**（ldmia），因为读的是越界地址。

---

## 八、相关文件

| 文件 | 作用 |
|---|---|
| `ota/port/at32/ota_jump.c` | 跳转实现（已修复为内联汇编） |
| `ota/port/at32/ota_jump.h` | 跳转接口声明 |
| `bootloader/boot.c` | Boot_CheckState() 状态机，调用 OtaJump_ToApp() |
| `bootloader/main.c` | Boot 入口，时钟/串口初始化 |
| `docs/踩坑指南.md` | §35 本条踩坑的精简版记录 |
| `docs/STM32-OTA平台化移植到AT32-BLDC可行性方案.md` | §4.3 Bootloader 移植要点 |
