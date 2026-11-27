/**
  ******************************************************************************
  * @file    uart_app.h
  * @brief   USART1 application: printf redirect + IDLE/DMA frame receive
  * @note    用户代码文件，放在 Inc-User/ 目录，不受 CubeMX 重新生成影响
  ******************************************************************************
  */
#ifndef __UART_APP_H
#define __UART_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

/* 接收缓冲区大小（一帧最大字节数） */
#define UART_RX_BUF_SIZE   256

/* 应用版本号，用于验收（v0.1.1：命令改为整串精确匹配，补 ORE 清除） */
#define UART_APP_VERSION   "v0.1.1"

/* 接收状态（volatile：中断与主循环之间共享） */
extern volatile uint8_t  uart_rx_done;   /* 1 = 收到完整一帧 */
extern volatile uint16_t uart_rx_len;    /* 本帧有效字节数 */
extern uint8_t           uart_rx_buf[UART_RX_BUF_SIZE];

/* 接口函数 */
void UART_App_Init(UART_HandleTypeDef *huart);       /* 使能 IDLE 中断并启动 DMA 接收 */
void UART_App_IDLE_Handler(UART_HandleTypeDef *huart);/* 在 USART1_IRQHandler 里转调 */
void UART_App_Process(void);                          /* 主循环里轮询处理收到的帧 */

#ifdef __cplusplus
}
#endif

#endif /* __UART_APP_H */
