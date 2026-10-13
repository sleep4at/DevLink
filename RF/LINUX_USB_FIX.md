# Linux USB 适配改动说明

本文只记录相对于官方工程
`CH570无线串口-3v1/RF/RF_UartDongle/APP/usb_uart.c`
的 Linux USB 枚举稳定性改动。

## 结论

针对 Linux 的 USB/CH341 适配，必需修改的文件只有：

```text
RF/RF_UartDongle/APP/usb_uart.c
```

`APP/include/usb_uart.h` 在修改版中还声明了 `USB_DeviceDisable()`，但该接口只服务于 Host 的 PA7 自启动 ISP，不属于 Linux USB 枚举修复；`LED_PIN` 和 `HostLedActivity()` 也属于应用层改动。单独移植下面的 Linux USB 修复逻辑时，不需要为了枚举稳定性修改该头文件；若同时启用 Host PA7 自启动功能，则需要保留 `USB_DeviceDisable()` 声明。

官方代码和修改版都使用：

```text
VID = 0x1A86
PID = 0x7523
USB_VENDOR_MODE
Linux 内核 ch341 驱动
```

因此不需要修改 VID、PID，也不需要增加 Microsoft OS 描述符。

## 修改位置

下面的行号以当前修改版 `usb_uart.c` 为参考，后续代码变化后行号可能变化，移植时应以代码上下文为准。

### 1. EP0 快照长度宏

文件开头，约第 19～21 行：

```c
#define THIS_ENDP0_SIZE         64
#define MAX_PACKET_SIZE         64
#define USB_EP0_EVENT_DATA_SIZE 8
```

官方代码没有 `USB_EP0_EVENT_DATA_SIZE`。

### 2. USB 事件队列和 EP0 快照

约第 232～243 行。

官方代码：

```c
#define USB_IRQ_FLAG_NUM     4

uint8_t usb_irq_w_idx = 0;
uint8_t usb_irq_r_idx = 0;
volatile uint8_t usb_irq_len[USB_IRQ_FLAG_NUM];
volatile uint8_t usb_irq_pid[USB_IRQ_FLAG_NUM];
volatile uint8_t usb_irq_flag[USB_IRQ_FLAG_NUM];
```

修改版：

```c
#define USB_IRQ_FLAG_NUM     32

uint8_t usb_irq_w_idx = 0;
uint8_t usb_irq_r_idx = 0;
volatile uint8_t usb_irq_len[USB_IRQ_FLAG_NUM];
volatile uint8_t usb_irq_pid[USB_IRQ_FLAG_NUM];
volatile uint8_t usb_irq_flag[USB_IRQ_FLAG_NUM];

uint8_t usb_irq_ep0_snapshot[USB_IRQ_FLAG_NUM]
                            [USB_EP0_EVENT_DATA_SIZE];
```

作用：避免 Linux 枚举期间的控制请求突发造成队列溢出，并保存每个 EP0 SETUP/OUT 请求，避免共享 `Ep0Buffer` 被后续请求覆盖。

额外 RAM 约为：

```text
事件数组增加：约 84 B
EP0 快照：32 × 8 = 256 B
合计：约 340 B
```

如果官方工程 RAM 紧张，可以先使用 `USB_IRQ_FLAG_NUM = 16` 进行测试；不建议继续使用官方的 4。

### 3. EP0 OUT 数据保存

在 `USB_IRQHandler()` 的：

```c
case UIS_TOKEN_OUT | 0:
```

约第 582～589 行，加入：

```c
uint8_t copy_len = usb_irq_len[usb_irq_w_idx];
if( copy_len > USB_EP0_EVENT_DATA_SIZE )
    copy_len = USB_EP0_EVENT_DATA_SIZE;

memcpy( usb_irq_ep0_snapshot[usb_irq_w_idx],
        Ep0Buffer,
        copy_len );
```

然后保留官方的 Toggle 检查和 EP0 NAK 设置。

### 4. 新 SETUP 请求重置 EP0 状态

在 `USB_IRQHandler()` 中：

```c
if(R8_USB_INT_ST & RB_UIS_SETUP_ACT)
```

的开头加入：

```c
R8_UEP0_CTRL =
    RB_UEP_R_TOG |
    RB_UEP_T_TOG |
    UEP_R_RES_NAK |
    UEP_T_RES_NAK;
```

作用：新的 SETUP 请求会终止旧的控制传输，清除之前的 STALL 状态并重新初始化 EP0 Toggle。

### 5. 保存 SETUP 请求快照

在同一个 SETUP 分支中，官方代码：

