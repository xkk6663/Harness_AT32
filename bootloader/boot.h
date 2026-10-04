#ifndef BOOT_H
#define BOOT_H

#include <stdint.h>
#include "ota_common.h"

/* ============================================================
 * Bootloader 启动流程 API
 * ============================================================ */

/* 全局变量（由 main.c 定义, boot.c 和 main.c 共用） */
extern uint8_t              G_RxBuffer[PACKET_SIZE];
extern uint16_t             G_RxCounter;
extern uint32_t             G_RunningCRC;
extern uint32_t             G_FlashWriteOffset;

/* 检查升级状态并执行对应启动流程 */
void Boot_CheckState(void);

/* 收字节组包 → 满包写 Flash（主循环体） */
void Boot_ProcessRX(void);

#endif /* BOOT_H */
