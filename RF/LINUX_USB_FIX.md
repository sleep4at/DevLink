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

## Linux 主机 USB 自动挂起（非固件改动）

现场遇到的“Host 已枚举，但首次打开 `/dev/ttyUSB*` 等待约 5 秒后报输入/输出错误”还可能由 Linux USB runtime autosuspend 引起。设备进入 `suspended` 后，当前 CH570 USB 固件/外设在该环境下不能可靠地从系统挂起状态恢复。通过 udev 将该 VID:PID 的 `power/control` 固定为 `on`，并在启用 TLP 时将设备加入其 USB 自动挂起黑名单/拒绝列表后，串口打开恢复正常。该处理是主机系统配置，不属于 `usb_uart.c` 的固件移植项，也不应改动 Windows 兼容路径。

创建 `/etc/udev/rules.d/99-ch570-dongle-no-autosuspend.rules`：

```udev
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="1a86", ATTR{idProduct}=="7523", TEST=="power/control", ATTR{power/control}="on"
```

然后执行并重新插拔 Host：

```sh
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb
```

按实际设备路径检查，例如 `/sys/bus/usb/devices/3-2/power/control` 应为 `on`，`power/runtime_status` 应保持 `active`。`3-2` 只是示例，设备路径可能变化。若启用了 TLP，在 `/etc/tlp.conf` 的 `USB_DENYLIST` 中追加 `1a86:7523`（不要覆盖已有条目），重启 TLP，并用 `tlp-stat -u` 验证；不同 TLP 版本应以本机配置注释为准。

这与下文列出的 Linux USB 固件改动是两个独立层次：固件修复 CH341 Vendor 控制请求和事件处理；udev/TLP 设置则避免操作系统将设备挂起。首次打开异常的现场根因属于后者。

## 修改位置

下列位置按当前 `RF/RF_UartDongle/APP/usb_uart.c` 的符号名说明，不依赖固定行号。当前文件还包含 USB OUT 保留/反压、断总线清理等可靠性改动；不要把旧版文档中的片段直接贴入官方代码，移植时需按完整控制流整合。

### 1. EP0 快照与事件槽位

文件开头定义 `USB_EP0_EVENT_DATA_SIZE` 为 8；USB 事件定义位于 `USB_IRQ_FLAG_NUM` 附近：

```c
#define USB_IRQ_FLAG_NUM     32
#define USB_IRQ_DATA_SLOTS   (USB_IRQ_FLAG_NUM - 1)
#define USB_IRQ_SETUP_SLOT   (USB_IRQ_FLAG_NUM - 1)
```

`usb_irq_len/pid/flag` 仍各有 32 项，EP0 快照为 `32 × 8 B`。其中索引 31 专供 EP0 同步处理，普通事件环仅使用索引 0～30，即 31 项；不能把 32 项都当作普通事件槽。相较官方 4 项事件元数据，元数据数组增加约 84 B；快照数组占 256 B，另有少量索引/诊断状态变量。

`gUsbIrqDropCount` 统计普通事件环已满时拒收的事件。队列满时不覆盖尚未处理的事件；对 OUT 端点保持 NAK，让 USB 主机稍后重试。

### 2. EP0 控制请求的当前处理路径

`USB_IRQHandler()` 中每个新的 SETUP 请求先将 EP0 的接收/发送 toggle 重置为 DATA1，并将端点暂置 NAK。随后复制 8 字节 SETUP 到 `usb_irq_ep0_snapshot[USB_IRQ_SETUP_SLOT]`，设置专用槽的 PID/长度，并立即调用 `USB_IRQProcessHandler()`；处理完成后清除专用槽标志并恢复之前的读索引。

EP0 的控制 OUT/IN 数据阶段也在 USB 中断中使用同一个专用槽同步调用 `USB_IRQProcessHandler()`。EP0 OUT 会先检查 `RB_U_TOG_OK`，重复/不同步的数据包不再重复处理。由于 EP0 事务在调用前已快照 DMA 缓冲区，处理函数不会依赖稍后被覆盖的共享 `Ep0Buffer`。

