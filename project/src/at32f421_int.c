/* add user code begin Header */
/**
  **************************************************************************
  * @file     at32f421_int.c
  * @brief    main interrupt service routines.
  **************************************************************************
  * Copyright (c) 2025, Artery Technology, All rights reserved.
  *
  * The software Board Support Package (BSP) that is made available to
  * download from Artery official website is the copyrighted work of Artery.
  * Artery authorizes customers to use, copy, and distribute the BSP
  * software and its related documentation for the purpose of design and
  * development in conjunction with Artery microcontrollers. Use of the
  * software is governed by this copyright notice and the following disclaimer.
  *
  * THIS SOFTWARE IS PROVIDED ON "AS IS" BASIS WITHOUT WARRANTIES,
  * GUARANTEES OR REPRESENTATIONS OF ANY KIND. ARTERY EXPRESSLY DISCLAIMS,
  * TO THE FULLEST EXTENT PERMITTED BY LAW, ALL EXPRESS, IMPLIED OR
  * STATUTORY OR OTHER WARRANTIES, GUARANTEES OR REPRESENTATIONS,
  * INCLUDING BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY,
  * FITNESS FOR A PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
  *
  **************************************************************************
  */
/* add user code end Header */

/* includes ------------------------------------------------------------------*/
#include "at32f421_int.h"
#include "wk_system.h"
/* private includes ----------------------------------------------------------*/
/* add user code begin private includes */
#include "SguanESC.h"
#include "wk_tmr15.h"
#include <at32f421_wk_config.h>
#include "at32f421_usart.h"
#include "stdio.h"
#include "string.h"
#include "log.h"
#include "ota_app_hook.h"


/* add user code end private includes */

/* private typedef -----------------------------------------------------------*/
/* add user code begin private typedef */

/* add user code end private typedef */

/* private define ------------------------------------------------------------*/
/* add user code begin private define */

/* add user code end private define */

/* private macro -------------------------------------------------------------*/
/* add user code begin private macro */

/* add user code end private macro */

/* private variables ---------------------------------------------------------*/
/* add user code begin private variables */

/* add user code end private variables */

/* private function prototypes --------------------------------------------*/
/* add user code begin function prototypes */

/* add user code end function prototypes */

/* private user code ---------------------------------------------------------*/
/* add user code begin 0 */
volatile uint32_t ADC_InjectedValues[4] = {0};
volatile uint32_t g_high_loop_cnt = 0; /* Step 7: 高速环执行计数(主循环心跳用) */
/* add user code end 0 */

/* external variables ---------------------------------------------------------*/
/* add user code begin external variables */

/* add user code end external variables */

/**
  * @brief  this function handles nmi exception.
  * @param  none
  * @retval none
  */
void NMI_Handler(void)
{
  /* add user code begin NonMaskableInt_IRQ 0 */

  /* add user code end NonMaskableInt_IRQ 0 */

  /* add user code begin NonMaskableInt_IRQ 1 */

  /* add user code end NonMaskableInt_IRQ 1 */
}

/**
  * @brief  this function handles hard fault exception.
  * @param  none
  * @retval none
  */
/* NOTE: HardFault/MemManage/BusFault/UsageFault 处理已由 crash/crash.c 接管
 * (startup 向量表里的 WEAK 符号被 crash.c 的强符号覆盖, 崩溃时保存现场到
 * g_hardfault 并死循环)。ATWP 重新生成本文件后会恢复以下 4 个函数,
 * 需删除(见 docs/学习路线.md Step 6 注意事项)。 */


/**
  * @brief  this function handles debug monitor exception.
  * @param  none
  * @retval none
  */
void DebugMon_Handler(void)
{
  /* add user code begin DebugMonitor_IRQ 0 */

  /* add user code end DebugMonitor_IRQ 0 */
  /* add user code begin DebugMonitor_IRQ 1 */

  /* add user code end DebugMonitor_IRQ 1 */
}

