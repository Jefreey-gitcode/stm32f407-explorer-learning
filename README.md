# STM32F407 探索者 V3 学习工程集

正点原子探索者 V3（STM32F407ZGT6）的系统学习工程，按外设阶段平铺存放。
每个阶段一个独立子目录，都是从上一阶段复制后增量修改，**可以单独 clone 取用，也可以整仓对比学习**。

> 目标：一条从点灯到 RTOS / 边缘 AI 的可回退、可复现的学习路径。

## 目录结构

| 目录 | 阶段 | 内容 | 状态 |
|---|---|---|---|
| `00-blink/` | 0 | CubeMX 最小模板 + LED 闪烁（干净基线） | ✅ 已验证 |
| `01-uart/` | 1 | USART1 双向通信：printf 重定向 + IDLE/DMA 不定长接收 + 命令解析 | ✅ 已验证 |
| `02-exti/` | 2 | 4 键外部中断：EXTI 事件 + 状态去重消抖 + 事件队列 + LED 状态镜像 | ✅ 已验证 |

三个子目录内部的布局完全一致（`Src/` `Inc/` `MDK-ARM/` `Template.ioc`），
所以从一个阶段迁移到下一个阶段时，路径和工程配置都不用改。

## 环境与工具链

| 项目 | 参数 |
|---|---|
| 主控 | STM32F407ZGT6（LQFP144，1MB Flash / 192KB SRAM） |
| 开发板 | 正点原子探索者 V3，板背丝印 ATK-DNF407 V3.4 |
| CubeMX | 6.18.1 |
| 固件包 | STM32Cube FW_F4 **V1.28.3**（锁定，不要随意升级） |
| IDE | Keil MDK-ARM V5.32，**AC5** 编译器（`uAC6=0`） |
| C 库 | 标准 ARM C 库（**不勾** Use MicroLIB），已配好半主机桩函数 |
| 时钟 | HSE 8MHz → SYSCLK 168MHz，APB1 42MHz，APB2 84MHz |
| 调试 | 板载 ST-Link（SWD）；USART1 走板载 CH340，115200-8N1 |
| LED | DS0 = PF9，DS1 = PF10，**低电平点亮** |

## clone 下来后如何恢复编译（所有阶段通用）

仓库**只存自己写的代码和配置**，HAL 库（`Drivers/`）不进版本库——它能从固件包一键重新生成。

1. **装好环境**：CubeMX 6.18.1 + Keil MDK V5 + FW_F4 V1.28.3 + STM32F4xx DFP
2. **生成 HAL 库**：CubeMX 打开目标阶段的 `Template.ioc` → **GENERATE CODE**
   → 该目录下自动出现 `Drivers/`，`Src/` `Inc/` 里的 USER CODE 区内容会保留
3. **编译烧录**：Keil 打开 `MDK-ARM/Template.uvprojx` → F7 → 下载

> 顺序不能反：先补 `Drivers/` 再编译，否则满屏 `cannot open source input file "stm32f4xx_hal.h"`。

## 阶段 0：00-blink —— 点灯基线

**目标**：证明「CubeMX 配置 → Keil 编译 → 烧录 → 现象正确」这条链路通。

改动只有两处，都在 `Src/main.c` 的 USER CODE 区：

- `USER CODE BEGIN 2`：上电先点亮 DS0 做启动指示
- `USER CODE BEGIN 3`：while 循环里 DS0/DS1 交替闪烁，周期 1 秒

**验收**：两个 LED 以 1 秒周期交替亮灭。

## 阶段 1：01-uart —— 串口双向通信

**目标**：上位机能发命令、板子能回应，且**不定长接收不丢帧**（为后续 CLI 控制打基础）。

### CubeMX 里具体做了什么（从 00-blink 复制后）

只有 4 步，都在 **Pinout & Configuration** 页面完成：

| # | 界面路径 | 操作 | 结果 |
|---|---|---|---|
| 1 | Connectivity → USART1 | Mode 选 `Asynchronous` | PA9 自动变 TX、PA10 自动变 RX |
| 2 | Connectivity → USART1 → Parameter Settings | Baud Rate 填 `115200`，其余保持 8-N-1 | 生成 `huart1.Init.BaudRate = 115200` |
| 3 | System Core → DMA → Add | 选 `USART1_RX`，Mode = Normal，Data Width = Byte | 分配 DMA2 Stream2 / Channel4 |
| 4 | System Core → NVIC | 勾选 `USART1 global interrupt` | 生成 `USART1_IRQHandler` |

