/**
 * @file    ota_app_hook.h
 * @brief   APP 侧 OTA 触发挂钩（v1.1: 纯软件串口请求, 不保留按键）
 *
 * 设计（方案文档 §4.4 第 3~4 步）：
 *  - USART1 IDLE 中断里每收到一字节调用 OtaAppHook_ScanChar():
 *      统计连续 '!'（0x21）次数, ≥5 置 ota_trigger_flag。
 *      '!' 不在电机遥测 ASCII 协议字符集内, 不干扰 SguanESC_Printf_Loop。
 *  - APP 主循环调用 OtaAppHook_HandleTrigger():
 *      停机保护链 → 写升级状态 → 延时 → 复位, Bootloader 接管升级。
 */

#ifndef OTA_APP_HOOK_H
#define OTA_APP_HOOK_H

#include <stdint.h>

/* 中断内轻量扫描: 每收到一个字节调用一次 */
void OtaAppHook_ScanChar(char c);

/* 主循环响应: 触发标志置位时执行停机保护链（重量动作, 不在中断里做） */
void OtaAppHook_HandleTrigger(void);

/* 查询触发标志（可观测, 日志用） */
uint8_t OtaAppHook_IsTriggered(void);

/* 查询当前连续 '!' 计数（诊断用） */
uint8_t OtaAppHook_BangCount(void);

#endif /* OTA_APP_HOOK_H */