/**
  * @brief  this function handles systick handler.
  * @param  none
  * @retval none
  */
void SysTick_Handler(void)
{
  /* add user code begin SysTick_IRQ 0 */

  /* add user code end SysTick_IRQ 0 */

  wk_timebase_handler();
  /* add user code begin SysTick_IRQ 1 */

  /* add user code end SysTick_IRQ 1 */
}

/**
  * @brief  this function handles ADC1 & Comparator handler.
  * @param  none
  * @retval none
  */
void ADC1_CMP_IRQHandler(void)
{
  /* add user code begin ADC1_CMP_IRQ 0 */

  /* add user code end ADC1_CMP_IRQ 0 */

  if(adc_interrupt_flag_get(ADC1, ADC_CCE_FLAG) != RESET)
  {
    /* add user code begin ADC1_ADC_CCE_FLAG */
    /* clear flag */
    adc_flag_clear(ADC1, ADC_CCE_FLAG);
    //处理ADC1的常规组中断事件 常规组只有 1 个数据寄存器 DR
    // Fix: 母线已并入注入组第4通道(JDR4), 此处不再写 [3](避免普通组被意外触发时覆盖母线值)
    // ADC_InjectedValues[3] = adc_ordinary_conversion_data_get(ADC1); // 母线电压


    /* add user code end ADC1_ADC_CCE_FLAG */ 
  }

  if(adc_interrupt_flag_get(ADC1, ADC_PCCE_FLAG) != RESET)
  {
    /* add user code begin ADC1_ADC_PCCE_FLAG */
    /* clear flag */
    adc_flag_clear(ADC1, ADC_PCCE_FLAG);
    //处理ADC1的抢占组中断事件 抢占组有 4 个数据寄存器 JDR1~JDR4
    ADC_InjectedValues[0] = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_1); // A相电压
    ADC_InjectedValues[1] = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_2); // B相电压
    ADC_InjectedValues[2] = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_3); // C相电压
    ADC_InjectedValues[3] = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_4); // 母线电压(并入注入组第4通道, 与三相同帧)  
    
    /* add user code end ADC1_ADC_PCCE_FLAG */ 
  }

  /* add user code begin ADC1_CMP_IRQ 1 */

  /* add user code end ADC1_CMP_IRQ 1 */
}

/**
  * @brief  this function handles TMR1 brake overflow trigger and hall handler.
  * @param  none
  * @retval none
  */
void TMR1_BRK_OVF_TRG_HALL_IRQHandler(void)
{
  /* add user code begin TMR1_BRK_OVF_TRG_HALL_IRQ 0 */
  /* add user code end TMR1_BRK_OVF_TRG_HALL_IRQ 0 */

 /* overflow interrupt management */
  if(tmr_interrupt_flag_get(TMR1, TMR_OVF_FLAG) != RESET)
  {
    /* add user code begin TMR1_TMR_OVF_FLAG */
    /* clear flag */
    tmr_flag_clear(TMR1, TMR_OVF_FLAG);
  //处理TMR1的溢出中断事件

    // 触发ADC采样:注入组(三相+母线4通道)转换 —— 每4次中断(100us)触发一次,
    // 长采样(41.5周期×4=216周期)需留足转换窗口, 25us 连续触发会转换不完/触发堆积
    static uint8_t adc_div_cnt = 0;
    if (++adc_div_cnt >= 4) {
        adc_div_cnt = 0;
        adc_preempt_software_trigger_enable(ADC1, TRUE);
    }

    //电机控制主循环
    SguanESC_High_Loop();
    // 电调状态机处理函数
    SguanESC_Low_Loop();

    // Step 7: 高速环计数(日志输出移到主循环, 中断里不打日志避免阻塞, 且中断内 tick 失真)
    g_high_loop_cnt++;

    // 555 PWM(PA2) 捕获快照: 40kHz 采样率 >> PWM 周期, 主循环结算用
    wk_tmr15_isr();

    /* add user code end TMR1_TMR_OVF_FLAG */
  }

  /* add user code begin TMR1_BRK_OVF_TRG_HALL_IRQ 1 */

  /* add user code end TMR1_BRK_OVF_TRG_HALL_IRQ 1 */
}

