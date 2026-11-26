/**
  ******************************************************************************
  * @file    key_app.c
  * @brief   4 键外部中断：EXTI 回调 + 分向消抖 + 事件队列 + 串口/LED 反馈
  * @note    ISR 里严禁 printf / HAL_Delay：
  *          - printf 走 HAL_UART_Transmit，会锁 huart->gState，主循环正在发时
  *            中断里再发就永久等待（死锁）
  *          - HAL_Delay 依赖 SysTick，而 SysTick 优先级(15)比 EXTI(2) 低，
  *            在 ISR 里调用永远等不到
  *          所以 ISR 只做"记事件"，耗时动作全部放到 Key_Process()。
  ******************************************************************************
  */
#include "key_app.h"
#include <stdio.h>

/* ============================ 硬件配置表 ============================
 * 加第 5 个键要做三件事：
 *   1. 本表加一行     2. key_app.h 里 KEY_COUNT +1
 *   3. Key_Process() 的 switch 加一个 case（LED 反馈动作）
 * 前两步是数据，第三步是"这个键按下去要做什么"，本来就得单独决定。
 * active：0 = 按下为低电平（上拉接法），1 = 按下为高电平（下拉接法）
 * ================================================================== */
typedef struct
{
    GPIO_TypeDef *port;
    uint16_t      pin;
    uint8_t       active;
    const char   *name;
} Key_Hw_t;

static const Key_Hw_t s_hw[KEY_COUNT] =
{
    { KEY0_GPIO_Port,  KEY0_Pin,  0, "KEY0"  },   /* PE4，上拉，按下为低 */
    { KEY1_GPIO_Port,  KEY1_Pin,  0, "KEY1"  },   /* PE3，上拉，按下为低 */
    { KEY2_GPIO_Port,  KEY2_Pin,  0, "KEY2"  },   /* PE2，上拉，按下为低 */
    { WK_UP_GPIO_Port, WK_UP_Pin, 1, "WK_UP" },   /* PA0，下拉，按下为高 */
};

/* ============================ 事件队列 ============================
 * 单生产者（EXTI 中断）/ 单消费者（主循环）：
 *   s_head 只有中断里写，s_tail 只有主循环写，两边不写对方的指针。
 * 深度 16 且用 & 15 取模，一条 AND 指令完成，中断里开销最小。
 * ================================================================ */
typedef struct
{
    uint8_t  id;
    uint8_t  down;      /* 1 = 按下, 0 = 松开 */
    uint32_t tick;
} Key_Event_t;

static volatile Key_Event_t s_q[16];
static volatile uint8_t     s_head;
static volatile uint8_t     s_tail;
static uint32_t             s_cnt;

/* 每个键两个状态量：
 *   s_state[i]     当前逻辑状态（1 = 处于按下）
 *   s_last_change  上一次状态翻转的时刻
 *
 * 为什么不是"按下/松开各记一个时间戳"：
 * 分向计时会让"松手抖动"漏过去——133 号 DOWN 明明紧跟 132 号 UP（间隔≈0），
 * 但它比的是"上次按下"的时间（162ms 前），于是被判为有效。
 * 改成单一时戳 + 状态位，判断变成"状态变了吗 → 变得太快吗"，两个漏洞一起堵上。 */
static uint8_t  s_state[KEY_COUNT];
static uint32_t s_last_change[KEY_COUNT];

/* ============================ EXTI 回调 ============================
 * 覆盖 HAL 里的 __weak 版本。调用链：
 *   EXTIx_IRQHandler (CubeMX 生成)
 *     → HAL_GPIO_EXTI_IRQHandler(KEYx_Pin)   清挂起标志
 *       → HAL_GPIO_EXTI_Callback(pin)        ← 本函数
 * 注意：全工程只能有一处定义，main.c 的 USER CODE 4 必须保持为空。
 * ================================================================== */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    uint32_t now = HAL_GetTick();

    for (uint8_t i = 0u; i < KEY_COUNT; i++)
    {
        if (s_hw[i].pin != pin)
            continue;

        /* 读实际电平，避免"边沿来了但电平已经变了"的误判 */
        uint8_t level = (HAL_GPIO_ReadPin(s_hw[i].port, pin) == GPIO_PIN_SET) ? 1u : 0u;
        uint8_t down  = (level == s_hw[i].active) ? 1u : 0u;

        /* ① 状态去重：和当前逻辑状态相同，说明是同向重复边沿（抖动），直接丢。
              这一条挡的是"连续两个 DOWN""连续两个 UP"。 */
        if (down == s_state[i])
            return;

        /* ② 变化太频繁：距上次状态翻转不足消抖窗口，也丢。
              这一条挡的是"松手瞬间电平弹跳"造成的假按下。 */
        if ((now - s_last_change[i]) < KEY_DEBOUNCE_MS)
            return;

        s_last_change[i] = now;
        s_state[i]      = down;

        /* 入队；满了就丢弃（不打印，保证 ISR 恒定时间） */
        uint8_t next = (uint8_t)((s_head + 1u) & 15u);
        if (next != s_tail)
        {
            s_q[s_head].id   = (uint8_t)i;
            s_q[s_head].down = down;
            s_q[s_head].tick = now;
            s_head = next;
        }
        return;
    }
    /* pin 不匹配任何按键：可能是别的 EXTI 源，直接忽略 */
}

