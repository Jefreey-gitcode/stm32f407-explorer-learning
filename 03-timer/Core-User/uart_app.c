/**
  ******************************************************************************
  * @file    uart_app.c
  * @brief   USART1 application: printf redirect + IDLE/DMA frame receive
  * @note    用户代码文件，放在 Core-User/ 目录，不受 CubeMX 重新生成影响。
  *          依赖 CubeMX 生成的 huart1 和 hdma_usart1_rx（声明在 main.c/usart.c）。
  ******************************************************************************
  */
#include "uart_app.h"
#include "usart.h"      /* CubeMX 生成，内含 extern UART_HandleTypeDef huart1 */
#include "pwm_app.h"    /* pwm 命令 */
#include "key_app.h"    /* ver 命令里报按键模块版本 */
#include <stdio.h>
#include <string.h>

/* ---- 接收状态（双缓冲）----
 * dma_rx_buf : DMA 专用，硬件直接往里写，中断里读取
 * uart_rx_buf: 帧缓冲，中断拷一份给主循环处理，两者互不干扰
 */
static uint8_t    dma_rx_buf[UART_RX_BUF_SIZE];
volatile uint8_t  uart_rx_done = 0;
volatile uint16_t uart_rx_len  = 0;
uint8_t           uart_rx_buf[UART_RX_BUF_SIZE];

/* ---- printf 重定向：兼容「勾 MicroLIB」与「不勾 MicroLIB」两种设置 --------------
 * 不勾 MicroLIB 时用的是标准 ARM C 库：printf 格式化完成后调 _sys_write，
 * 而 _sys_write 会执行 BKPT 0xAB 进入半主机（semihosting），等调试器接管。
 * 独立运行（没接调试器）时没人接管，CPU 就停在 BKPT 上 —— main 卡在第一条
 * printf，表现为"串口完全没输出、程序根本不跑"。
 * 解决：链接期声明 __use_no_semihosting（让 armlink 不引入半主机实现），
 *       并补齐标准库要求的桩函数（_ttywrch / _sys_exit / _sys_command_string /
 *       FILE __stdout），最后重定向 fputc 把字符送到串口。
 * 勾了 MicroLIB 时，它的 stdio 本身不走半主机，这些都不需要。
 * 用 __MICROLIB 宏区分两种设置，以后勾不勾都能编译通过。
 * ---------------------------------------------------------------------------- */
#if !defined(__MICROLIB)

#if (__ARMCC_VERSION >= 6010050)                      /* AC6 编译器 */
__asm(".global __use_no_semihosting\n\t");
__asm(".global __ARM_use_no_argv \n\t");
#else                                                  /* AC5 编译器 */
#pragma import(__use_no_semihosting)

/* 标准库声明 FILE 为 struct __FILE，这里补上定义才能实例化 __stdout */
struct __FILE
{
    int handle;
};
#endif

/* _ttywrch 必须提供，否则链接报 L6915E（声明了不用半主机却引用调试字符通道） */
int   _ttywrch(int ch)                       { return ch; }
void  _sys_exit(int x)                       { (void)x; }
char *_sys_command_string(char *cmd, int len){ (void)cmd; (void)len; return NULL; }

FILE __stdout;

#endif /* !__MICROLIB */

/* ---- printf 重定向：寄存器直写 -----------------------------------------------
 * 阶段 3 起不再用 HAL_UART_Transmit：
 *   - 它每次都要检查/设置 huart->gState 状态机，万一在中断里被调用，
 *     而主循环正持有 gState，就会永久等待（死锁）。
 *   - 直写 DR 只等 TXE 标志，最坏情况也就是字符交错，不会锁死。
 * 仍然是阻塞的：115200 下每字符约 87µs。真正非阻塞要等阶段 4 上 TX DMA。
 * ---------------------------------------------------------------------------- */
int fputc(int ch, FILE *f)
{
    (void)f;

    while ((USART1->SR & USART_SR_TXE) == 0)
    {
        /* 等发送数据寄存器空 —— 有界等待：只要串口没被禁用，TXE 必然置位 */
    }
    USART1->DR = (uint8_t)ch;
    return ch;
}

