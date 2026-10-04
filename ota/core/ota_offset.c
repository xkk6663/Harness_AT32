/**
 * @file    ota_offset.c
 * @brief   断电续传偏移记录管理
 *
 * 使用 FlashStore 的 slot 式磨损均衡机制，将已写入的页数
 * 存储在专用 Flash 页 (0x0800F800)，支持断电恢复。
 */

#include "ota_offset.h"
#include "ota_flash_store.h"

void OFFSET_Init(void)
{
    /* 如果页内已有有效值，先擦除整页重新开始 */
    if (FlashStore_Read(OFFSET_PAGE_ADDR, 0xFFFFFFFF) != 0xFFFFFFFF) {
        FlashStore_Erase(OFFSET_PAGE_ADDR);
    }
}

void OFFSET_Save(uint16_t page)
{
    FlashStore_Write(OFFSET_PAGE_ADDR, page);
}

uint16_t OFFSET_Get(void)
{
    uint32_t val = FlashStore_Read(OFFSET_PAGE_ADDR, 0);
    return (val <= 0xFFFF) ? (uint16_t)val : 0;
}
