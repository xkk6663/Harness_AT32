/**
 * @file    ota_usart_hal.c
 * @brief   AT32F421 USART1 传输层实现（DMA RX + IDLE 中断 + 环形队列）
 *
 * 接收路径：DMA1 CH3 非循环接收 → USART1 IDLE 空闲中断
 *   → 计算已收字节数 (BUF_LEN - dtcnt) → 搬入软件环形队列 → 重启 DMA
 * 发送路径：轮询 TDBE 阻塞写（与 APP 的 __io_putchar 一致）
 *
 * 软件环形队列：中断写 (ota_rx_head)，主循环读 (ota_rx_tail)。
 */

#include "ota_usart_hal.h"
#include "ota_common.h"
#include "at32f421.h"
#include <string.h>

/* ── 接收缓冲（DMA 目的地, 非循环） ── */
#define OTA_RX_DMA_BUF_SIZE  DMA_RX_BUF_SIZE   /* 1024 */

static uint8_t  ota_rx_dma_buf[OTA_RX_DMA_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t  ota_rx_ring[OTA_RX_DMA_BUF_SIZE];               /* 软件环形队列 */
static volatile uint16_t ota_rx_head;                           /* 中断写指针 */
static volatile uint16_t ota_rx_tail;                           /* 主循环读指针 */

/* ════════════════════════════════════════════════════════════
 * 硬件初始化（GPIO + USART1 + DMA）
 * ════════════════════════════════════════════════════════════ */

static void ota_usart_gpio_init(void)
{
    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);

    /* TX: PB6 → USART1_TX, MUX0 */
    gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE6, GPIO_MUX_0);
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
    gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
    gpio_init_struct.gpio_pins = GPIO_PINS_6;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init(GPIOB, &gpio_init_struct);

    /* RX: PB7 → USART1_RX, MUX0 */
    gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE7, GPIO_MUX_0);
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
    gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
    gpio_init_struct.gpio_pins = GPIO_PINS_7;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init(GPIOB, &gpio_init_struct);
}

static void ota_usart_dma_rx_start(void)
{
    dma_reset(DMA1_CHANNEL3);
    dma_data_number_set(DMA1_CHANNEL3, OTA_RX_DMA_BUF_SIZE);
    dma_channel_enable(DMA1_CHANNEL3, TRUE);
}

static void ota_usart_uart_init(void)
{
    usart_init(USART1, 115200, USART_DATA_8BITS, USART_STOP_1_BIT);
    usart_transmitter_enable(USART1, TRUE);
    usart_receiver_enable(USART1, TRUE);
    usart_parity_selection_config(USART1, USART_PARITY_NONE);
    usart_dma_transmitter_enable(USART1, TRUE);
    usart_dma_receiver_enable(USART1, TRUE);
    usart_hardware_flow_control_set(USART1, USART_HARDWARE_FLOW_NONE);
    usart_enable(USART1, TRUE);
}

void OtaUsartHal_Init(void)
{
    /* 外设时钟：DMA1 / GPIOB / USART1（CRM 已在 main 使能, 这里兜底） */
    crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_USART1_PERIPH_CLOCK, TRUE);

    ota_usart_gpio_init();

    /* DMA1 CH2: USART1_TX（内存→外设） */
    dma_init_type dma_init_struct;
    dma_reset(DMA1_CHANNEL2);
    dma_default_para_init(&dma_init_struct);
    dma_init_struct.direction = DMA_DIR_MEMORY_TO_PERIPHERAL;
    dma_init_struct.memory_data_width = DMA_MEMORY_DATA_WIDTH_BYTE;
    dma_init_struct.memory_inc_enable = TRUE;
    dma_init_struct.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
    dma_init_struct.peripheral_inc_enable = FALSE;
    dma_init_struct.priority = DMA_PRIORITY_LOW;
    dma_init_struct.loop_mode_enable = FALSE;
    dma_init(DMA1_CHANNEL2, &dma_init_struct);

    /* DMA1 CH3: USART1_RX（外设→内存, 非循环, IDLE 中断搬运） */
    dma_reset(DMA1_CHANNEL3);
    dma_default_para_init(&dma_init_struct);
    dma_init_struct.direction = DMA_DIR_PERIPHERAL_TO_MEMORY;
    dma_init_struct.memory_data_width = DMA_MEMORY_DATA_WIDTH_BYTE;
    dma_init_struct.memory_inc_enable = TRUE;
    dma_init_struct.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
    dma_init_struct.peripheral_inc_enable = FALSE;
    dma_init_struct.priority = DMA_PRIORITY_LOW;
    dma_init_struct.loop_mode_enable = FALSE;
    dma_init(DMA1_CHANNEL3, &dma_init_struct);

    /* DMA 通道寄存器直操作（与 APP wk_dma_channel_config 同法）：
     *   CH2 TX 目的 = USART1->dt, 传输计数 0（写前再设）
     *   CH3 RX 源 = USART1->dt, 目的 = ota_rx_dma_buf, 计数 = BUF_SIZE */
    DMA1_CHANNEL2->dtcnt = 0;
    DMA1_CHANNEL2->paddr = (uint32_t)&USART1->dt;
    DMA1_CHANNEL2->maddr = 0;

    DMA1_CHANNEL3->dtcnt = OTA_RX_DMA_BUF_SIZE;
    DMA1_CHANNEL3->paddr = (uint32_t)&USART1->dt;
    DMA1_CHANNEL3->maddr = (uint32_t)ota_rx_dma_buf;

    ota_usart_uart_init();

    /* 开启 USART1 IDLE 中断（空闲中断 + RXNE 不使能, 只收完整帧） */
    usart_interrupt_enable(USART1, USART_IDLE_INT, TRUE);

    /* 环形队列清零 */
    ota_rx_head = 0;
    ota_rx_tail = 0;

    /* 启动 DMA 接收 */
    ota_usart_dma_rx_start();
}

