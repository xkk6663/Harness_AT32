#ifndef __USERDATA_FUNCTION_H
#define __USERDATA_FUNCTION_H
#include <stdint.h>
/* 电机控制User用户设置·功能接口 */
/* 用户自己的CODE BEGIN Includes */
// Your code belike:
// #include "main.h"
// #include "tim.h"
// #include "usart.h"
// #include "adc.h"
// #include "Timer.h"
// #include "Sguan_printf.h"

#include "wk_tmr.h"
#include "wk_usart.h"
#include "wk_adc.h"
#include "Sguan_printf.h"
#include <at32f421_int.h>
#include "UserData_Config.h"
/* 用户自己的CODE END Includes */

/* 前向声明(static inline 被下文 User_StopMotor_DeInit 在定义前引用) */
static inline void User_PWM_SWitch(uint8_t Duty_CH, uint8_t Enable);

/**
 * @description: 1.电机上电即初始化的函数接口
 * @reminder: (此方函数->填入一些最先初始化的代码)
 * @reminder: (比如定时器中断开启、串口接收开启)
 * @return {*}
 */
static inline void User_Initial_Init(void){
    /* Your code for Initializing immediately upon powering on here */
    // 初始化定时器中断
    //tmr_interrupt_enable(TMR1, TMR_OVF_INT, TRUE);  //wk_tmr.c中已经初始化了TMR1的溢出中断，这里不需要再重复初始化


    // 启用串口DMA接收
    usart_interrupt_enable(USART1, USART_IDLE_INT, TRUE); //打开空闲中断
    // 启动DMA1通道3，设置dtcnt的值为BUF_LEN，dma搬运一个数据dtcnt-1
    dma_reset(DMA1_CHANNEL3); //重置RX DMA通道，准备接收数据
    dma_data_number_set(DMA1_CHANNEL3, BUF_LEN);
    dma_channel_enable(DMA1_CHANNEL3, TRUE);


    // User profile is like:
    // // 初始化定时器中断
    // HAL_TIM_Base_Start_IT(&htim1);
  
    // // 启用串口DMA接收
    // HAL_UARTEx_ReceiveToIdle_DMA(&huart1, Sguan_PrintfBuff, sizeof(Sguan_PrintfBuff));
    // __HAL_DMA_DISABLE_IT(huart1.hdmarx, DMA_IT_HT);
}

/**
 * @description: 2.有启动信号后才初始化的函数
 * @reminder: (此方函数->填入电机启动后才初始化的代码)
 * @reminder: (比如使能驱动、开启PWM输出)
 * @return {*}
 */
static inline void User_StartMotor_Init(void){
    /* Your code for initing TIM and gate driver and encoder and ADC here */
    //tmr_output_enable(TMR1, TRUE);  //wk_tmr.c中已经初始化了TMR1的PWM输出，这里不需要再重复初始化
    //tmr_counter_enable(TMR1, TRUE); //wk_tmr.c中已经初始化了TMR1的计数器，这里不需要再重复初始化


    // User profile is like:
    // // 开启PWM输出
    // HAL_TIM_PWM_Start(&htim1,TIM_CHANNEL_1);
    // HAL_TIM_PWM_Start(&htim1,TIM_CHANNEL_2);
    // HAL_TIM_PWM_Start(&htim1,TIM_CHANNEL_3);
}

/**
 * @description: 3.电机失能函数
 * @reminder: (此方函数->填入电机停机失能的代码)
 * @reminder: (比如关闭使能驱动、关闭PWM输出)
 * @return {*}
 */
static inline void User_StopMotor_DeInit(void){
    /* Your code for initing TIM and gate driver and encoder and ADC here */
    // Fix: 停机时三路 PWM 全部强制低(CHx+CHxN 全关, 该相悬浮), 确保失能后无输出
    User_PWM_SWitch(0, 0);
    User_PWM_SWitch(1, 0);
    User_PWM_SWitch(2, 0);

    // User profile is like:
    // 关闭PWM输出
    // 关闭栅极驱动使能
}

