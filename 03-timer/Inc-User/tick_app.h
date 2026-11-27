/**
  ******************************************************************************
  * @file    tick_app.h
  * @brief   TIM6 5ms 时基：整个系统的"心跳"，所有周期任务由这里调度
  * @note    用户代码，Inc-User/ 目录，不受 CubeMX 重新生成影响。
  ******************************************************************************
  */
#ifndef __TICK_APP_H
#define __TICK_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

#define TICK_APP_VERSION   "v0.1.0"

/* 时基周期（ms）。必须与 CubeMX 里 TIM6 的 PSC/ARR 一致：
 * 84MHz / (83+1) / (4999+1) = 200Hz = 5ms
 * 改这里的同时要改 .ioc，否则所有基于 tick 的时间都不准。 */
#define TICK_PERIOD_MS     5u

/* 启动 TIM6 更新中断。必须在 Key_App_Init / PWM_App_Init 之后调用，
   因为中断一开就会调它们。 */
void     Tick_App_Init(void);

/* 开机以来的毫秒数（由 5ms tick 累加，分辨率 5ms）。
   注意这不是 HAL_GetTick()，后者来自 SysTick，分辨率 1ms。 */
uint32_t Tick_App_GetMs(void);

#ifdef __cplusplus
}
#endif

#endif /* __TICK_APP_H */
