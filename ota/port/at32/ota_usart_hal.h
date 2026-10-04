#ifndef OTA_USART_HAL_H
#define OTA_USART_HAL_H

#include <stdint.h>
#include "ota_transport.h"

/**
 * @brief  USART1 传输层实例（PB6=TX, PB7=RX, 115200 8N1, DMA）
 *
 * 与 APP 侧 wk_usart1_init 相同的引脚/波特率/DMA 通道：
 *   - USART1 @ PB6/PB7, 115200, 8N1, 无流控
 *   - DMA1 CH2 = TX（内存→外设）
 *   - DMA1 CH3 = RX（外设→内存, IDLE 中断）
 *
 * 接收：DMA1 CH3 非循环接收 + USART1 IDLE 空闲中断，
 *      中断内把已收字节搬入软件环形队列（主循环字节流读取）。
 * 发送：轮询 TDBE 阻塞写。
 */
extern const Transport Transport_USART1;

/** @brief 完整初始化 GPIO + USART1 + DMA（供 Transport.init 调用） */
void OtaUsartHal_Init(void);

/** @brief 关闭 USART1/DMA1 时钟（供 Transport.deinit 调用） */
void OtaUsartHal_Deinit(void);

/** @brief 从软件环形队列读一个字节（0=空, 1=成功） */
uint8_t DMA_RX_Read(uint8_t *byte);

/** @brief 环形队列中待读字节数 */
uint16_t DMA_RX_Available(void);

/** @brief USART1 IDLE 中断入口（由 USART1_IRQHandler 调用） */
void OtaUsartHal_OnIdle(void);

#endif /* OTA_USART_HAL_H */