> **引脚不是一个个手点的**：第 1 步选完 `Asynchronous`，PA9/PA10 由 CubeMX 自动分配并标绿。
> Pinout view 上的绿色只表示"已占用"，真正的参数在左侧分类树里。

**参数落在代码哪个位置**（想核对就去这几处）：

| CubeMX 里的项 | 生成到哪 |
|---|---|
| Baud Rate 115200 / 8-N-1 | `Src/usart.c` 的 `MX_USART1_UART_Init()`：`huart1.Init.BaudRate = 115200;` |
| PA9/PA10 复用推挽 | `Src/usart.c` 的 `HAL_UART_MspInit()` → `HAL_GPIO_Init()` |
| DMA2 Stream2 | `Src/dma.c` 的 `MX_DMA_Init()` + `usart.c` 里 `__HAL_LINKDMA` |
| USART1 中断 | `Src/stm32f4xx_it.c` 的 `USART1_IRQHandler` |

> 补充：**没有配 TX DMA**，printf 走 `HAL_UART_Transmit` 阻塞发送。后续要连续发大量数据时再加。

### 关于 PA13/PA14（这两个不是我配的）

它们是 **System Core → SYS → Debug** 的设置，CubeMX 新建 F407 工程时就默认占好了：

| 引脚 | 信号名 | 作用 |
|---|---|---|
| PA13 | `SYS_JTMS-SWDIO` | SWD 数据线 |
| PA14 | `SYS_JTCK-SWCLK` | SWD 时钟线 |
| PA15 | `SYS_JTDI` | JTAG 数据输入（仅 JTAG 模式占用） |
| PB3 | `SYS_JTDO-SWO` | JTAG 数据输出 + SWO 跟踪（仅 JTAG 模式占用） |

当前 Debug 选的是 **JTAG 4 pins**（CubeMX 6.18 对 F407 的默认值），所以 4 个引脚全占。

板载 ST-Link 走 SWD，**只用到 PA13/PA14，这两个绝对不能改**——关掉就连不上芯片，只能靠复位时机抢救。PA15/PB3 是 JTAG 独占的，以后想拿它们当普通 IO，把 Debug 改成 `Serial Wire` 就能释放。

### 改动清单

| 文件 | 改了什么 |
|---|---|
| `Template.ioc` | 加 USART1、DMA2 Stream2、NVIC 使能 |
| `Src/usart.c` / `Inc/usart.h` | CubeMX 生成 |
| `Src/dma.c` / `Inc/dma.h` | CubeMX 生成 |
| `Src/main.c` | USER CODE 2 里调 `UART_App_Init(&huart1)`；主循环调 `UART_App_Process()` |
| `Src/stm32f4xx_it.c` | `USART1_IRQHandler` 里转调 `UART_App_IDLE_Handler(&huart1)` |
| **`Core-User/uart_app.c`** | **新增**：printf 重定向、IDLE 处理、命令解析 |
| **`Inc-User/uart_app.h`** | **新增**：接口声明与缓冲区大小 |
| `MDK-ARM/Template.uvprojx` | 新增 `User_App` 分组，include 路径加 `..\Inc-User` |

### 为什么要分 `Core-User/` 和 `Inc-User/`

CubeMX 生成的 `Src/` `Inc/` 会在每次 GENERATE CODE 时被刷新（USER CODE 区之外全部重写）。
**自己的业务代码放独立目录，就永远不会被覆盖**，也不用小心翼翼地把代码塞进注释块里。

代价是 Keil 工程要手工加一个 Group 和一条 include 路径——一次性配置，之后 CubeMX 6.9.1 以上版本会保留用户自建的 Group。

### 命令协议

不需要回车也能触发（IDLE 判帧：任意字符发完停顿即认为一帧结束）。

| 输入 | 响应 |
|---|---|
| `help` | 打印命令列表 |
| `ver` | 打印版本号 |
| 其它任意文本 | `Echo: <原文>` |
| 纯回车 | `(empty line)` |

**命令是整串精确匹配，不是前缀匹配**：`ver222` 会回显，不会返回版本号。

### 核心实现要点

- **双缓冲**：`dma_rx_buf` 只给 DMA 硬件写，中断里 `memcpy` 到 `uart_rx_buf` 供主循环读。
  单缓冲会在中断里清空主循环正在读的数据（曾导致"长度对、内容全是 `\0`"）。
- **长度反推**：`UART_RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(hdmarx)`，不依赖结束符。
- **必须清 ORE**：`USART1_IRQHandler` 没调 `HAL_UART_IRQHandler` 时，溢出错误要自己清
  （读 SR 再读 DR）。不清的话一帧超过缓冲区就永久停收，只能复位。
