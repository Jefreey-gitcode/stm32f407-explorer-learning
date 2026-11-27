/**
  ******************************************************************************
  * @file    tick_app.c
  * @brief   TIM6 5ms 时基：唯一的周期中断入口，按键扫描与呼吸灯步进都挂在这里
  * @note    为什么单独一个文件：
  *          它是"时间来源"，不是"功能实现"。按键、PWM 各自只关心"被叫醒时做什么"，
  *          不关心周期性从哪来。以后换成 RTC 唤醒或 SysTick 驱动，只改这一个文件。
  ******************************************************************************
  */
#include "tick_app.h"
#include "tim.h"          /* htim6 的 extern 声明在这里 */
#include "key_app.h"      /* Key_Scan5ms() */
#include "pwm_app.h"      /* PWM_App_Tick() */
#include <stdio.h>

/* 由 5ms 中断累加，主循环读。32 位对齐变量在 Cortex-M4 上读写是原子的 */
static volatile uint32_t s_ms;

void Tick_App_Init(void)
{
    s_ms = 0u;

    /* 只负责"启动"，PSC/ARR/中断使能全在 CubeMX 生成的 MX_TIM6_Init 里配好了。
       不要在这里再调 HAL_TIM_Base_Init，那会把生成好的配置覆盖掉。 */
    HAL_TIM_Base_Start_IT(&htim6);

    printf("[TICK] TIM6 %ums base start\r\n", (unsigned)TICK_PERIOD_MS);
}

uint32_t Tick_App_GetMs(void)
{
    return s_ms;
}

/* ============================ 周期中断 ============================
 * 覆盖 HAL 的 __weak 版本。调用链：
 *   TIM6_DAC_IRQHandler (CubeMX 生成)
 *     → HAL_TIM_IRQHandler(&htim6)      判标志、清标志
 *       → HAL_TIM_PeriodElapsedCallback  ← 本函数
 *
 * 这里做的每件事都必须有确定的微秒级上界：
 *   Key_Scan5ms  = 4 次读引脚 + 几次比较，约 2µs
 *   PWM_App_Tick = 一次加法 + 一次寄存器写，约 1µs
 * 所以整个 ISR 约 3µs，5ms 周期下 CPU 占用 0.06%。
 *
 * 绝对不要在这里 printf：fputc 走 USART，主循环正在发时会等 gState，
 * 而主循环被这个中断打断着，永远等不到 → 死锁。
 * ================================================================== */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM6)
        return;                        /* TIM14 没开中断，但保留判断以防以后加 */

    s_ms += TICK_PERIOD_MS;

    Key_Scan5ms();                     /* 按键采样状态机 */
    PWM_App_Tick();                    /* 呼吸灯步进（内部 2 分频 → 10ms 一步） */
}
