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

int fputc(int ch, FILE *f)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
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

/* ---- 主循环处理：收到完整一帧后回显 + 简单命令解析 ----------------------------
 * 命令（结尾无需回车，任意字符发完停顿即触发 IDLE）：
 *   help  -> 打印帮助
 *   ver   -> 打印版本
 *   其它  -> 原样回显
 * 注意：命令用整串比较，不是前缀比较。否则 "ver222" 会被当成 "ver" 命令响应。
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

    if (strcmp(cmd, "help") == 0)
    {
        printf("\r\n  Commands: help | ver | <any text to echo>\r\n");
    }
    else if (strcmp(cmd, "ver") == 0)
    {
        printf("\r\n  UART App " UART_APP_VERSION "\r\n");
    }
    else if (cmd[0] == '\0')
    {
        printf("\r\n  (empty line)\r\n");
    }
    else
    {
        printf("\r\n  Echo: %s\r\n", cmd);                   /* 非命令：原样回显 */
    }

    if (len >= UART_RX_BUF_SIZE)
    {
        printf("\r\n  [WARN] frame reached buffer limit (%d), tail may be lost\r\n", len);
    }

    printf("\r\n> ");                                        /* 提示符 */
}
