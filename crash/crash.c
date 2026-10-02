/**
 * crash.c — HardFault 现场记录器（Step 6: coredump 崩溃定位）
 *
 * 原理: startup_at32f421.s 里的 HardFault_Handler 是 WEAK 符号,
 *       本文件用强符号覆盖它(不用动启动文件), 崩溃时把现场保存到
 *       RAM 里的 g_hardfault, 然后死循环停住等 GDB attach 分析。
 *
 * GDB 用法(崩溃后连上):
 *   (gdb) p g_hardfault        # 崩溃现场结构体(PC/LR/栈帧/故障原因)
 *   (gdb) p/x g_hardfault.cfsr # 故障原因寄存器(逐位解析)
 *   (gdb) x/16xw &g_hardfault  # 原始内存
 *   (gdb) bt                   # 栈回溯
 *
 * 演示: (gdb) set {unsigned long}0 = 1  触发一次 BusFault
 *       (gdb) set $pc = 0xFFFFFFFF      触发一次 HardFault
 */
#include <stdint.h>
#include "log.h"

/* SCB 故障寄存器 (Cortex-M4, SCB 基址 0xE000ED00) */
#define SCB_CFSR    (*(volatile uint32_t *)0xE000ED28u) /* MMFSR+BFSR+UFSR 组合 */
#define SCB_HFSR    (*(volatile uint32_t *)0xE000ED2Cu) /* 硬故障状态寄存器 */
#define SCB_MMFAR   (*(volatile uint32_t *)0xE000ED34u) /* MemManage 访问地址 */
#define SCB_BFAR    (*(volatile uint32_t *)0xE000ED38u) /* BusFault 访问地址 */

struct HardFaultRegs {
    uint32_t r0;   /* 以下 8 个由异常机制自动压栈(栈帧) */
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;   /* 崩溃点的返回地址 */
    uint32_t pc;   /* 崩溃点指令地址(最重要!) */
    uint32_t psr;  /* xPSR 程序状态 */
    uint32_t cfsr; /* 故障原因(可逐位解析) */
    uint32_t hfsr; /* 硬故障状态 */
    uint32_t mmfar;/* MemManage 触发地址 */
    uint32_t bfar; /* BusFault 触发地址 */
    uint32_t faulting_sp; /* 触发异常的 SP */
    uint32_t exc_return;  /* EXC_RETURN 值 */
};
volatile struct HardFaultRegs g_hardfault = {0};

/* naked: 无函数序言, 第一时间抓 SP 和 EXC_RETURN, 再尾调用 C 部分 */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"           /* EXC_RETURN bit2: 0=MSP, 1=PSP */
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"        /* r0 = faulting SP */
        "mov r1, lr\n"           /* r1 = EXC_RETURN */
        "b HardFault_HandlerC\n"
    );
}

void HardFault_HandlerC(uint32_t faulting_sp, uint32_t exc_return)
{
    uint32_t *frame = (uint32_t *)faulting_sp; /* 异常自动压栈的帧 */

    g_hardfault.r0   = frame[0];
    g_hardfault.r1   = frame[1];
    g_hardfault.r2   = frame[2];
    g_hardfault.r3   = frame[3];
    g_hardfault.r12  = frame[4];
    g_hardfault.lr   = frame[5];
    g_hardfault.pc   = frame[6];
    g_hardfault.psr  = frame[7];
    g_hardfault.cfsr = SCB_CFSR;
    g_hardfault.hfsr = SCB_HFSR;
    g_hardfault.mmfar = SCB_MMFAR;
    g_hardfault.bfar  = SCB_BFAR;
    g_hardfault.faulting_sp = faulting_sp;
    g_hardfault.exc_return  = exc_return;

    /* Step 7: 崩溃现场直接打到串口(不接 GDB 也能看到崩溃点) */
    log_write(LOG_LEVEL_ERROR,
              "*** CRASH *** pc=0x%08lX lr=0x%08lX faulting_sp=0x%08lX",
              (unsigned long)g_hardfault.pc,
              (unsigned long)g_hardfault.lr,
              (unsigned long)g_hardfault.faulting_sp);
    log_write(LOG_LEVEL_ERROR,
              "  cfsr=0x%08lX hfsr=0x%08lX mmfar=0x%08lX bfar=0x%08lX",
              (unsigned long)g_hardfault.cfsr,
              (unsigned long)g_hardfault.hfsr,
              (unsigned long)g_hardfault.mmfar,
              (unsigned long)g_hardfault.bfar);
    log_write(LOG_LEVEL_ERROR,
              "  r0=0x%08lX r1=0x%08lX r2=0x%08lX r3=0x%08lX",
              (unsigned long)g_hardfault.r0,
              (unsigned long)g_hardfault.r1,
              (unsigned long)g_hardfault.r2,
              (unsigned long)g_hardfault.r3);

    /* 停住, 等 GDB attach 读现场; nop 防止 -O2 优化掉空循环 */
    while (1) {
        __asm volatile("nop");
    }
}

/* 其他 fault 类型同样接管(向量表 WEAK → 强符号), 共用同一现场记录 */
__attribute__((naked)) void MemManage_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b HardFault_HandlerC\n"
    );
}

__attribute__((naked)) void BusFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b HardFault_HandlerC\n"
    );
}

__attribute__((naked)) void UsageFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b HardFault_HandlerC\n"
    );
}

/* 触发崩溃演示(GDB 里二选一, 无需 C 函数):
 *   (gdb) set {unsigned long}0 = 1     # 写地址 0 → BusFault
 *   (gdb) set $pc = 0xFFFFFFFF         # 跳到非法地址 → HardFault
 */
