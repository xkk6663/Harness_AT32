/**
  **************************************************************************
  * @file     User_PWMSpeed.h
  * @brief    555 定时器 PWM(PA2) → BLDC 电机调速业务
  *
  * 硬件: 555 输出 PWMIN1 -> R17(1k) -> PA2(TMR15_CH1/TI1 输入, ~100kHz 可调占空比)
  * 测量: wk_tmr15 输入捕获(零中断), 主循环读 周期/脉宽 → 占空比
  *
  * 映射策略(电压开环, Define_Run_Mode=0):
  *   占空比 < 5%                → Func_Stop() 缓停
  *   占空比 ≥ 5% 且电机未运行   → Func_Start() 启动
  *   占空比 ≥ 5%                → Func_Set_Ubus(占空比 × 母线电压) 调速
  *
  * 若改为速度闭环(Define_Run_Mode=1), 把 Func_Set_Ubus 换成:
  *   Sguan.Func_Set_Velocity(占空比 × 最大转速);  // 转速单位: 机械角速度 rad/s
  *
  * 观测: g_pwm_speed 供心跳/驾驶舱打印 (valid/period/pulse/duty/freq)
  **************************************************************************
  */
#ifndef __USER_PWMSPEED_H
#define __USER_PWMSPEED_H

#include "SguanESC.h"
#include "wk_tmr15.h"

/* 占空比低于此值 → 停机(555 电位器旋到最低时让电机停下) */
#define PWM_SPEED_STOP_DUTY   0.05f

typedef struct {
  uint8_t  valid;       /* 输入信号有效(周期在合法窗口内) */
  uint16_t period_cnt;  /* 周期计数 @120MHz */
  uint16_t pulse_cnt;   /* 高电平脉宽计数 */
  float    duty;        /* 占空比 0.0~1.0 */
  float    freq_khz;    /* 输入频率 kHz */
  float    target_ubus; /* 当前目标电压(电压开环) */
} pwm_speed_t;

/* 定义在 main.c, 供心跳打印 */
extern volatile pwm_speed_t g_pwm_speed;

/**
  * @brief  PWM 调速初始化: 配 TMR2 输入捕获 + 清零观测
  */
static inline void User_PWMSpeed_Init(void)
{
  wk_tmr15_init();
  g_pwm_speed.valid       = 0;
  g_pwm_speed.period_cnt  = 0;
  g_pwm_speed.pulse_cnt   = 0;
  g_pwm_speed.duty        = 0.0f;
  g_pwm_speed.freq_khz    = 0.0f;
  g_pwm_speed.target_ubus = 0.0f;
}

/**
  * @brief  PWM 调速主循环(每轮调用一次)
  * @note   wk_tmr15 每 16 轮(0.4ms)结算一次周期; 结算成功即置 valid=1 并保持,
  *         连续 ~75ms(3000 轮)无有效结算才判信号丢失(valid=0)
  */
static inline void User_PWMSpeed_Loop(void)
{
  uint16_t period = 0, pulse = 0;
  static uint32_t s_no_sig_cnt = 0;

  if (wk_tmr15_pwm_measure(&period, &pulse))
  {
    float duty = (float)pulse / (float)period;
    float freq = 120000000.0f / (float)period / 1000.0f;  /* kHz */

    g_pwm_speed.valid       = 1;
    g_pwm_speed.period_cnt  = period;
    g_pwm_speed.pulse_cnt   = pulse;
    g_pwm_speed.duty        = duty;
    g_pwm_speed.freq_khz    = freq;
    s_no_sig_cnt = 0;

    if (duty < PWM_SPEED_STOP_DUTY)
    {
      /* 电位器旋到最低: 缓停 */
      Sguan.Func_Stop();
      g_pwm_speed.target_ubus = 0.0f;
    }
    else
    {
      /* 未运行(待机/初始化/空闲) → 启动; 运行中重复调 Start 无害会被状态机忽略 */
      if (Sguan.status <= MOTOR_STATUS_IDLE)
        Sguan.Func_Start();

      /* 电压开环: 目标电压 = 占空比 × 实际母线电压
         (未接动力电时 __Real_VBUS≈0 → 目标≈0, 接电后正常)
         Func_Set_Ubus 内部会限幅到 ±实际母线电压 */
      float target = duty * Sguan.esc.__Real_VBUS;
      Sguan.Func_Set_Ubus(target);
      g_pwm_speed.target_ubus = target;
    }
  }
  else
  {
    /* 无有效结算: 短时抖动保持 valid; 长时间无信号才判丢失 */
    if (++s_no_sig_cnt > 3000)
      g_pwm_speed.valid = 0;
  }
}

#endif /* __USER_PWMSPEED_H */