- **半主机桩函数**：不勾 MicroLIB 时，除了重定向 `fputc`，还必须声明
  `__use_no_semihosting` 并补齐 `_ttywrch` / `_sys_exit` / `_sys_command_string` / `FILE __stdout`，
  否则 `printf` 走到 `BKPT 0xAB` 就卡死，串口一个字都没有。

### 验收标准

- 复位后收到 `Hello from USART1 @ 115200 8N1 v0.1.1`
- `help` → 命令列表；`ver` → `UART App v0.1.1`
- `help1234567` → `Echo: help1234567`（**不能**被当成 help 命令）
- `ver1234567` → `Echo: ver1234567`（**不能**返回版本号）
- 纯回车 → `(empty line)`
- 一次发 300 字节 → 报 `[RX 256 bytes]` + `[WARN]`，**之后仍能继续收发**
- 再点一次 GENERATE CODE，用户目录代码仍在、功能不变

## 阶段 2：02-exti —— 四个按键的外部中断

**目标**：从"主循环轮询"跨到"中断驱动"，理解中断里什么能做、什么不能做，以及 ISR 怎么把事件安全地交给主循环。

### 按键硬件（已从官方例程与原理图核对）

| 按键 | 引脚 | 配置 | 按下电平 | EXTI 触发沿 |
|---|---|---|---|---|
| KEY0 | PE4 | 上拉 | 低 | 双边沿 |
| KEY1 | PE3 | 上拉 | 低 | 双边沿 |
| KEY2 | PE2 | 上拉 | 低 | 双边沿 |
| WK_UP | PA0 | 下拉 | 高（与另外三个相反） | 双边沿 |

### CubeMX 里具体做了什么

| # | 界面路径 | 操作 |
|---|---|---|
| 1 | Pinout view | PE4→`GPIO_EXTI4`、PE3→`GPIO_EXTI3`、PE2→`GPIO_EXTI2`、PA0→`GPIO_EXTI0` |
| 2 | System Core → GPIO | 四个引脚 Mode 选 `Rising/Falling edge`；PE2/3/4 设 Pull-up，PA0 设 Pull-down；User Label 填 `KEY0`/`KEY1`/`KEY2`/`WK_UP` |
| 3 | System Core → NVIC | 勾 `EXTI line0/2/3/4 interrupt` |
| 4 | GENERATE CODE | 生成 `EXTI0/2/3/4_IRQHandler`、GPIOE 时钟、8 个引脚宏 |

> User Label 必须填，代码里的 `KEY0_Pin`、`WK_UP_GPIO_Port` 全靠它生成。漏了会被 `key_app.h` 的 `#error` 拦下来。

### 改动清单

| 文件 | 改了什么 |
|---|---|
| **`Core-User/key_app.c`** | **新增**：EXTI 回调、状态去重消抖、事件队列、LED 状态镜像 |
| **`Inc-User/key_app.h`** | **新增**：接口、`KEY_COUNT`、消抖窗口、引脚宏守卫 |
| `Src/main.c` | USER CODE 2 加 `Key_App_Init()`；主循环改成 `Key_Process()` + `UART_App_Process()` + `HAL_Delay(1)`（**删掉了原来的 500ms 心跳**） |
| `Src/gpio.c` / `stm32f4xx_it.c` / `Inc/main.h` | CubeMX 生成，未手改 |
| `MDK-ARM/Template.uvprojx` | `User_App` 组加 `key_app.c` |

### 核心实现要点

**1. ISR 里只做三件事，绝不 printf**

`printf` 走 `HAL_UART_Transmit`，会锁 `huart->gState`；主循环正在发送时中断里再发就永久等待。`HAL_Delay` 更不行——SysTick 优先级 15 比 EXTI 低，在 ISR 里等它永远等不到。

所以 ISR 只做：读电平 → 判断是否有效 → 压队列。打印和翻灯全部放到 `Key_Process()`。

**2. 消抖 = 状态去重 + 时间窗，顺序不能反**

```c
if (down == s_state[i]) return;                        /* ① 同向重复 = 抖动 */
if ((now - s_last_change[i]) < 50) return;             /* ② 变得太快 = 抖动 */
```

只有 ① 缺 ②，松手抖动会漏过去（0ms 内 UP 又 DOWN）；只有 ② 缺 ①，同一次按下能报两次（130ms 间隔远超任何合理窗口）。两条缺一不可。

**3. LED 用"状态镜像"，不用"翻转"**

