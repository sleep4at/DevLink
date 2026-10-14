# 单角色固件

当前拆分为两个互斥的固件：

- `RF_Uart`：Device 固件，连接目标 MCU。PA0/PA1 为目标 UART，PA2 为目标 RESET，PA7 为目标 BOOT（高有效），PA3 为双 LED 状态输出。目标 UART 环形缓冲区保持 3 KiB。
- `RF_UartDongle`：Host 固件，连接 USB。PA2 为接地触发的远端下载请求键，PA7 为接地触发的本机 Bootloader 键，PA3 为双 LED 状态输出。Host RF 接收环形缓冲区为 1 KiB。

两份固件不再保存或切换运行角色，因此不会同时编译/运行 USB 和目标 UART 两套业务路径。Host 的 PA2 请求通过状态响应的 `resv` 控制位传给 Device；Device 收到单字节 `0x7F` 后在主循环执行 BOOT/RESET 脉冲，并在脉冲完成后再向目标 UART 发送 `0x7F`。

Device 的 PA7 同时接地触发本机按键。PA7 配置为上拉输入和下降沿中断；中断只记录按键时间，主循环去抖并要求持续按住 3 秒。达到门限后 PA3 快速闪烁提示“松开将擦除 Flash”，松开才执行擦除；短按会取消。入口在 `__HIGH_CODE` 中擦除 `0x0000` 起始的一个 4 KiB 块，随后执行 FlashROM 复位准备和系统软件复位，以便 ROM ISP 接管。**这是破坏性入口**：当前 Device 镜像从 `0x0000` 启动，擦除会破坏启动代码；Bootloader 必须启用，恢复时须烧录包含 `0x0000` 启动内容的完整 HEX。进入本机 Bootloader 时不会执行目标 MCU 的 PA2/PA7 脉冲。

Host 的 PA7 使用相同的按下确认、25 ms 去抖和松开复位流程；复位前先在普通任务中关闭应用 USB，并按参考工程等待约 20 ms，再由 RAM 中的函数擦除 stub 并复位，Host USB 由 ROM Bootloader 重新枚举。PA2 的远端下载请求路径不受影响。

Host 的链接布局为：`0x0000` 放置 4 字节 `boot_stub.S` 跳转，应用从 `0x2000` 开始；Host 自启动 ISP 时只擦除 `0x0000` 起始的 4 KiB。Device 没有独立 boot stub，应用从 `0x0000` 启动，同样通过擦除首个 4 KiB 后复位进入 ROM ISP。两种入口都是破坏性操作，ISP 超时后必须重新烧录完整 HEX 才能恢复应用。

PA3 指示规则：未连接时约 1 Hz 闪烁，连接后角色灯常亮；有 USB/RF 有效数据时约 50 ms 切换到另一只 LED，随后恢复角色灯。Host 使用输出高电平对应的 LED，Device 使用输出低电平对应的 LED，浮空时两灯熄灭。

LED 闪烁使用主频自由运行计数器；已在启动时初始化，不依赖未配置的 SysTick 默认状态。

注意：单角色方案取消了 PA7 长按角色切换功能；切换角色需要重新烧录另一份固件。

数据保护：Device 在状态请求和数据包中按 16 字节单位报告 UART 输出队列的可用额度，Host 按额度读取 USB 数据，并在 ACK 中返回 Host RF 接收队列额度；状态响应 `resv` bit 0 保留远端下载命令，bit 7 表示额度有效，bit 1..6 以 32 字节单位报告 Host 额度。旧版 Host 不置有效位时，Device 按旧协议继续工作。Device 根据 Host 额度限制目标 UART 上行包长度，额度为 0 时保留 UART 队列数据并发送状态探测，不会继续盲发。Host 若仍遇到整包无法入队，会不确认该包并在短重试窗口重新监听。缓冲区额度可避免正常反压下溢出，但目标 UART 没有硬件流控，持续输入超过 3 KiB 队列容量仍可能丢数据；无线重试耗尽会断开并重新配对，不承诺无限时无损。

当前主要缓冲区：Device 目标 UART 3 KiB、Device RF 输出 512 B；Host USB 输入 512 B、Host RF 接收 1024 B。当前构建链接器报告 RAM 使用：Device 11220/12288 B（名义剩余 1068 B），Host 10824/12288 B（名义剩余 1464 B）。该数值是链接器 RAM 区域占用差，不是运行时栈高水位测量；工程没有栈水位测试。Host 增加了 USB IRQ 调试输出抑制和 boot-stub 擦除式 ROM ISP 复位序列。

固件提供运行统计变量：Device 的 gRfResendCount、gRfResendFailCount、gRfUartDirectCount，以及 Host 的 gRfBackpressureCount、gRfCreditZeroCount，可在调试版本中观察重传、额度为零和安全回退次数。

与官方原工程的逐项修改记录见：[CHANGES_FROM_OFFICIAL.md](CHANGES_FROM_OFFICIAL.md)。

Host USB 当前默认选择原厂商 CH341 兼容模式（VID:PID `1a86:7523`）。Linux 应由内核 `ch341` 驱动创建 `/dev/ttyUSB*`，Windows 使用 CH340/CH341 兼容驱动。此前的 CDC-ACM（`1a86:8040`、`/dev/ttyACM*`）控制端点在本板上会触发 reset/重枚举，暂不作为推荐版本。刷入 Vendor 固件后应检查 `lsusb -t` 是否显示 `Driver=ch341`。
