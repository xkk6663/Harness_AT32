/**
 * @file    boot.c
 * @brief   Bootloader 启动流程 + 协议主循环（AT32F421 移植）
 *
 * 移植自 STM32-OTA-QT IAP-Bootloader User/boot.c, 按 v1.1 决策修订：
 *   - 不保留按键：KEY 触发全部移除, 纯软件串口请求（'!' 5 连 / 协议命令）
 *   - Flash 操作走平台无关 OtaFlashHal（AT32 扇区 1KB, 与页粒度一致）
 *   - 保留 printf 打印（日志与协议帧共存 USART1）
 */

#include "boot.h"
#include <stdio.h>
#include "ota_upgrade_state.h"
#include "ota_offset.h"
#include "ota_crc32.h"
#include "ota_delay.h"
#include "ota_protocol.h"
#include "ota_transport.h"
#include "ota_flash_hal.h"
#include "ota_jump.h"

/* ============================================================
 * 启动流程
 * ============================================================ */

/* 帧解析器（主循环收包用） */
static FrameParser s_Parser;

/* 擦除 APP 区域（AT32F421 扇区 1KB, 与页粒度一致, 逐扇区擦除） */
static void Erase_App_Region(uint32_t start_addr, uint16_t page_count)
{
    for (uint16_t i = 0; i < page_count; i++) {
        OtaFlashHal_ErasePage(start_addr + (uint32_t)i * 1024UL);
    }
}

/* 写入一包（128 字节 = 32 字） */
static void Write_App_Packet(uint32_t addr, const uint8_t *buf, uint16_t words)
{
    for (uint16_t i = 0; i < words; i++) {
        uint32_t word;
        memcpy(&word, buf + i * 4, 4);
        OtaFlashHal_ProgramWord(addr + (uint32_t)i * 4, word);
    }
}

void Boot_CheckState(void)
{
    UpgradeState state = UPGRADE_GetState();
    printf("\r\n=== Bootloader Start ===\r\n");
    printf("Current upgrade state: 0x%08lX\r\n", (unsigned long)state);

    /* 清空 DMA 环形缓冲区, 防止残留数据（如升级后的 'U' 字符）误触发升级 */
    uint8_t dummy;
    while (g_Transport->available() > 0) {
        g_Transport->read(&dummy);
    }

    switch (state) {

    case STATE_UPGRADE_READY:
        /* APP 请求升级：一次性擦除整片 APP 区域, 避免接收中阻塞 */
        printf("\r\n=== Upgrade requested. Erasing APP flash (%d pages)... ===\r\n",
               APP_PAGE_COUNT);
        Erase_App_Region(APP_START_ADDRESS, APP_PAGE_COUNT);
        OFFSET_Init();                          /* 清空偏移记录 */
        UPGRADE_SetState(STATE_UPGRADING);
        G_RunningCRC = CRC32_Start();
        printf("Erase done, ready to receive firmware.\r\n");
        break;

    case STATE_UPGRADE_SUCCESS:
        /* 上次升级校验通过, 跳转 APP */
        printf("\r\n=== Upgrade verified OK. Jumping to APP... ===\r\n");
        UPGRADE_SetState(STATE_RUNNING);
        OtaJump_ToApp();
        break;

    case STATE_CRC_FAIL:
        printf("\r\n=== WARNING: Previous upgrade CRC failed! ===\r\n");
        printf("Send \"!!!!!\" to retry upgrade...\r\n");
        while (1) {
            uint8_t detected = 0;
            if (g_Transport->available() > 0) {
                uint8_t byte;
                g_Transport->read(&byte);
                detected = (byte == '!');
            }
            if (detected) {
                printf("Signal detected, erasing and restarting upgrade...\r\n");
                Erase_App_Region(APP_START_ADDRESS, APP_PAGE_COUNT);
                OFFSET_Init();
                UPGRADE_SetState(STATE_UPGRADING);
                G_RunningCRC = CRC32_Start();
                G_FlashWriteOffset = 0;
                break;
            }
        }
        break;

    case STATE_UPGRADING: {
        /* 断电续传：读已完成的页数, 重算 CRC, 擦除剩余页 */
        uint16_t done_pages = OFFSET_Get();
        uint32_t done_bytes = (uint32_t)done_pages * 1024;

        if (done_pages > 0 && done_pages <= APP_PAGE_COUNT) {
            printf("\r\n=== Resuming from page %u (%lu bytes already written) ===\r\n",
                   done_pages, done_bytes);
            G_RunningCRC = CRC32_Start();
            G_RunningCRC = CRC32_Update(G_RunningCRC, (uint8_t *)APP_START_ADDRESS, done_bytes);
            Erase_App_Region(APP_START_ADDRESS + done_bytes, APP_PAGE_COUNT - done_pages);
            G_FlashWriteOffset = done_bytes;
        } else {
            printf("\r\n=== No valid resume point, starting fresh... ===\r\n");
            Erase_App_Region(APP_START_ADDRESS, APP_PAGE_COUNT);
            G_RunningCRC = CRC32_Start();
        }
        break;
    }

    case STATE_RUNNING:
        printf("\r\n=== Bootloader Ready ===\r\n");
        printf("Send \"!!!!!\" within 2s to enter upgrade mode...\r\n");
        for (int i = 0; i < 20; i++) {             /* 2 秒窗口, 等主机发 !!!!! */
            OtaDelay_Ms(100);

            uint8_t detected = 0;
            if (g_Transport->available() > 0) {
                uint8_t byte;
                /* 清空 RX 缓冲区, 直到遇到 '!' 或帧头 0xAA */
                while (g_Transport->available() > 0) {
                    g_Transport->read(&byte);
                    if (byte == '!' || byte == 0xAA) {
                        detected = 1;
                        break;
                    }
                }
            }

            if (detected) {
                printf("Signal detected, entering upgrade mode...\r\n");
                /* 准备升级：擦除 APP 区、重置偏移、进入 UPGRADING 状态 */
                Erase_App_Region(APP_START_ADDRESS, APP_PAGE_COUNT);
                OFFSET_Init();
                UPGRADE_SetState(STATE_UPGRADING);
                G_RunningCRC = CRC32_Start();
                G_FlashWriteOffset = 0;
                /* 丢弃触发信号之后的残留字节 */
                {
                    uint8_t dummy;
                    while (g_Transport->available() > 0) {
                        g_Transport->read(&dummy);
                    }
                }
                break;
            }
        }
        printf("No upgrade request, jumping to APP...\r\n");
        OtaJump_ToApp();
        break;
    }

    /* 初始化帧解析器, 准备主循环收包（OtaJump_ToApp 路径不会到达这里） */
    Protocol_Parser_Init(&s_Parser);
}

