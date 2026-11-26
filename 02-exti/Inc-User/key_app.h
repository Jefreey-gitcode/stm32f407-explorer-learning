/**
  ******************************************************************************
  * @file    key_app.h
  * @brief   4 键外部中断应用：EXTI 事件 + 分向消抖 + 事件队列
  * @note    用户代码文件，放在 Inc-User/ 目录，不受 CubeMX 重新生成影响。
  *          依赖 CubeMX 在 main.h 里生成的 KEY0_Pin / WK_UP_Pin 等宏，
  *          这些宏来自 Pinout 里给引脚打的 User Label。
  ******************************************************************************
  */
#ifndef __KEY_APP_H
#define __KEY_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 引脚宏缺失时直接报错，比一串 "undefined symbol" 好查得多。
   注意：报错消息用英文，因为 Keil AC5 按本地编码读源文件，中文会变乱码。 */
#if !defined(KEY0_Pin) || !defined(KEY1_Pin) || !defined(KEY2_Pin) || !defined(WK_UP_Pin)
#error "KEY0_Pin undefined. Configure PE4/PE3/PE2/PA0 as GPIO_EXTI with User Label KEY0/KEY1/KEY2/WK_UP in CubeMX, then click GENERATE CODE."
#endif

#define KEY_APP_VERSION   "v0.2.0"

/* 消抖窗口（ms）。
   机械按键抖动一般 5~20ms，但"松手抖动"实测可达 40ms 以上（日志里见过 44ms），
   取 50 才能压住；人手最快点击间隔约 100ms，50ms 不会吞掉真实操作。 */
#define KEY_DEBOUNCE_MS   50u

/* 按键个数：加第 5 个键时改这里 + key_app.c 的配置表 + Key_Process 的 switch */
#define KEY_COUNT         4

/* 上电初始化：先关中断清掉残留边沿，再开中断并打印按键表 */
void Key_App_Init(void);

/* 主循环轮询：取空事件队列 → 串口打印 + LED 反馈（非阻塞，可连续调用） */
void Key_Process(void);

#ifdef __cplusplus
}
#endif

#endif /* __KEY_APP_H */