/**
 * @description: 4.用户读取的ADC原始数据传入函数
 * @reminder: (此方函数->填入你电机采样的ADC原始数据)
 * @reminder: (比如12位数据就是0-4095)
 * @reminder: (0是中性点数据，若无，UserData_Config.h中定义后可软件计算)
 * @reminder: (123分别对应电机UVW三相的相电压采集数据)
 * @param {int32_t} Current_CH
 * @return {*}
 */
static inline int32_t User_ReadADC_Raw(int32_t Current_CH){
    int32_t ADC_num = 0;
    switch (Current_CH){
    case 0:
        /* Your code for Motor Umid raw **Umid：虚拟中性点电压***/
        // User profile is like:
        // ADC_num = (int32_t)ADC_InjectedValues[0];
        break;
    case 1:
    ADC_num = (int32_t)ADC_InjectedValues[0];
        /* Your code for Motor Ua raw */
        // User profile is like:
        // ADC_num = (int32_t)ADC_InjectedValues[2];
        break;
    case 2:
    ADC_num = (int32_t)ADC_InjectedValues[1];
        /* Your code for Motor Ub raw */
        // User profile is like:
        // ADC_num = (int32_t)ADC_InjectedValues[1];
        break;
    case 3:
    ADC_num = (int32_t)ADC_InjectedValues[2];
        /* Your code for Motor Uc raw */
        // User profile is like:
        // ADC_num = (int32_t)ADC_InjectedValues[0];
        break;
    default:
        break;
    }
    return ADC_num;
}

/**
 * @description: 5.用户的PWM驱动接口函数
 * @reminder: (此方函数->填写你自己的驱动器PWM占空比)
 * @return {*}
 */
static inline void User_PwmDuty_Set(uint8_t Duty_CH,
                                uint32_t Duty_uvw){
    /* Your code for Motor PWM_CH0~2 duty set */
    switch (Duty_CH){
    case 0:
        tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_1, Duty_uvw);
        // User profile is like:
        // __HAL_TIM_SET_COMPARE(&htim1,TIM_CHANNEL_1,Duty_uvw);
        break;
    case 1:
         tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_2, Duty_uvw);
         // User profile is like:
        // __HAL_TIM_SET_COMPARE(&htim1,TIM_CHANNEL_2,Duty_uvw);
        break;
    case 2:
         tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_3, Duty_uvw);
        // User profile is like:
        // __HAL_TIM_SET_COMPARE(&htim1,TIM_CHANNEL_3,Duty_uvw);
        break;
    
    default:
        break;
    }
}

/**
 * @description: 6.用户的PWM使能与失能函数
 * @reminder: 互补输出，切换通道模式：使能=PWM模式A；失能=强制低输出
 * @return {*}
 */
static inline void User_PWM_SWitch(uint8_t Duty_CH,
                                    uint8_t Enable){
    /* 高级定时器互补输出，修改通道输出模式，不要操作GPIO */
    tmr_channel_select_type ch_sel;
    switch(Duty_CH)
    {
        case 0: ch_sel = TMR_SELECT_CHANNEL_1; break;
        case 1: ch_sel = TMR_SELECT_CHANNEL_2; break;
        case 2: ch_sel = TMR_SELECT_CHANNEL_3; break;
        default: return;
    }

    if (Enable != 0)
    {
        // 使能该相：切换为PWM模式A，使用设置好的CCR占空比输出PWM
       tmr_output_channel_mode_select(TMR1, ch_sel, TMR_OUTPUT_CONTROL_PWM_MODE_A);
    }
    else
    {
        // 失能该相：设置强制低，CHx 和 CHxN 全部关闭，该相悬浮
        tmr_output_channel_mode_select(TMR1, ch_sel, TMR_OUTPUT_CONTROL_FORCE_LOW);

    }
}
// /**
//  * @description: 6.用户的PWM使能与失能函数
//  * @reminder: (此方函数->填写你自己的驱动器PWM使能或者失能设置)
//  * @return {*}
//  */
// static inline void User_PWM_SWitch(uint8_t Duty_CH,
//                                 uint8_t Enable){
//     /* Your code for Motor PWM_CH0~2 duty enable */
    