`USB_IRQProcessHandler()` 对排队事件仍会在处理 SETUP/EP0 OUT 前，将对应 8 字节快照恢复到 `Ep0Buffer`；EP0 专用槽与普通数据环共用处理函数，但索引必须分别按 `USB_IRQ_SETUP_SLOT` 和 `USB_IRQ_DATA_SLOTS` 管理。

### 3. 普通 USB 事件队列和主循环

USB 中断将一般的非 EP0 事件写入 `usb_irq_*` 环；生产索引到 `USB_IRQ_DATA_SLOTS` 后回绕，不进入专用 EP0 槽。`USB_IRQProcessHandler()` 消费普通事件时也按 31 项回绕。

`USB_StatusQuery()` 在每轮主循环中最多排空 `USB_IRQ_FLAG_NUM` 次，遇到当前读槽无事件就停止。实际普通事件环容量仍为 31；循环上限 32 不代表多出一个普通槽。该处理可及时排空 Linux `ch341` 驱动产生的控制/状态事件。

### 4. EP2 bulk OUT 数据保留与反压

当前默认是 Vendor/CH341 模式，UART 字节流主要走 EP2 bulk OUT。`USB_IRQHandler()` 对 `UIS_TOKEN_OUT | 2` 不依赖普通事件环：收到有效 toggle 后将整包复制到专用 `Ep2OUTDataBuf`，记录 `Ep2DataOUTLen/Ep2DataOUTFlag`，随后将 EP2 OUT 设为 NAK。

`USB_StatusQuery()` 在前台尝试将完整暂存包写入 `pUsbBuf`；只有整包写入成功才清除 pending 标志并恢复 EP2 OUT ACK。缓冲区空间不足时保持 NAK 和暂存数据，让 USB 主机重试，避免设备已经 ACK、软件却因队列满丢掉整包的情况。零长度包也会走 pending/提交/恢复 ACK 流程，防止 EP2 永久 NAK。

Vendor EP1 OUT 兼容路径也会保留一包并在前台整包提交后 ACK。CDC 模式下对应逻辑不同，但当前默认固件不是 CDC 模式。

### 5. USB 总线复位和初始化

收到 `RB_UIF_BUS_RST` 时，当前 ISR 清除 EP1/EP2 pending 标志和长度、清空 `pUsbBuf`、将读写索引归零并清除所有事件槽，然后重置端点响应状态。这样旧 USB 会话中的数据和 toggle 状态不会残留到重新枚举后的会话。

`USB_Init()` 的顺序是先初始化 USB 参数并创建 `pUsbBuf`，再调用 `InitUSBDevice()`（该函数配置设备并使能 USB 上拉/中断），最后使能 PFIC 的 USB IRQ。先创建缓冲区可避免 USB 已开始响应后收到 OUT 包、但 `pUsbBuf` 尚未初始化。

### 6. 描述符错误处理与新 SETUP 恢复

新 SETUP 到来时重置 EP0 toggle 和响应状态，清除上一个控制传输可能留下的 STALL。`USB_GET_DESCRIPTOR` 对不支持的描述符将 `len` 设为 `0xFF` 并立即结束该描述符分支，不再沿用上一次的 `pDescr`；处理函数随后将失败请求以 EP0 STALL 结束。当前 Vendor 字符串实现只响应已定义的索引，不提供 Microsoft OS `0xEE` 字符串。

### 7. Linux 主机侧电源管理不是固件代码

首次打开串口约 5 秒后报 I/O 错误的现场问题，最终通过关闭 Linux 对 `1a86:7523` 设备的 USB 自动挂起、并在使用 TLP 时加入其 USB denylist 解决。操作步骤见本文件前面的“Linux 主机 USB 自动挂起（非固件改动）”。这项修复需要应用到 Linux 主机，不属于上述 `usb_uart.c` 移植代码，也不应通过改变 Windows USB 描述符或协议来处理。

当前修改中仍有 Vendor 控制请求调试打印；不要把“完全没有 USB IRQ/EP0 打印”作为本版本行为。Linux USB 兼容的核心是请求得到及时、正确的处理，自动挂起则由主机电源策略单独控制。

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