/* ---- 初始化：清标志 + 开 IDLE 中断 + 启动 DMA 接收 ---------------------------- */
void UART_App_Init(UART_HandleTypeDef *huart)
{
    __HAL_UART_CLEAR_IDLEFLAG(huart);                       /* 先清残留 IDLE 标志 */
    __HAL_UART_ENABLE_IT(huart, UART_IT_IDLE);               /* 使能空闲中断 */
    HAL_UART_Receive_DMA(huart, dma_rx_buf, UART_RX_BUF_SIZE); /* 启动 DMA 接收 */
}

/* ---- IDLE 中断处理：一帧结束 --------------------------------------------------
 * 由 stm32f4xx_it.c 的 USART1_IRQHandler 在 HAL_UART_IRQHandler 之前转调。
 * 原理：DMA 自动把数据搬进 uart_rx_buf；总线空闲（一帧发完）触发 IDLE；
 *       用 DMA 剩余计数器反推本帧长度，标记完成，然后重启 DMA 接收下一帧。
 * ---------------------------------------------------------------------------- */
void UART_App_IDLE_Handler(UART_HandleTypeDef *huart)
{
    /* ---- 溢出错误处理（必须先于 IDLE 判断）--------------------------------
     * 触发场景：一帧超过 UART_RX_BUF_SIZE（DMA 满停搬），或上位机连发过快、
     *           线路上有噪声。ORE 一旦置位且不清，UART 会停止接收，
     *           表现为"突然再也收不到任何数据"，只能复位。
     * 清除序列按参考手册：先读 SR 再读 DR，同时清掉 ORE/FE/NE/PE。
     * -------------------------------------------------------------------- */
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_ORE) != RESET)
    {
        volatile uint32_t sr = huart->Instance->SR;
        volatile uint32_t dr = huart->Instance->DR;
        (void)sr;
        (void)dr;
    }

    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_IDLE) != RESET)
    {
        /* F407: 清 IDLE 标志需先读 SR 再读 DR（HAL 宏 __HAL_UART_CLEAR_IDLEFLAG 已封装） */
        __HAL_UART_CLEAR_IDLEFLAG(huart);

        HAL_UART_DMAStop(huart);                             /* 暂停 DMA 以读取计数器 */

        uint32_t remain = __HAL_DMA_GET_COUNTER(huart->hdmarx);
        uint16_t len = UART_RX_BUF_SIZE - remain;            /* 反推已接收字节数 */

        if (len > 0)
        {
            memcpy(uart_rx_buf, dma_rx_buf, len);            /* 拷到帧缓冲，供主循环处理 */
            uart_rx_len = len;
            uart_rx_done = 1;                                /* 通知主循环处理 */
        }

        memset(dma_rx_buf, 0, UART_RX_BUF_SIZE);             /* 清 DMA 缓冲（不动帧缓冲） */
        HAL_UART_Receive_DMA(huart, dma_rx_buf, UART_RX_BUF_SIZE); /* 重启 DMA */
    }
}

/* ---- 从帧里取出"命令体" ------------------------------------------------------
 * 上位机常勾选"加回车换行"，一帧里会带着 '\r' '\n'；而且命令后面可能跟空格。
 * 这里截到第一个 '\r'/'\n'，再去掉尾随空白，得到可以整串比较的命令。
 * ---------------------------------------------------------------------------- */
static void uart_extract_cmd(const uint8_t *frame, uint16_t len, char *cmd, uint16_t cmd_size)
{
    uint16_t i = 0;

    while (i < len && i < (uint16_t)(cmd_size - 1))
    {
        if (frame[i] == '\r' || frame[i] == '\n')
            break;
        cmd[i] = (char)frame[i];
        i++;
    }

    while (i > 0 && (cmd[i - 1] == ' ' || cmd[i - 1] == '\t'))
        i--;

    cmd[i] = '\0';
}

/* ---- 按空格/制表符切分命令 ----------------------------------------------------
 * 原地修改：分隔符替换成 '\0'，argv[] 指向 cmd 缓冲区内部，不拷贝、不占额外内存。
 * 返回 token 个数。不支持带引号的参数（当前命令用不到）。
 * ---------------------------------------------------------------------------- */
static uint8_t uart_split(char *s, char *argv[], uint8_t max)
{
    uint8_t n = 0u;

    while (*s != '\0' && n < max)
    {
        while (*s == ' ' || *s == '\t')
            s++;

        if (*s == '\0')
            break;

        argv[n++] = s;

        while (*s != '\0' && *s != ' ' && *s != '\t')
            s++;

        if (*s != '\0')
            *s++ = '\0';
    }

    return n;
}

