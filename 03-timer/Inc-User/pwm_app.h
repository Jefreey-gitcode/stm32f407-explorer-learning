/**
  ******************************************************************************
  * @file    pwm_app.h
  * @brief   TIM14_CH1 / PF9 的 PWM 输出：呼吸灯 + 固定占空比两种模式
  * @note    用户代码，Inc-User/ 目录，不受 CubeMX 重新生成影响。
  ******************************************************************************
  */
#ifndef __PWM_APP_H
#define __PWM_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

#define PWM_APP_VERSION    "v0.1.0"

/* 与 CubeMX 里 TIM14 的配置必须一致：
 *   PSC = 83 → 84MHz / 84 = 1MHz 计数频率
 *   ARR = 499 → 1MHz / 500 = 2kHz PWM
 * 改频率时改这两个值（同时改 .ioc），不要在代码里硬写寄存器。 */
#define PWM_ARR            499u
#define PWM_CCR_MAX        (PWM_ARR + 1u)   /* 500 = 真正的 100% */

/* 呼吸上限：不到 100%，因为 DS0 全亮太刺眼，官方例程也是 300/500 = 60% */
#define PWM_BREATH_MAX     300u

/* 呼吸步进：每 10ms 改一次 CCR（5ms tick 内部 2 分频），步长 1 → 一个来回约 6 秒 */
#define PWM_STEP_DIV       2u
#define PWM_STEP           1u

typedef enum
{
    PWM_MODE_AUTO  = 0,      /* 自动呼吸 */
    PWM_MODE_FIXED = 1       /* 固定占空比（串口 pwm 命令设置） */
} PWM_Mode_t;

/* 初始化：设 0% 并启动 PWM 通道。必须在 Tick_App_Init 之前调用 */
void       PWM_App_Init(void);

/* 设置固定占空比 0~100（%），并切到 FIXED 模式。主循环调用 */
void       PWM_SetDutyPercent(uint8_t percent);

/* 切回自动呼吸，从当前亮度继续走，不跳变 */
void       PWM_SetModeAuto(void);

uint8_t    PWM_GetPercent(void);
PWM_Mode_t PWM_GetMode(void);

/* 由 TIM6 中断每 5ms 调用一次。只允许中断上下文调用 */
void       PWM_App_Tick(void);

#ifdef __cplusplus
}
#endif

#endif /* __PWM_APP_H */
