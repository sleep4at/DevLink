# 单角色固件

当前拆分为两个互斥的固件：

- `RF_Uart`：Device 固件，连接目标 MCU。PA0/PA1 为目标 UART，PA2 为目标 RESET，PA7 为目标 BOOT（高有效），PA3 为双 LED 状态输出。目标 UART 环形缓冲区保持 3 KiB。
- `RF_UartDongle`：Host 固件，连接 USB。PA2 为接地触发的远端下载请求键，PA7 不再作为角色键使用，PA3 为双 LED 状态输出。Host RF 接收环形缓冲区为 1.5 KiB。

两份固件不再保存或切换运行角色，因此不会同时编译/运行 USB 和目标 UART 两套业务路径。Host 的 PA2 请求通过状态响应的 `resv` 控制位传给 Device；Device 收到单字节 `0x7F` 后在主循环执行 BOOT/RESET 脉冲，并在脉冲完成后再向目标 UART 发送 `0x7F`。

PA3 指示规则：未连接时约 1 Hz 闪烁，连接后常亮；Host 使用输出高电平对应的 LED，Device 使用输出低电平对应的 LED，浮空时两灯熄灭。

注意：单角色方案取消了 PA7 长按角色切换功能；切换角色需要重新烧录另一份固件。

数据保护：Device 在状态请求中按 16 字节单位报告 UART 输出队列的可用额度，Host 按额度读取 USB 数据；Device 发出的每个数据包也携带同一额度，因而 Host 在 ACK 中返回 USB 数据时仍受限于 Device 的可用空间。Host 接收 RF 数据时如果整包无法写入缓冲区则不发送 ACK，Device 会按原序号超时重传。额度为 0 时只发送空 ACK/状态响应，不会强行取数。

当前主要缓冲区：Device 目标 UART 3 KiB、Device RF 输出 512 B；Host USB 输入 512 B、Host RF 接收 1536 B。Device 链接 RAM 使用约 11.5 KiB/12 KiB，映射文件显示静态区末端到栈顶约 772 B；Host 约 10.5 KiB/12 KiB，约 1760 B。Device 余量偏紧，因此不建议继续增大 Device 缓冲区，实际使用应通过栈水位/长时间烧录测试确认安全余量。

固件提供运行统计变量：Device 的 gRfResendCount、gRfResendFailCount、gRfUartDirectCount，以及 Host 的 gRfBackpressureCount、gRfCreditZeroCount，可在调试版本中观察重传、额度为零和安全回退次数。

与官方原工程的逐项修改记录见：[CHANGES_FROM_OFFICIAL.md](CHANGES_FROM_OFFICIAL.md)。