/* ============================ 初始化 ============================
 * 顺序很关键：CubeMX 生成的 MX_GPIO_Init() 里已经调用了 HAL_NVIC_EnableIRQ，
 * 而引脚从上电浮空切到上拉/下拉的那一瞬间可能产生一个边沿。
 * 如果直接初始化队列，用户一复位就会看到一条"没人按过"的按键事件。
 * 所以：先关中断 → 清挂起位 → 清状态 → 再开中断。
 * =============================================================== */
void Key_App_Init(void)
{
    HAL_NVIC_DisableIRQ(EXTI0_IRQn);        /* WK_UP  (PA0) */
    HAL_NVIC_DisableIRQ(EXTI2_IRQn);        /* KEY2   (PE2) */
    HAL_NVIC_DisableIRQ(EXTI3_IRQn);        /* KEY1   (PE3) */
    HAL_NVIC_DisableIRQ(EXTI4_IRQn);        /* KEY0   (PE4) */

    __HAL_GPIO_EXTI_CLEAR_IT(KEY0_Pin);
    __HAL_GPIO_EXTI_CLEAR_IT(KEY1_Pin);
    __HAL_GPIO_EXTI_CLEAR_IT(KEY2_Pin);
    __HAL_GPIO_EXTI_CLEAR_IT(WK_UP_Pin);

    s_head = 0u;
    s_tail = 0u;
    s_cnt  = 0u;

    for (uint8_t i = 0u; i < KEY_COUNT; i++)
    {
        /* 按实际电平初始化逻辑状态：如果上电时手正按着某个键，
           状态就该是"按下"，否则第一次松手会被当成状态变化误报一条事件。 */
        uint8_t level = (HAL_GPIO_ReadPin(s_hw[i].port, s_hw[i].pin) == GPIO_PIN_SET) ? 1u : 0u;
        s_state[i]       = (level == s_hw[i].active) ? 1u : 0u;
        s_last_change[i] = 0u - KEY_DEBOUNCE_MS - 1u;   /* "很久以前"，保证复位后首按就能响应 */
    }

    HAL_NVIC_EnableIRQ(EXTI0_IRQn);
    HAL_NVIC_EnableIRQ(EXTI2_IRQn);
    HAL_NVIC_EnableIRQ(EXTI3_IRQn);
    HAL_NVIC_EnableIRQ(EXTI4_IRQn);

    printf("\r\n[KEY] ready " KEY_APP_VERSION
           " | KEY0=PE4 KEY1=PE3 KEY2=PE2 WK_UP=PA0 | debounce=%ums\r\n",
           (unsigned)KEY_DEBOUNCE_MS);
}

/* ============================ LED 反馈 ============================
 * 灯直接反映"当前有哪些键被按住"，而不是"按一次翻转一次"。
 *
 * 为什么不用 Toggle：Toggle 是相对操作，多键同时按会互相抵消——
 *     KEY0 DOWN → DS0 翻一次（亮）
 *     KEY2 DOWN → DS0 又翻一次（灭）
 * 结果 DS0 回到原状，看起来像"KEY0 没生效"，实测就撞到过这种情况。
 *
 * 改成按状态直接写电平，无论按几个键、按什么顺序，结果都唯一。
 *
 * 映射（板上只有 2 个灯，4 个键无法 1:1）：
 *     DS0 亮 = KEY0 或 KEY2 正被按住
 *     DS1 亮 = KEY1 或 KEY2 正被按住
 *     WK_UP 没有独立灯可分，只靠串口打印——这是 4 键 2 灯的必然取舍。
 * ================================================================== */
static void key_refresh_led(void)
{
    uint8_t on0 = (s_state[0] != 0u) || (s_state[2] != 0u);
    uint8_t on1 = (s_state[1] != 0u) || (s_state[2] != 0u);

    /* 低电平点亮 */
    HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, on0 ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, on1 ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

/* ============================ 主循环处理 ============================
 * 一次性取空队列，保证打印顺序与按键顺序一致。
 * printf 是阻塞的（fputc 走 HAL_UART_Transmit），只能在这里调用。
 * ================================================================ */
void Key_Process(void)
{
    while (s_tail != s_head)
    {
        /* 逐成员拷贝：volatile 只保证不被优化掉，不保证原子性 */
        Key_Event_t e;
        e.id   = s_q[s_tail].id;
        e.down = s_q[s_tail].down;
        e.tick = s_q[s_tail].tick;

        s_tail = (uint8_t)((s_tail + 1u) & 15u);

        s_cnt++;
        printf("[KEY] %-5s %s (cnt=%lu)\r\n",
               s_hw[e.id].name,
               e.down ? "DOWN" : "UP",
               (unsigned long)s_cnt);

        /* LED 按"当前按住状态"刷新，多键同时按也不会互相抵消 */
        key_refresh_led();

        printf("> ");
    }
}
