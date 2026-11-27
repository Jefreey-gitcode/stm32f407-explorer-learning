/**
  ******************************************************************************
  * @file    pwm_app.c
  * @brief   TIM14_CH1 / PF9 PWM：呼吸灯步进 + 串口可设的固定占空比
  * @note    极性是 LOW（DS0 低电平点亮），所以 CCR/500 = 低电平占比：
  *             CCR=0   → 引脚恒高 → 灯灭
  *             CCR=250 → 50% 低   → 平均 1.65V
  *             CCR=500 → 引脚恒低 → 最亮
  ******************************************************************************
  */
#include "pwm_app.h"
#include "tim.h"          /* htim14 的 extern 声明在这里 */
#include <stdio.h>

/* s_mode / s_duty 会被主循环（串口命令）和中断（呼吸步进）同时访问，
   所以声明成 volatile，并且在主循环侧用临界区保护"模式+占空比"的一致性。
   s_dir / s_div 只在中断里用，不需要 volatile。 */
static volatile PWM_Mode_t s_mode = PWM_MODE_AUTO;
static volatile uint16_t   s_duty;              /* 当前 CCR，0..500 */
static int8_t              s_dir = 1;
static uint8_t             s_div;

/* 把 CCR 钳在合法范围：超过 ARR 会变成"恒有效"（灯常亮），语义就错了 */
static uint16_t pwm_clamp(uint16_t ccr)
{
    return (ccr > PWM_CCR_MAX) ? (uint16_t)PWM_CCR_MAX : ccr;
}

void PWM_App_Init(void)
{
    /* 先设 0%，再启动通道：
       极性 LOW + CCR=0 → 引脚恒高 → 上电灯是灭的，不会闪一下 */
    __HAL_TIM_SET_COMPARE(&htim14, TIM_CHANNEL_1, 0u);
    s_duty = 0u;
    s_mode = PWM_MODE_AUTO;
    s_dir  = 1;
    s_div  = 0u;

    HAL_TIM_PWM_Start(&htim14, TIM_CHANNEL_1);

    printf("[PWM] TIM14_CH1/PF9 ok  %lu Hz  ARR=%u  polarity=LOW\r\n",
           2000ul, (unsigned)PWM_ARR);
}

/* ============================ 呼吸步进（中断里调用）============================
 * 只有 AUTO 模式才动。FIXED 模式下什么都不做，占空比由串口命令保持。
 * ========================================================================== */
void PWM_App_Tick(void)
{
    if (s_mode != PWM_MODE_AUTO)
        return;

    if (++s_div < PWM_STEP_DIV)        /* 5ms × 2 = 10ms 一步 */
        return;
    s_div = 0u;

    int16_t d = (int16_t)s_duty + (int16_t)s_dir;

    if (d >= (int16_t)PWM_BREATH_MAX)
    {
        d = (int16_t)PWM_BREATH_MAX;
        s_dir = -1;
    }
    else if (d <= 0)
    {
        d = 0;
        s_dir = 1;
    }

    __HAL_TIM_SET_COMPARE(&htim14, TIM_CHANNEL_1, (uint16_t)d);
    s_duty = (uint16_t)d;
}

/* ============================ 主循环接口 ============================ */

void PWM_SetDutyPercent(uint8_t percent)
{
    if (percent > 100u)
        return;

    /* 百分比 → CCR：(percent × 500 + 50) / 100，+50 是四舍五入 */
    uint16_t ccr = pwm_clamp((uint16_t)(((uint32_t)percent * PWM_CCR_MAX + 50u) / 100u));

    /* 临界区：模式和占空比必须一起改。
       否则中断可能在这个空档里按 AUTO 模式再改一次 CCR，把刚设的值覆盖掉。 */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    s_mode = PWM_MODE_FIXED;
    __HAL_TIM_SET_COMPARE(&htim14, TIM_CHANNEL_1, ccr);
    s_duty = ccr;

    __set_PRIMASK(primask);            /* 恢复，不是"开中断" */
}

void PWM_SetModeAuto(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    s_mode = PWM_MODE_AUTO;

    /* 如果当前亮度超过呼吸上限（比如刚做过 100% 的启动指示），
       先压回上限，避免第一拍被 clamp 出一次可见的亮度跳变。 */
    if (s_duty > PWM_BREATH_MAX)
    {
        s_duty = (uint16_t)PWM_BREATH_MAX;
        __HAL_TIM_SET_COMPARE(&htim14, TIM_CHANNEL_1, s_duty);
    }

    /* 从当前亮度继续：在上升段就继续往上，在下降段就继续往下，避免跳变 */
    s_dir = (s_duty >= (PWM_BREATH_MAX / 2u)) ? -1 : 1;
    s_div = 0u;

    __set_PRIMASK(primask);
}

uint8_t PWM_GetPercent(void)
{
    return (uint8_t)(((uint32_t)s_duty * 100u + PWM_CCR_MAX / 2u) / PWM_CCR_MAX);
}

PWM_Mode_t PWM_GetMode(void)
{
    return s_mode;
}