void OtaUsartHal_Deinit(void)
{
    usart_interrupt_enable(USART1, USART_IDLE_INT, FALSE);
    usart_enable(USART1, FALSE);
    dma_channel_enable(DMA1_CHANNEL3, FALSE);
    dma_channel_enable(DMA1_CHANNEL2, FALSE);
    crm_periph_clock_enable(CRM_USART1_PERIPH_CLOCK, FALSE);
    crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, FALSE);
}

/* ════════════════════════════════════════════════════════════
 * IDLE 中断：DMA 已收数据 → 搬入环形队列 → 重启 DMA
 * ════════════════════════════════════════════════════════════ */

void OtaUsartHal_OnIdle(void)
{
    /* AT32 空闲标志必须读 STS 再读 DT 来清除（usart_flag_clear 无效） */
    __IO uint16_t temp;
    temp = USART1->sts;
    temp = USART1->dt;
    (void)temp;

    /* 实际收到字节数 = 缓冲总长 - DMA 剩余计数 */
    uint16_t rx_len = OTA_RX_DMA_BUF_SIZE - dma_data_number_get(DMA1_CHANNEL3);
    if (rx_len > 0 && rx_len <= OTA_RX_DMA_BUF_SIZE) {
        /* 搬入环形队列（head 方向追加, 溢出时丢弃最旧数据保护指针） */
        for (uint16_t i = 0; i < rx_len; i++) {
            ota_rx_ring[ota_rx_head] = ota_rx_dma_buf[i];
            ota_rx_head = (uint16_t)((ota_rx_head + 1) & (OTA_RX_DMA_BUF_SIZE - 1));
            if (ota_rx_head == ota_rx_tail) {
                ota_rx_tail = (uint16_t)((ota_rx_tail + 1) & (OTA_RX_DMA_BUF_SIZE - 1));
            }
        }
    }

    /* 重启 DMA 接收（下一轮等待数据） */
    ota_usart_dma_rx_start();
}

/* ════════════════════════════════════════════════════════════
 * 环形队列读接口（主循环/协议层调用）
 * ════════════════════════════════════════════════════════════ */

uint16_t DMA_RX_Available(void)
{
    return (uint16_t)((ota_rx_head - ota_rx_tail) & (OTA_RX_DMA_BUF_SIZE - 1));
}

uint8_t DMA_RX_Read(uint8_t *byte)
{
    if (ota_rx_head == ota_rx_tail) {
        return 0;
    }
    *byte = ota_rx_ring[ota_rx_tail];
    ota_rx_tail = (uint16_t)((ota_rx_tail + 1) & (OTA_RX_DMA_BUF_SIZE - 1));
    return 1;
}

/* ════════════════════════════════════════════════════════════
 * Transport 实例
 * ════════════════════════════════════════════════════════════ */

static uint8_t usart1_read(uint8_t *byte)  { return DMA_RX_Read(byte); }

static void usart1_write(uint8_t byte)
{
    /* 等待发送数据缓冲空 (TDBE), 加超时防串口异常阻塞 */
    volatile uint32_t timeout = 100000;
    while (usart_flag_get(USART1, USART_TDBE_FLAG) == RESET) {
        if (--timeout == 0) { return; }
    }
    usart_data_transmit(USART1, (uint16_t)byte);
}

static uint16_t usart1_available(void)    { return DMA_RX_Available(); }
static void     usart1_init(void)         { OtaUsartHal_Init(); }
static void     usart1_deinit(void)       { OtaUsartHal_Deinit(); }

const Transport Transport_USART1 = {
    .init       = usart1_init,
    .deinit     = usart1_deinit,
    .read       = usart1_read,
    .write      = usart1_write,
    .available  = usart1_available,
};

/* ════════════════════════════════════════════════════════════
 * USART1 中断处理（Bootloader target 独占, 覆盖 startup 弱符号）
 * ════════════════════════════════════════════════════════════ */

void USART1_IRQHandler(void)
{
    if (usart_interrupt_flag_get(USART1, USART_IDLEF_FLAG) != RESET) {
        OtaUsartHal_OnIdle();
    }
}
