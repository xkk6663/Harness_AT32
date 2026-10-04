#ifndef OTA_FLASH_HAL_H
#define OTA_FLASH_HAL_H

#include <stdint.h>

/**
 * @file    ota_flash_hal.h
 * @brief   Flash 底层操作平台无关接口（移植层）
 *
 * 供 ota_flash_store.c 调用，屏蔽具体芯片 Flash 控制器差异。
 * 各平台在 ota/port/<platform>/ 下实现本接口：
 *   - STM32: 直接封装 FLASH_Unlock / FLASH_ProgramWord / FLASH_ErasePage ...
 *   - AT32 : 封装 flash_unlock / flash_word_program / flash_sector_erase ...
 */

/** @brief 解锁 Flash 写保护（编程/擦除前必须调用） */
void OtaFlashHal_Unlock(void);

/** @brief 锁定 Flash 写保护（编程/擦除完成后调用） */
void OtaFlashHal_Lock(void);

/**
 * @brief 清除 Flash 操作标志位（EOP / PGERR / WRPRTERR 等）
 *
 * 擦除/编程前调用，避免历史错误标志导致后续操作被拒。
 */
void OtaFlashHal_ClearFlags(void);

/**
 * @brief 擦除一页/扇区
 * @param  page_addr 页起始地址（必须按页/扇区对齐）
 */
void OtaFlashHal_ErasePage(uint32_t page_addr);

/**
 * @brief 写入一个 32 位字
 * @param  addr  目标地址（按字对齐）
 * @param  data  待写入数据
 */
void OtaFlashHal_ProgramWord(uint32_t addr, uint32_t data);

#endif /* OTA_FLASH_HAL_H */
