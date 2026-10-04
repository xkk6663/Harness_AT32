#ifndef OTA_OFFSET_H
#define OTA_OFFSET_H

#include <stdint.h>
#include "ota_common.h"

/**
 * @brief  清空偏移记录（新升级开始时调用）
 */
void OFFSET_Init(void);

/**
 * @brief  记录已完成页数（每写完 1024 字节调用一次）
 * @param  page  已完成的页数（从 1 开始)
 */
void OFFSET_Save(uint16_t page);

/**
 * @brief  读取上次记录的页数
 * @return 页数，0 = 无记录
 */
uint16_t OFFSET_Get(void);

#endif /* OTA_OFFSET_H */
