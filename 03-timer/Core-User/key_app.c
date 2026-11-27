/**
  ******************************************************************************
  * @file    key_app.c
  * @brief   4 键应用：5ms 采样状态机 + 长按/连发 + 事件队列 + PF10 指示
  * @note    阶段 3 重写。与阶段 2 的边沿版对比：
  *            边沿版：EXTI 回调里判电平 + 50ms 拒绝窗 → 只有 DOWN/UP，做不出长按
  *            采样版：每 5ms 问一次"现在按着没" → 连续 3 次一致才认，
  *                    按住时长用计数器累计 → 长出 LONG / REPEAT
  *          ISR 里只做"读 4 个引脚 + 比较 +（必要时）压队列"，约 2µs。
  ******************************************************************************
  */
#include "key_app.h"
#include "tick_app.h"      /* Tick_App_GetMs()，用于空闲心跳 */
#include <stdio.h>

/* ============================ 硬件配置表 ============================
 * 加第 5 个键：本表加一行 + key_app.h 里 KEY_COUNT +1，核心逻辑不用动。
 * active：0 = 按下为低（上拉接法），1 = 按下为高（下拉接法）
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
    { KEY1_GPIO_Port,  KEY1_Pin,  0, "KEY1"  },   /* PE3 */
    { KEY2_GPIO_Port,  KEY2_Pin,  0, "KEY2"  },   /* PE2 */
    { WK_UP_GPIO_Port, WK_UP_Pin, 1, "WK_UP" },   /* PA0，下拉，按下为高 */
};

/* ============================ 运行时状态 ============================
 * 每个键 5 个量，共 8 字节。对比开源库（MultiButton 44B / lwbtn 48B），
 * 我们省掉的是链表指针和回调数组——键数固定 4 个，数组下标就够了。
 * ================================================================== */
typedef struct
{
    uint8_t  stable;      /* 已消抖的逻辑状态：0=释放 1=按下 */
    uint8_t  cnt;         /* 与 stable 相反的连续采样次数 */
    uint16_t hold;        /* 确认按下后持续的 tick 数（5ms/格） */
    uint16_t hold_rep;    /* 上次 REPEAT 时的 hold，用来算间隔 */
    uint8_t  long_fired;  /* LONG 只报一次 */
} Key_Runtime_t;

/* volatile：stable 由 TIM6 中断写、主循环读（key_refresh_led），
   不加的话 -O3 会把读操作缓存进寄存器，LED 就不跟着变。
   其余字段只在中断里用，但整个数组一起声明更省心，5ms 一次的开销可以忽略。 */
static volatile Key_Runtime_t s_rt[KEY_COUNT];

/* ============================ 事件队列 ============================
 * 单生产者（TIM6 中断，唯一） / 单消费者（主循环）。
 * s_head 只有 ISR 写，s_tail 只有主循环写，二者 8 位对齐 → 天然原子，
 * 所以这套 SPSC 队列不需要临界区。
 * 深度 16：4 键全按住刷 REPEAT 是 40 条/秒，约 400ms 填满；
 * 满了丢的是 REPEAT（无害），DOWN/UP 在正常打印节奏下不会丢。
 * ================================================================= */
typedef struct
{
    uint8_t  id;
    uint8_t  evt;         /* Key_Evt_t */
    uint16_t hold_ms;
} Key_Event_t;            /* 4 字节 */

static volatile Key_Event_t s_q[16];
static volatile uint8_t     s_head;
static volatile uint8_t     s_tail;
static uint32_t             s_cnt;

static void key_push(uint8_t id, uint8_t evt, uint16_t hold_ms)
{
    uint8_t next = (uint8_t)((s_head + 1u) & 15u);
    if (next == s_tail)
        return;                       /* 满：静默丢弃，保证 ISR 恒定时间 */

    s_q[s_head].id      = id;
    s_q[s_head].evt     = evt;
    s_q[s_head].hold_ms = hold_ms;
    s_head = next;
}

static uint8_t key_pop(Key_Event_t *e)
{
    if (s_tail == s_head)
        return 0u;

    /* 逐成员拷贝：volatile 只保证不被优化掉，不保证结构体赋值原子 */
    e->id      = s_q[s_tail].id;
    e->evt     = s_q[s_tail].evt;
    e->hold_ms = s_q[s_tail].hold_ms;

    s_tail = (uint8_t)((s_tail + 1u) & 15u);
    return 1u;
}

/* ============================ 5ms 采样状态机 ============================
 * 只允许 TIM6 中断调用。
 *
 * 状态只有两个（释放/按下），消抖过程用计数器表达而不是"中间状态"——
 * 中间状态只是把 cnt 从 0 数到 3 的过程换个说法，多两个状态就多两个
 * switch 分支，收益为零。
 * ==================================================================== */
