#ifndef OTA_FLASH_STORE_H
#define OTA_FLASH_STORE_H

#include <stdint.h>

/** 每页 1KB ÷ 4B = 256 个 32 位槽位 */
#define FLASH_STORE_SLOT_COUNT  256

/**
 * @brief  读取页内最后一个有效值
 * @param  page_addr   页起始地址（必须按页对齐）
 * @param  default_val 整页为空时返回的默认值
 * @return 最后一个写入的非 0xFFFFFFFF 值，无值时返回 default_val
 */
uint32_t FlashStore_Read(uint32_t page_addr, uint32_t default_val);

/**
 * @brief  找空槽写入值（自动处理页满擦除）
 * @param  page_addr  页起始地址
 * @param  value      要写入的 32 位值
 */
void FlashStore_Write(uint32_t page_addr, uint32_t value);

/**
 * @brief  擦除整页（页内所有槽归零为 0xFFFFFFFF）
 * @param  page_addr  页起始地址
 */
void FlashStore_Erase(uint32_t page_addr);

#endif /* OTA_FLASH_STORE_H */
