/**
  **************************************************************************
  * @file     wk_tmr15.h
  * @brief    TMR15 输入捕获驱动: PA2 = TMR15_CH1(TI1) 测量 555 PWM 脉宽/周期
  *
  * 测量原理(零中断, 主循环周期读):
  *   使用定时器 PWM 输入模式: IC1(TI1 上升沿)->CCR1 锁存周期起点时刻
  *                              IC2(TI1 下降沿, 间接)->CCR2 锁存脉宽终点时刻
  *   周期 = 相邻两次 CCR1 之差(16 位回绕安全), 脉宽 = CCR2 - CCR1
  *   占空比 = 脉宽 / 周期   (555 输出 ~100kHz 可调占空比)
  *
  * 硬件事实(数据手册 Table 5): PA2 复用 = TMR15_CH1 / USART2_TX
  *   TMR15 挂 APB2(div=1), 计数时钟 120MHz
  *   (100kHz -> 1200 计数/周期, 脉宽分辨率 ~8.3ns)
  **************************************************************************
  */
#ifndef __WK_TMR15_H
#define __WK_TMR15_H

#include "at32f421.h"

void wk_tmr15_init(void);                                /* PA2 复用 + TMR15 时基 + PWM 输入模式 */
void wk_tmr15_isr(void);                                 /* TMR1 中断里刷新捕获快照(40kHz) */
uint8_t wk_tmr15_pwm_measure(uint16_t *period, uint16_t *pulse); /* 1=有效; 0=无有效信号 */

#endif /* __WK_TMR15_H */
