/**
 * @file    ota_jump.c
 * @brief   AT32F421 跳转 APP 实现
 *
 * AT32F421 为 Cortex-M4（无 SCB->VTOR 由内核提供, 直接写 SCB->VTOR 即可）。
 * APP 侧链接脚本 AT32F421x8_APP.ld 将中断向量表放在 0x08004800,
 * 跳转前将 VTOR 切到 APP 向量表。
 */

#include "ota_jump.h"
#include "ota_common.h"
#include "at32f421.h"

typedef void (*pFunction)(void);

void OtaJump_ToApp(void)
{
    /* 检查 APP 起始地址的栈顶指针是否合法（SRAM 0x20000000 范围） */
    uint32_t app_sp = *(volatile uint32_t *)APP_START_ADDRESS;
    if ((app_sp & 0x2FFE0000) == 0x20000000) {
        __disable_irq();                    /* 关全局中断 */

        /* 复位外设（尽量干净的运行环境给 APP） */
        crm_reset();

        /* 切 VTOR 到 APP 向量表 */
        SCB->VTOR = APP_START_ADDRESS;

        /* 取 APP 复位向量 + 初始化 MSP + 跳转 */
        uint32_t jump_addr = *(volatile uint32_t *)(APP_START_ADDRESS + 4);
        pFunction jump_to_app = (pFunction)jump_addr;
        __set_MSP(app_sp);
        jump_to_app();
    }
}
