/**
 * @file    ota_app_hook.c
 * @brief   APP 侧 OTA 触发挂钩（v1.1: 纯软件串口请求触发）
 *
 * 停机保护链（BLDC 场景硬性安全要求, 方案文档 §4.4 第 4 步）:
 *   ota_trigger_flag == 1
 *     → Sguan.Func_Stop()              // 电机状态机停机（缓停）
 *     → tmr_output_enable(TMR1, FALSE) // 关互补 PWM 输出（六路全关, 防止桥臂直通）
 *     → UPGRADE_SetState(STATE_UPGRADE_READY)  // 写状态页（slot 磨损均衡）
 *     → 延时 ~2s                        // 与 F103 行为一致, 排空串口/让操作者感知
 *     → NVIC_SystemReset()              // 复位, Bootloader 读到 READY → 擦 APP → 收固件
 *
 * 触发扫描:
 *   USART1 IDLE 中断逐字节调用 OtaAppHook_ScanChar(), 连续 '!' ≥5 置标志。
 *   '!' 不在电机遥测协议字符集内（AO=xx? 等), 与遥测解析互不干扰（方案 §5.3）。
 */

#include "ota_app_hook.h"
#include "ota_common.h"
#include "ota_upgrade_state.h"
#include "at32f421.h"
#include "SguanESC.h"

/* 触发阈值: 连续 '!' 个数（方案文档 v1.1: 5 连触发） */
#define OTA_TRIGGER_THRESHOLD   5

static volatile uint8_t  ota_bang_count = 0;   /* 连续 '!' 计数（中断内, volatile） */
static volatile uint8_t  ota_trigger_flag = 0; /* 触发标志（主循环消费后清零） */

void OtaAppHook_ScanChar(char c)
{
    if (c == '!') {
        ota_bang_count++;
        if (ota_bang_count >= OTA_TRIGGER_THRESHOLD) {
            ota_trigger_flag = 1;
            /* 保持置位; 计数封顶避免溢出 */
            ota_bang_count = OTA_TRIGGER_THRESHOLD;
        }
    } else {
        /* 非 '!' 字符打断连续序列（遥测协议帧数据不会误触发） */
        ota_bang_count = 0;
    }
}

uint8_t OtaAppHook_IsTriggered(void)
{
    return (uint8_t)ota_trigger_flag;
}

uint8_t OtaAppHook_BangCount(void)
{
    return (uint8_t)ota_bang_count;
}

void OtaAppHook_HandleTrigger(void)
{
    uint32_t wait_ms;

    if (!ota_trigger_flag) {
        return;
    }

    /* ---- 停机保护链（主循环, 不在中断里做重活）---- */

    /* 1. 电机状态机停机（缓停, 先让电流降下来） */
    if (Sguan.Func_Stop) {
        Sguan.Func_Stop();
    }

    /* 2. 关 TMR1 互补 PWM 输出（六路全关, 防止复位瞬间桥臂直通/电机冲击） */
    tmr_output_enable(TMR1, FALSE);

    /* 3. 写升级状态页 READY（Bootloader 上电据此擦 APP 区进升级） */
    UPGRADE_SetState(STATE_UPGRADE_READY);

    /* 4. 延时 ~2s（与 F103 行为一致）: 用 SysTick 时基忙等, 无 RTOS */
    for (wait_ms = 0; wait_ms < 2000; wait_ms++) {
        volatile uint32_t i;
        for (i = 0; i < 12000; i++) { /* ~1ms @120MHz 粗延时, 停机态无精度要求 */
            __NOP();
        }
    }

    /* 5. 复位: Bootloader 接管（读到 READY → 擦 APP → 等待固件） */
    NVIC_SystemReset();

    /* 不复位条件触发到此为止; 复位后 ota_trigger_flag 回到 0 */
}
