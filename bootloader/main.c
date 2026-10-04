/**
 * @file    main.c
 * @brief   AT32F421 Bootloader 主程序
 *
 * 启动流程：
 *   1. 时钟：HICK → PLL ×30 → 120MHz（与 APP 一致, 保证 USART 波特率计算相同）
 *   2. 外设时钟 + SysTick 时基
 *   3. 注册 USART1 传输层（PB6/PB7 115200 DMA+IDLE）
 *   4. Boot_CheckState(): 按升级状态决定 跳转/续传/等待触发
 *   5. Boot_ProcessRX(): 协议主循环（收帧→组包→写 Flash）
 */

#include "boot.h"
#include <stdio.h>
#include <string.h>
#include "at32f421.h"
#include "ota_transport.h"
#include "ota_usart_hal.h"
#include "ota_delay.h"

/* ── 全局变量（boot.c 引用） ── */
uint8_t  G_RxBuffer[PACKET_SIZE];
uint16_t G_RxCounter = 0;
uint32_t G_RunningCRC = 0;
uint32_t G_FlashWriteOffset = 0;

/* ════════════════════════════════════════════════════════════
 * 系统时钟：HICK / 12 × 30 = 120MHz
 * （与 APP 侧 wk_system_clock_config 完全一致）
 * ════════════════════════════════════════════════════════════ */

static void Boot_SystemClock_Config(void)
{
    crm_reset();

    /* flash 等待周期: 120MHz 需 3 cycle */
    flash_psr_set(FLASH_WAIT_CYCLE_3);

    /* LICK (低速内部时钟, 与 APP 保持一致) */
    crm_clock_source_enable(CRM_CLOCK_SOURCE_LICK, TRUE);
    while (crm_flag_get(CRM_LICK_STABLE_FLAG) != SET) {
    }

    /* HICK (高速内部时钟) */
    crm_clock_source_enable(CRM_CLOCK_SOURCE_HICK, TRUE);
    while (crm_flag_get(CRM_HICK_STABLE_FLAG) != SET) {
    }

    /* PLL: HICK 源 ×30 → 120MHz */
    crm_pll_config(CRM_PLL_SOURCE_HICK, CRM_PLL_MULT_30);
    crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
    while (crm_flag_get(CRM_PLL_STABLE_FLAG) != SET) {
    }

    /* AHB/APB 分频 1 */
    crm_ahb_div_set(CRM_AHB_DIV_1);
    crm_apb2_div_set(CRM_APB2_DIV_1);
    crm_apb1_div_set(CRM_APB1_DIV_1);

    /* 自动步进模式（切换时钟安全） */
    crm_auto_step_mode_enable(TRUE);
    crm_sysclk_switch(CRM_SCLK_PLL);
    while (crm_sysclk_switch_status_get() != CRM_SCLK_PLL) {
    }
    crm_auto_step_mode_enable(FALSE);

    system_core_clock_update();
}

static void Boot_PeriphClock_Config(void)
{
    crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_SCFG_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_USART1_PERIPH_CLOCK, TRUE);
}

static void Boot_Nvic_Config(void)
{
    nvic_priority_group_config(NVIC_PRIORITY_GROUP_4);
    nvic_irq_enable(USART1_IRQn, 5, 0);
}

int main(void)
{
    /* 1. 时钟 */
    Boot_SystemClock_Config();
    Boot_PeriphClock_Config();
    Boot_Nvic_Config();

    /* 2. 时基（SysTick 1ms） */
    OtaDelay_Init();

    /* 3. 注册 USART1 传输层并初始化 */
    Transport_Attach(&Transport_USART1);
    Transport_Init();

    /* 4. 打印启动横幅 */
    printf("\r\n========================================\r\n");
    printf(" AT32F421 Bootloader v1.0 (OTA)\r\n");
    printf(" USART1 115200 PB6/PB7 DMA+IDLE\r\n");
    printf("========================================\r\n");

    /* 5. 状态检查：升级 / 续传 / 跳转 / 等待触发 */
    Boot_CheckState();

    /* 6. 协议主循环（升级模式才到达这里） */
    while (1) {
        Boot_ProcessRX();
    }
}
