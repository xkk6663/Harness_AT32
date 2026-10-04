#ifndef OTA_COMMON_H
#define OTA_COMMON_H

#include <stdint.h>

/**
 * @defgroup OTA_Global_Constants OTA 全局常量定义
 *
 * 所有与硬件无关的全局常量集中在此文件。
 * Flash 布局、应用参数、升级状态值等只需在此处修改。
 * @{
 */

/* ── Flash Layout ─────────────────────────────────── */

/** Flash 起始地址 */
#define FLASH_BASE_ADDR         0x08000000
/** Bootloader 占用大小 (18 KB) */
#define BOOTLOADER_SIZE         0x4800
/** Bootloader 结束地址 / APP 起始地址 (0x08004800) */
#define APP_START_ADDRESS       0x08004800
/** APP 区域大小 (44 KB) */
#define APP_SIZE                0xB000

/** 断电续传偏移记录页 (页 62) */
#define OFFSET_PAGE_ADDR        0x0800F800
/** 升级状态存储页 (页 63) */
#define UPGRADE_STATE_ADDR      0x0800FC00

/* ── Application Parameters ───────────────────────── */

/** APP 占用的 Flash 页数 */
#define APP_PAGE_COUNT          44
/** 数据帧最大载荷 / Flash 编程单位 (128 字节) */
#define PACKET_SIZE             128
/** DMA 接收环形缓冲区大小 (2 的幂) */
#define DMA_RX_BUF_SIZE         1024

/* ── Upgrade States ───────────────────────────────── */

/**
 * @brief 升级状态类型
 * @note 使用 uint32_t 而非 enum，避免 ARMCC 对有符号 int 范围溢出的警告。
 */
typedef uint32_t UpgradeState;

/** @brief 正常运行，Bootloader 等待 2s 后跳转 APP */
#define STATE_RUNNING           ((UpgradeState)0xA5A5A5A0)
/** @brief APP 请求升级，Bootloader 应擦除 Flash 并接收固件 */
#define STATE_UPGRADE_READY     ((UpgradeState)0xA5A5A5A1)
/** @brief 正在接收固件数据，支持断电续传 */
#define STATE_UPGRADING         ((UpgradeState)0xA5A5A5A2)
/** @brief 固件接收完成但 CRC 校验失败 */
#define STATE_CRC_FAIL          ((UpgradeState)0xA5A5A5A3)
/** @brief 固件校验通过，下次启动可跳转运行 */
#define STATE_UPGRADE_SUCCESS   ((UpgradeState)0xA5A5A5A4)

/** @} */

#endif /* OTA_COMMON_H */