//     switch (Duty_CH){
//     case 0:
//         if (Enable){
//             // User profile is like:
//             // HAL_GPIO_WritePin(SD1_GPIO_Port,SD1_Pin,GPIO_PIN_SET);
//         }
//         else{
//             // User profile is like:
//             // HAL_GPIO_WritePin(SD1_GPIO_Port,SD1_Pin,GPIO_PIN_RESET);
//         }
//         break;
//     case 1:
//         if (Enable){
//             // User profile is like:
//             // HAL_GPIO_WritePin(SD2_GPIO_Port,SD2_Pin,GPIO_PIN_SET);
//         }
//         else{
//             // User profile is like:
//             // HAL_GPIO_WritePin(SD2_GPIO_Port,SD2_Pin,GPIO_PIN_RESET);
//         }
//         break;
//     case 2:
//         if (Enable){
//             // User profile is like:
//             // HAL_GPIO_WritePin(SD3_GPIO_Port,SD3_Pin,GPIO_PIN_SET);
//         }
//         else{
//             // User profile is like:
//             // HAL_GPIO_WritePin(SD3_GPIO_Port,SD3_Pin,GPIO_PIN_RESET);
//         }
//         break;
    
//     default:
//         break;
//     }
// }




/**
 * @description: 7.用户的驱动器母线电压读取接口
 * @reminder: (此方函数->填写驱动器母线电压滤波后的数值)
 * @return {*}
 */
static inline float User_VBUS_DataGet(void){
    static float vbus_filter = 0.0f;
    static uint8_t filter_initialized = 0U;
    uint16_t raw = (uint16_t)ADC_InjectedValues[3]; // 读取ADC1的通道4的采样值
    float adc_in = (float)raw * 3.3f / 4095.0f;
    float vbus_now = adc_in * VBUS_SCALE;
    if (filter_initialized == 0U)
    {
        vbus_filter = vbus_now;
        filter_initialized = 1U;
    }
    else
    {
        vbus_filter += VBUS_ALPHA * (vbus_now - vbus_filter);
    }

    return vbus_filter;
}

/**
 * @description: 8.用户的驱动器母线电流读取接口
 * @reminder: (此方函数->填写驱动器母线电流滤波后的数值)
 * @return {*}
 */
static inline float User_CURRENT_DataGet(void){
    // float CURRENT_num = 0.0f;
    /* Your code for motor CURRENT Data return if you use it */
    
    // 如果不使用电流功能，返回0xFF800000（正常数值不会是负无穷）
    return 0xFF800000;
}

/**
 * @description: 9.用户的驱动器温度读取接口
 * @reminder: (此方函数->填写驱动器温度滤波后的数值)
 * @return {*}
 */
static inline float User_Temperature_DataGet(void){
    // float Temp_num = 0.0f;
    /* Your code for motor Temperature Data return if you use it */
    
    // 如果不使用温度功能，返回0xFF800000（正常数值不会是负无穷）
    return 0xFF800000;
}

/**
 * @description: 10.用户的通信接口设计
 * @reminder: (此方函数->填写串口或者CAN的对应接口函数)
 * @param {unsigned char} *ch
 * @param {unsigned short int} size
 * @return {*}
 */
static inline void User_CorrespondSet(uint8_t *ch, uint16_t size){
    /* Your code for UART or CAN Signal Transmit Driver */
    for(uint16_t i = 0; i < size; i++)
    {
        usart_data_transmit(USART1, ch[i]);
        /* 等待发送寄存器为空，等待TX完成 */
        while(usart_flag_get(USART1, USART_TDBE_FLAG) == RESET);
    }
    // User profile is like:
    // HAL_UART_Transmit(&huart1, ch, size, 0xFFFF);
}


#endif // USERDATA_FUNCYION_H
