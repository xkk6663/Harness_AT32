/**
  **************************************************************************
  * @file     wk_tmr15.c
  * @brief    TMR15 输入捕获驱动: PA2 = TMR15_CH1(TI1) 测量 555 PWM 脉宽/周期
  *
  * 接线: 555 输出 PWMIN1 -> R17(1k) -> PA2(TMR15_CH1 / TI1 输入)
  *
  * 关键配置:
  * 关键配置:
  *   GPIOA PA2 复用 GPIO_MUX_0 (AN0069 表5: PA2 MUX0=TMR15_CH1; MUX2 为空!)
  *   TMR15 时基: period=0xFFFF 自由运行, div=0 -> 120MHz 计数(APB2 div=1)
  *   PWM 输入模式(TMR_SELECT_CHANNEL_1):
  *     IC1 = TI1 直连 + 上升沿 -> CCR1 (周期起点)
  *     IC2 = TI1 间接 + 下降沿 -> CCR2 (脉宽终点)
  *   无任何中断, 主循环周期读捕获寄存器, 对 40kHz 电机环零干扰
  **************************************************************************
  */
#include "at32f421.h"
#include "wk_tmr15.h"

/* TMR15 计数时钟: APB2 div=1 -> 120MHz */
#define WK_TMR15_FREQ_HZ       120000000UL

/* 周期差分窗口(单轮读数): 100~8000 计数 = 15kHz~1.2MHz
   (555 无稳态频率可调范围大: 标称 100kHz, 实测 16k~165kHz 均需覆盖) */
#define WK_TMR15_PERIOD_MIN    100
#define WK_TMR15_PERIOD_MAX    8000

/* 最小差分采样窗口: 每 16 次中断(~400us≈7~30 个 PWM 周期)结算一次周期
   (采样率 40kHz 快于 PWM 周期, 相邻差分稳定 0/1 周期) */
#define WK_TMR15_AVG_ROUNDS    16

static uint16_t s_prev_ccr1 = 0;
static uint16_t s_min_period = 0;
static uint8_t  s_round_cnt  = 0;
static uint8_t  s_window_diff = 0;   /* 本窗口内是否有有效周期差分 */

/* 结算结果: TMR1 中断(40kHz)里每 16 次刷新一次, 主循环只读 */
static volatile uint16_t s_result_period = 0;
static volatile uint16_t s_result_pulse  = 0;
static volatile uint8_t  s_result_valid  = 0;

/**
  * @brief  测量结算(在 TMR1 溢出中断里调用, ~40kHz)
  * @note   采样率 40kHz >> PWM 周期(60us@16.6kHz), 相邻差分稳定为 0 或 1 个周期,
  *         周期取窗口内最小差分, 脉宽用"代数差模周期"重建
  *         开销: 2 次寄存器读 + 少量运算, ~1-2us/25us, 对电机高速环影响可忽略
  */
void wk_tmr15_isr(void)
{
  uint16_t cc1 = (uint16_t)tmr_channel_value_get(TMR15, TMR_SELECT_CHANNEL_1);
  uint16_t cc2 = (uint16_t)tmr_channel_value_get(TMR15, TMR_SELECT_CHANNEL_2);
  uint16_t p = (uint16_t)(cc1 - s_prev_ccr1);   /* 相邻上升沿间隔(回绕安全) */

  s_prev_ccr1 = cc1;

  /* 周期取窗口内最小差分 = 1 个真实周期(0/1 周期差中, 1 周期恒最小) */
  if (p >= WK_TMR15_PERIOD_MIN && p <= WK_TMR15_PERIOD_MAX)
  {
    s_window_diff = 1;
    if (s_min_period == 0 || p < s_min_period)
      s_min_period = p;
  }

  /* 每 16 次中断(400us)结算一次 */
  if (++s_round_cnt >= WK_TMR15_AVG_ROUNDS)
  {
    s_round_cnt = 0;
    if (s_window_diff && s_min_period >= WK_TMR15_PERIOD_MIN)
    {
      uint16_t t = s_min_period;
      int16_t  diff = (int16_t)(cc2 - cc1);
      /* 代数差模周期: 跨 1/2 周期都能归一到真实脉宽 */
      uint16_t w = (uint16_t)((diff % (int16_t)t + (int16_t)t) % (int16_t)t);
      s_result_period = t;
      s_result_pulse  = w;
      s_result_valid  = 1;
    }
    /* 结算失败: 保留上次结果(valid 不置 0), 由业务层 s_no_sig_cnt 超时判信号丢失,
       避免 555 频率抖动引起的间歇失败导致 valid 闪烁 */
    s_min_period = 0;
    s_window_diff = 0;
  }
}

/**
  * @brief  TMR15 输入捕获初始化
  * @note   与 ATWP 图形化界面无关: 全部由代码完成(时钟/GPIO 复用/捕获通道)
  *         TMR15 时钟未在 wk_periph_clock_config 中使能, 此处自行使能
  */
void wk_tmr15_init(void)
{
  gpio_init_type gpio_init_struct;
  tmr_input_config_type input_cfg;

  /* 1. TMR15 时钟(APB2) + GPIOA 时钟 */
  crm_periph_clock_enable(CRM_TMR15_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);

  /* 2. PA2 复用为 TMR15_CH1(TI1 输入): 555 推挽输出驱动, 输入浮空 */
  gpio_default_para_init(&gpio_init_struct);
  gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE2, GPIO_MUX_0); /* PA2 MUX0=TMR15_CH1 (AN0069 表5) */
  gpio_init_struct.gpio_pins     = GPIO_PINS_2;
  gpio_init_struct.gpio_mode     = GPIO_MODE_MUX;      /* 复用功能输入 */
  gpio_init_struct.gpio_pull     = GPIO_PULL_NONE;     /* 浮空: 信号源推挽驱动 */
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gpio_init(GPIOA, &gpio_init_struct);

  /* 3. TMR15 时基: 自由运行(period=0xFFFF), 预分频=0 -> 计数时钟 120MHz */
  tmr_base_init(TMR15, 0xFFFF, 0);
  tmr_cnt_dir_set(TMR15, TMR_COUNT_UP);
  tmr_period_value_set(TMR15, 0xFFFF);

  /* 4. PWM 输入模式(CH1): IC1=TI1 上升沿->CCR1(周期), IC2=TI1 下降沿->CCR2(脉宽)
        滤波器 0(100kHz 无需滤波; 若现场噪声大可加大到 2~4) */
  input_cfg.input_channel_select = TMR_SELECT_CHANNEL_1;
  input_cfg.input_polarity_select = TMR_INPUT_RISING_EDGE;
  input_cfg.input_mapped_select   = TMR_CC_CHANNEL_MAPPED_DIRECT;
  input_cfg.input_filter_value    = 0;
  tmr_pwm_input_config(TMR15, &input_cfg, TMR_CHANNEL_INPUT_DIV_1);

  /* 5. 启动计数 */
  tmr_counter_enable(TMR15, TRUE);
}

/**
  * @brief  读取 555 PWM 的周期/脉宽(计数)
  * @param  period: 真实周期计数(120MHz), pulse: 高电平脉宽计数
  * @retval 1=数据有效; 0=信号缺失/测量未就绪
  * @note   测量结算在 TMR1 中断(40kHz)完成, 此处只读结算结果
  */
uint8_t wk_tmr15_pwm_measure(uint16_t *period, uint16_t *pulse)
{
  if (s_result_valid)
  {
    *period = s_result_period;
    *pulse  = s_result_pulse;
    return 1;
  }
  return 0;
}
