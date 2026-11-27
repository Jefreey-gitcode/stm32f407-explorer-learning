/**
  ******************************************************************************
  * @file    key_app.h
  * @brief   4 键应用：5ms 采样状态机 + 长按/连发 + 事件队列
  * @note    阶段 3 起重写：从"EXTI 边沿中断 + 时间戳消抖"改成"5ms 周期采样 +
  *          连续 N 次一致"，因为边沿拿不到"按住多久"，做不出长按。
  *          四种主流开源按键库（MultiButton / FlexibleButton / easy_button / lwbtn）
  *          也全部是采样方案，边沿方案在业界只用于"唤醒"这类场景。
  ******************************************************************************
  */
#ifndef __KEY_APP_H
#define __KEY_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 引脚宏缺失时直接报错，比一串 undefined symbol 好查 */
#if !defined(KEY0_Pin) || !defined(KEY1_Pin) || !defined(KEY2_Pin) || !defined(WK_UP_Pin)
#error "KEY0_Pin undefined. Configure PE4/PE3/PE2/PA0 as GPIO_Input with User Label KEY0/KEY1/KEY2/WK_UP in CubeMX, then GENERATE CODE."
#endif

/* PF9 已经交给 TIM14_CH1 输出 PWM。如果 CubeMX 里 LED0 标签没清掉，
   这里会拦住——否则代码里误写 PF9 也能编译通过，bug 藏到运行时。 */
#ifdef LED0_Pin
#error "PF9 is driven by TIM14_CH1. Clear the LED0 User Label in CubeMX and remove all LED0 writes."
#endif

#define KEY_APP_VERSION    "v0.3.0"

#define KEY_COUNT          4

/* 采样周期，必须与 TIM6 的 5ms 一致（见 tick_app.h 的 TICK_PERIOD_MS） */
#define KEY_SCAN_MS        5u

/* 消抖：连续 3 次采样一致才认，即 15ms。
 * 取 3 而不是 2：阶段 2 实测过 44ms 的松手抖动，两拍（10ms）挡不住；
 * 取 5（25ms）开始能被人感知为"迟钝"。 */
#define KEY_DEBOUNCE_CNT   3u

/* 长按 800ms：常用区间 600~1000，明显区别于误触 */
#define KEY_LONG_MS        800u
/* 长按之后每 100ms 连发一次，与键盘自动重复一致 */
#define KEY_REPEAT_MS      100u

#define KEY_LONG_TICKS     (KEY_LONG_MS   / KEY_SCAN_MS)   /* 160 */
#define KEY_REPEAT_TICKS   (KEY_REPEAT_MS / KEY_SCAN_MS)   /* 20  */

typedef enum
{
    KEY_EV_NONE = 0,
    KEY_EV_DOWN,        /* 按下（消抖后确认） */
    KEY_EV_UP,          /* 松开 */
    KEY_EV_LONG,        /* 按住到达 800ms，只报一次 */
    KEY_EV_REPEAT       /* 长按之后每 100ms 一次 */
} Key_Evt_t;

/* 上电初始化：清队列、按当前电平对齐状态、打印参数表。
   必须在 Tick_App_Init 之前调用。 */
void Key_App_Init(void);

/* 5ms 采样一次。只允许 TIM6 中断调用 */
void Key_Scan5ms(void);

/* 主循环轮询：取空队列 → 打印 → 刷新 PF10。非阻塞 */
void Key_Process(void);

#ifdef __cplusplus
}
#endif

#endif /* __KEY_APP_H */