/* ---- 严格解析非负整数 ----------------------------------------------------------
 * 只接受纯数字：拒绝 "50abc"、"−5"、""；超过 99999 也拒绝，防止后续乘法溢出。
 * ---------------------------------------------------------------------------- */
static uint8_t uart_parse_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0u;

    if (*s == '\0')
        return 0u;

    while (*s != '\0')
    {
        if (*s < '0' || *s > '9')
            return 0u;

        v = v * 10u + (uint32_t)(*s - '0');

        if (v > 99999u)
            return 0u;

        s++;
    }

    *out = v;
    return 1u;
}

/* ---- 主循环处理：收到完整一帧后回显 + 命令解析 --------------------------------
 * 命令（结尾无需回车，任意字符发完停顿即触发 IDLE）：
 *   help        -> 打印帮助
 *   ver         -> 打印各模块版本
 *   pwm <0-100> -> 设固定占空比
 *   pwm auto    -> 恢复呼吸
 *   pwm         -> 查询当前状态
 *   其它        -> 原样回显（多个 token 用单空格拼回）
 * 注意：命令名用整串比较，不是前缀比较，否则 "ver222" 会被当成 "ver"。
 * ---------------------------------------------------------------------------- */
void UART_App_Process(void)
{
    static char cmd[UART_RX_BUF_SIZE];                       /* static：不占主循环栈 */

    if (!uart_rx_done)
        return;

    uart_rx_done = 0;                                        /* 先清标志，避免重复处理 */

    uint16_t len = uart_rx_len;                              /* 取本地副本，减少共享变量访问 */

    /* 打印收到的帧 */
    printf("\r\n[RX %d bytes] ", len);
    HAL_UART_Transmit(&huart1, uart_rx_buf, len, HAL_MAX_DELAY);

    uart_extract_cmd(uart_rx_buf, len, cmd, sizeof(cmd));

    char   *argv[4];
    uint8_t argc = uart_split(cmd, argv, 4u);

    if (argc == 0u)
    {
        printf("\r\n  (empty line)\r\n");
    }
    else if (strcmp(argv[0], "help") == 0)
    {
        printf("\r\n  Commands:\r\n");
        printf("    help          - this list\r\n");
        printf("    ver           - firmware version\r\n");
        printf("    pwm <0-100>   - set fixed duty\r\n");
        printf("    pwm auto      - back to breathing\r\n");
        printf("    pwm           - show pwm status\r\n");
        printf("    <text>        - echo\r\n");
    }
    else if (strcmp(argv[0], "ver") == 0)
    {
        printf("\r\n  UART " UART_APP_VERSION " | KEY " KEY_APP_VERSION
               " | PWM " PWM_APP_VERSION "\r\n");
    }
    else if (strcmp(argv[0], "pwm") == 0)
    {
        if (argc == 1u)                                      /* 查询 */
        {
            printf("\r\n  PWM mode=%s  duty=%u%%\r\n",
                   (PWM_GetMode() == PWM_MODE_AUTO) ? "auto (breathing)" : "fixed",
                   (unsigned)PWM_GetPercent());
        }
        else if (strcmp(argv[1], "auto") == 0)                /* 恢复呼吸 */
        {
            PWM_SetModeAuto();
            printf("\r\n  PWM auto: breathing resumed\r\n");
        }
        else                                                 /* 设固定占空比 */
        {
            uint32_t v;

            if (!uart_parse_u32(argv[1], &v) || v > 100u)
            {
                printf("\r\n  ERR: pwm <0-100> | pwm auto | pwm\r\n");
            }
            else
            {
                PWM_SetDutyPercent((uint8_t)v);
                printf("\r\n  PWM fixed %lu%%\r\n", (unsigned long)v);
            }
        }
    }
    else                                                     /* 非命令：回显 */
    {
        printf("\r\n  Echo: ");
        for (uint8_t i = 0u; i < argc; i++)
            printf("%s%s", (i == 0u) ? "" : " ", argv[i]);
        printf("\r\n");
    }

    if (len >= UART_RX_BUF_SIZE)
    {
        printf("\r\n  [WARN] frame reached buffer limit (%d), tail may be lost\r\n", len);
    }

    printf("\r\n> ");                                        /* 提示符 */
}