void Key_Scan5ms(void)
{
    for (uint8_t i = 0u; i < KEY_COUNT; i++)
    {
        volatile Key_Runtime_t *k = &s_rt[i];

        uint8_t lv   = (HAL_GPIO_ReadPin(s_hw[i].port, s_hw[i].pin) == GPIO_PIN_SET) ? 1u : 0u;
        uint8_t down = (lv == s_hw[i].active) ? 1u : 0u;

        /* ① 与当前稳定态不一致 → 累计，够 3 次才认 */
        if (down != k->stable)
        {
            if (++k->cnt < KEY_DEBOUNCE_CNT)
                continue;

            uint16_t held = (uint16_t)(k->hold * KEY_SCAN_MS);   /* 先存住按住时长 */

            k->cnt        = 0u;
            k->stable     = down;
            k->hold       = 0u;
            k->hold_rep   = 0u;
            k->long_fired = 0u;

            key_push(i, down ? KEY_EV_DOWN : KEY_EV_UP, down ? 0u : held);
            continue;
        }

        /* ② 一致就清零，单次毛刺无法累积 */
        k->cnt = 0u;

        if (k->stable == 0u)             /* 松开状态不累计按住时长 */
            continue;

        if (k->hold < 0xFFFFu)
            k->hold++;

        /* ③ 到 800ms 报一次 LONG */
        if (!k->long_fired && k->hold >= (uint16_t)KEY_LONG_TICKS)
        {
            k->long_fired = 1u;
            k->hold_rep   = k->hold;
            key_push(i, KEY_EV_LONG, (uint16_t)(k->hold * KEY_SCAN_MS));
        }
        /* ④ 之后每 100ms 报一次 REPEAT */
        else if (k->long_fired && (uint16_t)(k->hold - k->hold_rep) >= (uint16_t)KEY_REPEAT_TICKS)
        {
            k->hold_rep = k->hold;
            key_push(i, KEY_EV_REPEAT, (uint16_t)(k->hold * KEY_SCAN_MS));
        }
    }
}

/* ============================ 初始化 ============================ */
void Key_App_Init(void)
{
    s_head = 0u;
    s_tail = 0u;
    s_cnt  = 0u;

    for (uint8_t i = 0u; i < KEY_COUNT; i++)
    {
        /* 按引脚实际电平对齐初始状态：
           如果上电时手正按着某个键，状态就该是"按下"，
           否则第一次松手会被当成状态变化而误报一条 UP。 */
        uint8_t lv = (HAL_GPIO_ReadPin(s_hw[i].port, s_hw[i].pin) == GPIO_PIN_SET) ? 1u : 0u;

        s_rt[i].stable     = (lv == s_hw[i].active) ? 1u : 0u;
        s_rt[i].cnt        = 0u;
        s_rt[i].hold       = 0u;
        s_rt[i].hold_rep   = 0u;
        s_rt[i].long_fired = 0u;
    }

    printf("[KEY] ready " KEY_APP_VERSION
           " | %u keys | scan=%ums debounce=%ums long=%ums repeat=%ums\r\n",
           (unsigned)KEY_COUNT, (unsigned)KEY_SCAN_MS,
           (unsigned)(KEY_DEBOUNCE_CNT * KEY_SCAN_MS),
           (unsigned)KEY_LONG_MS, (unsigned)KEY_REPEAT_MS);
}

/* ============================ LED 指示（只写 PF10）============================
 * PF9 已经交给 TIM14_CH1 输出 PWM，绝对不能再碰。
 *
 * 4 个键只有 1 个灯可用，映射成：
 *   任一键按住 → PF10 常亮
 *   全部松开   → PF10 按 1Hz 心跳闪
 * 心跳有两个作用：按键指示 + 不接串口也能判断 TIM6 是否在跑。
 * 具体是哪个键，由串口打印承担。
 * ========================================================================== */
static void key_refresh_led(void)
{
    uint8_t any = 0u;

    for (uint8_t i = 0u; i < KEY_COUNT; i++)
    {
        if (s_rt[i].stable)
        {
            any = 1u;
            break;
        }
    }

    uint8_t on = any ? 1u : (uint8_t)((Tick_App_GetMs() / 500u) & 1u);

    HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

/* ============================ 主循环处理 ============================ */
static const char *const s_evt_name[] = { "NONE", "DOWN", "UP", "LONG", "REPEAT" };

void Key_Process(void)
{
    Key_Event_t e;

    while (key_pop(&e))
    {
        s_cnt++;
        printf("[KEY] %-5s %-6s %4ums (cnt=%lu)\r\n",
               s_hw[e.id].name,
               s_evt_name[e.evt],
               (unsigned)e.hold_ms,
               (unsigned long)s_cnt);
        printf("> ");
    }

    /* 队列空也要刷新：空闲心跳需要持续翻转 */
    key_refresh_led();
}
