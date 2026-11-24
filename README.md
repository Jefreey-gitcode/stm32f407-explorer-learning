# STM32F407 探索者 V3 学习工程集

正点原子探索者 V3（STM32F407ZGT6）的系统学习工程，按外设阶段平铺存放。
每个阶段一个独立子目录，都是从上一阶段复制后增量修改，**可以单独 clone 取用，也可以整仓对比学习**。

> 目标：一条从点灯到 RTOS / 边缘 AI 的可回退、可复现的学习路径。

## 目录结构

| 目录 | 阶段 | 内容 | 状态 |
|---|---|---|---|
| `00-blink/` | 0 | CubeMX 最小模板 + LED 闪烁（干净基线） | ✅ 已验证 |
| `01-uart/` | 1 | USART1 双向通信：printf 重定向 + IDLE/DMA 不定长接收 + 命令解析 | ✅ 已验证 |

两个子目录内部的布局完全一致（`Src/` `Inc/` `MDK-ARM/` `Template.ioc`），
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

### 外设配置

| 项目 | 配置 |
|---|---|
| USART1 | PA9 = TX，PA10 = RX，115200-8N1 |
| USART1_RX | **DMA2 Stream2 / Channel4** |
| USART1_TX | DMA2 Stream7 / Channel4（当前用阻塞发送，DMA 已配好备用） |
| 中断 | USART1 全局中断（用于 IDLE 检测） |

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
- [ ] **阶段 2**：EXTI 外部中断（按键 KEY0/KEY1/KEY2/WK_UP）→ `02-exti/`
- [ ] **阶段 3**：定时器 / PWM（呼吸灯、舵机）
- [ ] **阶段 4**：DMA + ADC（多通道采样）
- [ ] **阶段 5**：I2C / SPI（EEPROM、OLED、传感器）
- [ ] **阶段 6**：FreeRTOS 多任务
- [ ] **阶段 7**：边缘 AI（TFLite Micro / CMSIS-NN）

## 重新生成注意事项

- HSE 是 **8MHz**（板载晶振），CubeMX 默认按 25MHz 算，必须手动改回 8MHz，否则串口全是乱码
- Debug 必须设 **Serial Wire**，否则烧一次就锁芯片（只能连复位线解锁）
- 工程路径和文件名不要带中文
- 固件包锁定 **V1.28.3**，升级前先确认 HAL API 没变
- 保持 **不勾 Use MicroLIB**（工程已配好半主机桩函数），换设置前先看 `Core-User/uart_app.c` 里的条件编译