/**
  * @brief  this function handles USART1 handler.
  * @param  none
  * @retval none
  */
void USART1_IRQHandler(void)
{
  /* add user code begin USART1_IRQ 0 */
  if(usart_interrupt_flag_get(USART1, USART_IDLEF_FLAG) != RESET) //空闲中断跳转标志位
  {
    //AT32空闲标志必须读STS再读DT来清除，usart_flag_clear无效
    //usart_flag_clear(USART1, USART_IDLEF_FLAG);
    // 手动volatile读取，杜绝编译器O2/O3优化把读寄存器删掉
        __IO uint16_t temp;
        temp = USART1->sts;
        temp = USART1->dt;
        (void)temp;

        //计算实际收到字节数：BUF_LEN初始化为缓冲区总长，dma搬运一个数据dtcnt-1，所以BUF_LEN - dtcnt就是实际接收长度
        uint16_t rx_len = BUF_LEN - dma_data_number_get(DMA1_CHANNEL3);

        if(rx_len > 0)
        {// 这里等价HAL的HAL_UARTEx_RxEventCallback回调:IDLE串口空闲中断 + DMA
            /* OTA: '!' 触发扫描（每字节过一遍, 连续 '!' ≥5 置标志; 不干扰电机协议） */
            for (uint16_t _i = 0; _i < rx_len; _i++) {
                OtaAppHook_ScanChar((char)usart1_rx_buf[_i]);
            }
            memcpy(Sguan_PrintfBuff, usart1_rx_buf, rx_len);
            SguanESC_Printf_Loop(Sguan_PrintfBuff, rx_len);
            LOG_INFO("uart rx: %u bytes -> SguanESC_Printf_Loop", (unsigned)rx_len);
            rx_len = 0; //清零接收长度，准备下一轮接收
        }

        //重启DMA接收，下一轮等待数据
        //usart_interrupt_enable(USART1, USART_IDLE_INT, TRUE); //打开空闲中断,一次就行
        //踩坑(踩坑指南 §20): dma_reset 会清掉 CH3 的 paddr/maddr/方向/宽度,
        //   DMA 从错误地址搬 1 字节即停 —— 重启必须保留配置, 只关/开使能
        dma_channel_enable(DMA1_CHANNEL3, FALSE);   // 关使能(保留 paddr/maddr 配置)
        dma_data_number_set(DMA1_CHANNEL3, BUF_LEN); //设置DMA通道数据长度
        dma_channel_enable(DMA1_CHANNEL3, TRUE); //启动DMA接收
        /* add user code start USART1_IRQ 0 */
  }
  /* add user code end USART1_IRQ 0 */

  /* add user code begin USART1_IRQ 1 */

  /* add user code end USART1_IRQ 1 */
}

/* add user code begin 1 */
int __io_putchar(int ch)
{ 
    /* 等待发送数据缓冲空(TDBE)再写, 否则连续 usart_data_transmit
       会覆盖未发完的字节导致串口丢字节(日志只剩半截)。
       加超时: 串口链路异常(如 DAPLink 卡住)时不能永久阻塞主程序,
       超时则丢弃该字符继续运行(日志丢失可接受, 程序活着优先)。 */
    volatile uint32_t timeout = 100000;
    while(usart_flag_get(USART1, USART_TDBE_FLAG) == RESET)
    {
        if(--timeout == 0) { return ch; }
    }
    usart_data_transmit(USART1, (uint16_t)ch);
    return ch;
}

int _write(int file, char *ptr, int len)
{
    (void)file; // 忽略文件描述符
    for(int i=0;i<len;i++)
    {
        __io_putchar(*ptr++);
    }
    return len;
}


/* add user code end 1 */