```c
usb_irq_flag[usb_irq_w_idx] = 1;
usb_irq_pid[usb_irq_w_idx]  = UIS_TOKEN_SETUP | 0;
usb_irq_len[usb_irq_w_idx]  = 8;
```

后面加入：

```c
memcpy( usb_irq_ep0_snapshot[usb_irq_w_idx],
        Ep0Buffer,
        USB_EP0_EVENT_DATA_SIZE );
```

### 6. SETUP 请求立即处理（当前修改版采用）

当前修改版没有让 SETUP 请求单纯等待主循环，而是在 USB 中断中立即调用处理函数，约第 621～629 行：

```c
{
    uint8_t setup_idx = usb_irq_w_idx;

    usb_irq_w_idx++;
    if(usb_irq_w_idx >= USB_IRQ_FLAG_NUM)
        usb_irq_w_idx = 0;

    usb_irq_r_idx = setup_idx;
    USB_IRQProcessHandler();

    usb_irq_flag[setup_idx] = 0;
    usb_irq_r_idx = usb_irq_w_idx;
}
```

在 `USB_IRQHandler()` 前增加函数声明：

```c
void USB_IRQProcessHandler( void );
```

这项改动用于降低 Linux EP0 控制请求等待超时的概率，但它会在 USB ISR 中执行完整的请求处理函数，存在中断处理时间较长和队列索引竞争风险。

建议移植测试顺序：

1. 先移植队列扩大、EP0 快照和 EP0 状态重置；
2. 如果仍出现约 5 秒后 `-EIO/-ETIMEDOUT`，再加入 SETUP 立即处理；
3. 每次只刷入一个版本，并用 `dmesg --ctime -w` 观察首次枚举。

### 7. 处理事件前恢复 EP0 快照

在 `USB_IRQProcessHandler()` 中取出事件后，约第 699～706 行加入：

```c
if( ((usb_irq_pid[i] & 0x3f) == (UIS_TOKEN_SETUP | 0)) ||
    ((usb_irq_pid[i] & 0x3f) == (UIS_TOKEN_OUT | 0)) )
{
    memcpy( Ep0Buffer,
            usb_irq_ep0_snapshot[i],
            USB_EP0_EVENT_DATA_SIZE );
}
```

这样处理的 SETUP 数据来自对应事件的快照，而不是当前共享 DMA 缓冲区。

### 8. 不支持的描述符显式结束处理

在 `USB_GET_DESCRIPTOR` 的描述符选择完成后，约第 1282～1287 行加入：

```c
if( len == 0xFF )
{
    break;
}
```

作用：不支持的描述符请求不再继续使用上一次的 `pDescr` 指针和旧数据。

### 9. 主循环一次清空多个事件

在 `USB_StatusQuery()` 开头，约第 1948～1957 行，将官方每次只处理一个事件的代码改为：

```c
for( i = 0; i < USB_IRQ_FLAG_NUM; ++i )
{
    uint8_t event_idx = usb_irq_r_idx;

    if( !usb_irq_flag[event_idx] )
        break;

    USB_IRQProcessHandler();
    usb_irq_flag[event_idx] = 0;
}
```

作用：Linux `ch341` 驱动枚举时产生的多个控制事件可以在一轮主循环中处理完，减少队列堆积。

## 不属于 Linux USB 修复的改动

以下修改不要为了测试 Linux 枚举而一起移植：

```text
RF_UartDongle/APP/main.c
  - Host LED 活动指示
  - PA2 按键中断
  - SysTick 活动计时

RF_UartDongle/APP/rf_uart_rx.c
  - RF credit/反压
  - RF 重传和缓冲区处理
  - RF_RxService()
  - 无线数据 LED 逻辑
```

另外，修改版中 `Ep1Buffer` 扩大、CDC 路径和 USB 数据转发改动也不是 Linux 枚举修复的必要条件。测试 CH341 Linux 兼容性时，应尽量保留官方的 Vendor 数据路径。

## 验证方法

刷入测试固件后，优先使用稳定设备路径：

```text
/dev/serial/by-id/usb-wch.cn_USB2.0_To_Serial_Port_WCH454545TS2-if00-port0
```

观察：

```bash
sudo dmesg --ctime -w
lsusb -d 1a86:7523 -v
```

成功标准：

1. 插入后只完成一次 USB reset 和枚举；
2. 不再出现约 5 秒后的 `error -110`；
3. `ch341-uart converter now attached` 后设备保持连接；
4. 第一次打开串口不需要等待 5 秒后重试。