`Toggle` 是相对操作，`KEY0 DOWN` 翻一次、`KEY2 DOWN` 又翻一次，DS0 回到原状，看起来像"KEY0 没生效"。改成按当前按住状态直接写电平，多键同时按结果唯一。

**4. 上电先清残留边沿**

CubeMX 在 `MX_GPIO_Init()` 里就使能了 EXTI，而引脚从上电浮空切到上拉那一瞬可能产生边沿。`Key_App_Init()` 必须按"关中断 → 清挂起 → 清状态 → 开中断"的顺序做，否则一复位就冒一条没人按过的按键事件。

### 主循环契约（后续所有阶段都遵守）

```c
while (1)
{
    Key_Process();        /* 非阻塞：队列空就返回 */
    UART_App_Process();   /* 非阻塞：没帧就返回 */
    HAL_Delay(1);
}
```

每个 `Xxx_Process()` 必须 run-to-completion 且不阻塞。违反这条，后面加 OLED、ADC 时整个循环就会卡死。

### 验收标准

- 复位出现 `[KEY] ready v0.2.0 ... debounce=50ms`，**不应有**自发按键事件
- 按任一键：`DOWN` + `UP` 成对出现，`cnt` 连续不跳号
- 长按 5 秒：只有一组 DOWN/UP
- 多键交叉按住松开：事件顺序与操作顺序一致，LED 状态不互相抵消
- 按住 KEY0 时串口连发 `help` 10 次：全部正常返回，不卡死

### 本阶段的欠债（等阶段 3 有了定时器再还）

| 欠的 | 什么时候还 |
|---|---|
| 长按 / 双击 / 连发 | 阶段 3：需要 5ms 采样 + 状态机，边沿中断做不出来 |
| 严格消抖（按住期间的抖动） | 阶段 3：改成"连续 3 次采样一致" |
| 队列临界区保护 | 阶段 3：有定时器后才会出现跨优先级写队列 |
| `fputc` 改寄存器直写 | 阶段 3：这样即使误在 ISR 里 printf 也不会死锁 |
| EXTI 优先级 0 → 2 | 阶段 3：和定时器优先级一起梳理 |

## 上传规则（`.gitignore` 取舍）

判断标准一句话：**能靠 `.ioc` 重新生成的东西，一律不入库。**

| 入库 | 不入库 |
|---|---|
| `Template.ioc`（配置真相源） | `Drivers/`（HAL 库，约 6MB / 3000+ 文件） |
| `Src/` `Inc/`（含 CubEMX 初始化代码） | `MDK-ARM/Template/`（.o/.axf/.hex 编译产物） |
| `Core-User/` `Inc-User/`（手写业务代码） | `*.uvguix.*`（Keil 窗口布局，个人化） |
| `MDK-ARM/*.uvprojx` `*.uvoptx` | `*.map` `*.lst` `*.dep` 等中间文件 |
| `README.md` `.gitignore` | |

当前仓库共 42 个文本文件（`00-blink` 17 + `01-uart` 23 + 2 个根文件）。

## 学习路线进度

- [x] **阶段 0**：环境搭建 + 点灯 → `00-blink/`
- [x] **阶段 1**：USART1 双向串口（IDLE + DMA）→ `01-uart/`
- [x] **阶段 2**：EXTI 外部中断（按键 KEY0/KEY1/KEY2/WK_UP）→ `02-exti/`
- [ ] **阶段 3**：定时器 / PWM（呼吸灯、舵机）
- [ ] **阶段 4**：DMA + ADC（多通道采样）
- [ ] **阶段 5**：I2C / SPI（EEPROM、OLED、传感器）
- [ ] **阶段 6**：FreeRTOS 多任务
- [ ] **阶段 7**：边缘 AI（TFLite Micro / CMSIS-NN）

## 重新生成注意事项

- HSE 是 **8MHz**（板载晶振），CubeMX 默认按 25MHz 算，必须手动改回 8MHz，否则串口全是乱码
- Debug 当前是 CubeMX 默认的 **JTAG 4 pins**（占 PA13/PA14/PA15/PB3），SWD 烧录正常；
  若要把 PA15/PB3 当普通 IO，改成 **Serial Wire** 可释放这两个引脚（PA13/PA14 任何时候都不能动）
- 工程路径和文件名不要带中文
- 固件包锁定 **V1.28.3**，升级前先确认 HAL API 没变
- 保持 **不勾 Use MicroLIB**（工程已配好半主机桩函数），换设置前先看 `Core-User/uart_app.c` 里的条件编译