/* ============================================================
 * 主循环：协议解帧 → 提取载荷 → 满包写 Flash
 * ============================================================ */

/* 实时打印收到的字节（hex）, 仅在 Boot_ProcessRX 主循环中调用 */
static void Print_RX_Byte(uint8_t byte)
{
    printf("RX: 0x%02X", byte);
    /* 可打印字符也直接显示 ASCII */
    if (byte >= 0x20 && byte < 0x7F) {
        printf(" ('%c')", byte);
    }
    printf("\r\n");
}

void Boot_ProcessRX(void)
{
    uint8_t byte;
    while (g_Transport->read(&byte)) {
        /* 实时打印收到的每个字节 */
        Print_RX_Byte(byte);

        uint8_t result = Protocol_Parser_Feed(&s_Parser, byte);

        if (result == FRAME_CRC_ERROR) {
            /* CRC 校验失败 → 通知主机重传 */
            uint8_t pages = G_FlashWriteOffset / 1024;
            uint8_t param[3] = {1, s_Parser.seq, pages};  /* STATUS=1(ERR), SEQ, PAGES */
            Protocol_SendAck(g_Transport, RSP_ACK, param, 3);
            Protocol_Parser_Init(&s_Parser);
            continue;
        }

        if (result == FRAME_TYPE_DATA) {
            uint8_t frame_len = s_Parser.len;       /* 先保存帧信息, 再复位 */
            uint8_t frame_seq = s_Parser.seq;       /* 帧序号用于 ACK */
            Protocol_Parser_Init(&s_Parser);        /* 复位解析器, 下一帧 SOF 从 IDLE 开始 */

            /* 提取数据帧的载荷, 逐字节拷贝到组包缓冲区 */
            for (uint8_t i = 0; i < frame_len; i++) {
                G_RxBuffer[G_RxCounter++] = s_Parser.data[i];

                if (G_RxCounter >= PACKET_SIZE) {
                    /* 满一包 → 写 Flash */
                    uint32_t currentAddr = APP_START_ADDRESS + G_FlashWriteOffset;

                    G_RunningCRC = CRC32_Update(G_RunningCRC, G_RxBuffer, PACKET_SIZE);

                    Write_App_Packet(currentAddr, G_RxBuffer, PACKET_SIZE / 4);
                    G_FlashWriteOffset += PACKET_SIZE;

                    /* 每写完一页 (1024 字节) 记录偏移, 用于断电续传 */
                    if (G_FlashWriteOffset % 1024 == 0) {
                        OFFSET_Save(G_FlashWriteOffset / 1024);
                    }

                    printf("Addr 0x%08lX | RUN_CRC 0x%08lX | RBUF %u\r\n",
                           (unsigned long)currentAddr,
                           (unsigned long)G_RunningCRC,
                           (unsigned)g_Transport->available());

                    G_RxCounter = 0;
                }
            }

            /* 回 ACK = {STATUS(0=OK), SEQ, PAGES} */
            {
                uint8_t ack_pages = G_FlashWriteOffset / 1024;
                uint8_t ack_param[3] = {0, frame_seq, ack_pages};
                Protocol_SendAck(g_Transport, RSP_ACK, ack_param, 3);
            }

            /* 最后一帧（载荷不足 128 字节）或零长度帧（结束标记）→ 自动结束升级 */
            if (frame_len < PACKET_SIZE) {
                if (G_RxCounter > 0) {
                    /* 残余数据补 0xFF 写满一包 */
                    while (G_RxCounter < PACKET_SIZE) {
                        G_RxBuffer[G_RxCounter++] = 0xFF;
                    }
                    uint32_t currentAddr = APP_START_ADDRESS + G_FlashWriteOffset;
                    G_RunningCRC = CRC32_Update(G_RunningCRC, G_RxBuffer, PACKET_SIZE);
                    Write_App_Packet(currentAddr, G_RxBuffer, PACKET_SIZE / 4);
                    G_FlashWriteOffset += PACKET_SIZE;
                    G_RxCounter = 0;
                }

                G_RunningCRC = CRC32_Finish(G_RunningCRC);
                printf("\r\n=== Upgrade complete. Final CRC: 0x%08lX ===\r\n",
                       (unsigned long)G_RunningCRC);
                UPGRADE_SetState(STATE_UPGRADE_SUCCESS);
                OFFSET_Init();      /* 清空偏移记录, 下次升级从头开始 */
                printf("Reset to run APP.\r\n");
                while (1);  /* 防止后续串口噪声意外冲写 Flash */
            }

        } else if (result == FRAME_TYPE_CMD) {
            if (s_Parser.cmd == CMD_QUERY_OFFSET) {
                /* 查询当前写入页数 */
                uint16_t pages = OFFSET_Get();
                uint8_t param[2] = { pages & 0xFF, (pages >> 8) & 0xFF };
                printf("CMD_QUERY_OFFSET: %u pages\r\n", pages);
                Protocol_SendAck(g_Transport, RSP_OFFSET, param, 2);

            } else if (s_Parser.cmd == CMD_QUERY_STATE) {
                /* 查询当前升级状态 */
                UpgradeState st = UPGRADE_GetState();
                uint8_t param[4] = {
                    (uint8_t)(st & 0xFF),
                    (uint8_t)((st >> 8) & 0xFF),
                    (uint8_t)((st >> 16) & 0xFF),
                    (uint8_t)((st >> 24) & 0xFF)
                };
                printf("CMD_QUERY_STATE: 0x%08lX\r\n", (unsigned long)st);
                Protocol_SendAck(g_Transport, RSP_STATE, param, 4);

            } else if (s_Parser.cmd == CMD_RESET_UPGRADE) {
                /* 重置升级：清空偏移、擦除 APP、从头开始 */
                printf("\r\n=== CMD_RESET_UPGRADE: Restarting from scratch ===\r\n");
                Erase_App_Region(APP_START_ADDRESS, APP_PAGE_COUNT);
                OFFSET_Init();
                UPGRADE_SetState(STATE_UPGRADING);
                G_RunningCRC = CRC32_Start();
                G_FlashWriteOffset = 0;
                G_RxCounter = 0;
                {
                    uint8_t ack_param[3] = {0, 0, 0};  /* STATUS=OK, SEQ=0, PAGES=0 */
                    Protocol_SendAck(g_Transport, RSP_ACK, ack_param, 3);
                }
                printf("Erase done, ready to receive firmware.\r\n");

            } else {
                /* 未知命令 → 回复 NAK + 未知的命令字 */
                Protocol_SendAck(g_Transport, RSP_NAK, &s_Parser.cmd, 1);
            }

            Protocol_Parser_Init(&s_Parser);
        }
    }
}
