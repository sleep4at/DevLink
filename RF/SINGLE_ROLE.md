# 单角色固件

当前拆分为两个互斥的固件：

- `RF_Uart`：Device 固件，连接目标 MCU。PA0/PA1 为目标 UART；PA2 同时接目标 NRST 和本机自引导按键（按键接地触发）；PA7 为目标 BOOT 控制，空闲时高阻，仅远端下载时驱动；PA3 为双 LED 状态输出。目标 UART 环形缓冲区保持 3 KiB。
- `RF_UartDongle`：Host 固件，连接 USB。PA7 为接地触发的远端 Device 下载请求键，PA2 为接地触发的本机 Bootloader 键，PA3 为双 LED 状态输出。Host RF 接收环形缓冲区为 1 KiB。

两份固件不再保存或切换运行角色，因此不会同时编译/运行 USB 和目标 UART 两套业务路径。Host 的 PA7 请求通过状态响应的 `resv` 控制位传给 Device；需要 STM32 同步字节的路径会在下载控制完成后向目标 UART 发送 `0x7F`。Device 的 BOOT/RESET 时序由主循环中的非阻塞状态机执行：PA7 输出高 50 ms，随后 PA2 拉低 50 ms；释放 PA2 后继续保持 PA7 高 100 ms，最后 PA7 改为无上拉高阻，不主动拉低。时序计数使用启动时校准的 LSI/RTC 计数，不随 UART 配置引发的 24/100 MHz 系统时钟切换而改变。

Device 的本机自引导按键改为 PA2，配置为上拉输入和下降沿中断；该脚也接目标 NRST，因此按键按下期间会复位目标 MCU。为降低探头毛刺或误操作清除应用的风险，Device 使用两步确认：第一次按住 3 秒后 PA3 开始双闪，进入 5 秒确认窗口；松开第一次按键不会擦除。窗口内再按住 1 秒，PA3 快闪后松开，才执行擦除。确认窗口超时、短按或第二次按住不足 1 秒都会取消，设备继续运行。擦除入口在 `__HIGH_CODE` 中擦除 `0x0000` 起始的一个 4 KiB 块，随后执行 FlashROM 复位准备和系统软件复位，以便 ROM ISP 接管。**这是破坏性入口**：当前 Device 镜像从 `0x0000` 启动，擦除会破坏启动代码；Bootloader 必须启用，恢复时须烧录包含 `0x0000` 启动内容的完整 HEX。PA7 空闲时为无上拉高阻，依靠板上 NMOS 栅极下拉保持关闭；远端下载时才临时驱动 PA2/PA7。若本机长按键已按下，远端复位脉冲会延后，避免复位输出与按键接地冲突。

Host 的 PA2 使用相同的按下确认、25 ms 去抖和松开复位流程；复位前先在普通任务中关闭应用 USB，并按参考工程等待约 20 ms，再由 RAM 中的函数擦除 stub 并复位，Host USB 由 ROM Bootloader 重新枚举。PA7 用于请求远端 Device 进入下载模式。

Host 的链接布局为：`0x0000` 放置 4 字节 `boot_stub.S` 跳转，应用从 `0x2000` 开始；Host 自启动 ISP 时只擦除 `0x0000` 起始的 4 KiB。Device 没有独立 boot stub，应用从 `0x0000` 启动，同样通过擦除首个 4 KiB 后复位进入 ROM ISP。两种入口都是破坏性操作，ISP 超时后必须重新烧录完整 HEX 才能恢复应用。

PA3 指示规则：未连接时约 1 Hz 闪烁，连接后角色灯常亮；有 USB/RF 有效数据时约 50 ms 切换到另一只 LED，随后恢复角色灯。Host 使用输出高电平对应的 LED，Device 使用输出低电平对应的 LED，浮空时两灯熄灭。

Device 的 LED、按键长按和 BOOT/RESET 延时使用启动时校准的 LSI/RTC 计数，避免 UART 配置切换系统主频后时长变化；Host 的 LED 计时使用启动时初始化的自由运行 SysTick。

注意：单角色方案取消了 PA7 长按角色切换功能；切换角色需要重新烧录另一份固件。

数据保护：Device 在状态请求和数据包中按 16 字节单位报告 UART 输出队列的可用额度，Host 按额度读取 USB 数据，并在 ACK 中返回 Host RF 接收队列额度；状态响应 `resv` bit 0 保留远端下载命令，bit 7 表示额度有效，bit 1..6 以 32 字节单位报告 Host 额度。旧版 Host 不置有效位时，Device 按旧协议继续工作。Device 根据 Host 额度限制目标 UART 上行包长度，额度为 0 时保留 UART 队列数据并发送状态探测，不会继续盲发。Host 若仍遇到整包无法入队，会不确认该包并在短重试窗口重新监听。缓冲区额度可避免正常反压下溢出，但目标 UART 没有硬件流控，持续输入超过 3 KiB 队列容量仍可能丢数据；无线重试耗尽会断开并重新配对，不承诺无限时无损。

当前主要缓冲区：Device 目标 UART 3 KiB、Device RF 输出 512 B；Host USB 输入 512 B、Host RF 接收 1024 B。当前构建链接器报告 RAM 使用：Device 11220/12288 B（名义剩余 1068 B），Host 10824/12288 B（名义剩余 1464 B）。该数值是链接器 RAM 区域占用差，不是运行时栈高水位测量；工程没有栈水位测试。Host 增加了 USB IRQ 调试输出抑制和 boot-stub 擦除式 ROM ISP 复位序列。

固件提供运行统计变量：Device 的 gRfResendCount、gRfResendFailCount、gRfUartDirectCount，以及 Host 的 gRfBackpressureCount、gRfCreditZeroCount，可在调试版本中观察重传、额度为零和安全回退次数。

与官方原工程的逐项修改记录见：[CHANGES_FROM_OFFICIAL.md](CHANGES_FROM_OFFICIAL.md)。

Host USB 当前默认选择原厂商 CH341 兼容模式（VID:PID `1a86:7523`）。Linux 应由内核 `ch341` 驱动创建 `/dev/ttyUSB*`，Windows 使用 CH340/CH341 兼容驱动。此前的 CDC-ACM（`1a86:8040`、`/dev/ttyACM*`）控制端点在本板上会触发 reset/重枚举，暂不作为推荐版本。刷入 Vendor 固件后应检查 `lsusb -t` 是否显示 `Driver=ch341`。
