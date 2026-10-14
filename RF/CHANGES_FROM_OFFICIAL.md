# 相对官方原工程的修改说明

本文以同目录旁边的官方工程 CH570无线串口-3v1 为基线，说明当前“修改”工程中已经落地的变化。当前工程仍保留官方 RF 配对、2.4 GHz PHY、USB 协议、UART 参数配置和串口数据转发的基本流程；修改主要集中在单角色硬件引脚、远端下载控制、LED 指示、缓冲区保护和异常统计。

## 1. 工程形态变化

| 项目 | 官方原工程 | 当前修改工程 |
|---|---|---|
| 固件角色 | RF_Uart 为 Device，RF_UartDongle 为 Host | 仍为两份互斥的单角色固件 |
| 角色运行时切换 | 原工程没有统一的角色合并方案 | 当前分立工程不运行时切换角色；切换角色需要烧录另一份固件 |
| Device 固件 | 连接目标 MCU UART | 保持连接目标 MCU UART |
| Host 固件 | 连接 USB | 保持连接 USB |
| RF 配对与基本传输 | 官方流程 | 保持官方流程，增加反压和重传保护 |

当前没有把 Host 和 Device 拼成一个运行时双角色固件，也没有实现 PA7 长按后保存角色并软重启。RF_Uart 和 RF_UartDongle 仍然分别编译、分别烧录。

### Linux USB 现场检查与修复

现场排查区分出两类问题：旧 CDC 固件曾在打开端口时触发 USB reset/disconnect；此外，Linux 主机的 USB 自动挂起也会使当前 Host USB 设备进入 runtime suspend，恢复不稳定时表现为串口打开等待约 5 秒并报输入/输出错误。后者通过关闭该设备的自动挂起解决，不是单靠修改固件即可消除的问题。

当前默认构建已切回官方原有 CH341 兼容 Vendor 模式：设备描述符为 `1a86:7523`，数据端点使用 CH341 的 32 字节 bulk OUT/IN，控制请求实现保留 CH341 的 `0x5f/0x9a/0x95/0xa1/0xa4/0xb2` 集合。USB 事件队列现扩为 32 项，并为 EP0 SETUP/控制 OUT 保存事件快照；EP0 SETUP 现在在 USB 中断中立即处理，避免 Linux 控制传输等待前台循环导致约 5 秒超时；主循环继续排空数据事件。对 Linux 可能查询的未知字符串索引（如 `0xEE`）保持标准 STALL，并在下一个 SETUP 到来时清除 STALL、复位 EP0 数据 toggle；没有加入 Microsoft OS 专用描述符。Vendor 模式是当前推荐配置，实际首次枚举和打开端口的稳定性应以刷入当前 HEX 后的 `dmesg` 日志为准。

#### Linux 禁用该设备的 USB 自动挂起

如果设备可以枚举，但打开 `/dev/ttyUSB*` 时等待数秒、报输入/输出错误，且 `power/runtime_status` 显示 `suspended`，应检查 Linux USB runtime power management。确认 VID:PID 为 `1a86:7523` 后，可添加 udev 规则，让该 Host 设备保持唤醒：

```udev
# /etc/udev/rules.d/99-ch570-dongle-no-autosuspend.rules
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="1a86", ATTR{idProduct}=="7523", TEST=="power/control", ATTR{power/control}="on"
```

保存后重新加载规则并重新插拔设备：

```sh
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb
```

检查设备对应的 sysfs 路径（例如 `/sys/bus/usb/devices/3-2`）：`power/control` 应为 `on`，`power/runtime_status` 通常应为 `active`。设备号和路径会随 USB 拓扑、插拔而变化，不要把示例路径写死。

如果系统安装并启用了 TLP，还应将该设备加入 TLP 的 USB 自动挂起黑名单/拒绝列表，避免 TLP 再次将其设为自动挂起。保留已有配置，并在 `USB_DENYLIST` 中追加 `1a86:7523`，然后重启 TLP；可用 `tlp-stat -u` 检查结果。TLP 不同版本的配置项名称可能不同，应以本机 `/etc/tlp.conf` 注释和 `tlp-stat` 输出为准。

这是 Linux 主机侧电源管理配置，不会改动 Windows 兼容代码或 USB 描述符。固件中的 Linux Vendor/CH341 控制请求处理与此为不同层面的修复；避免设备被自动挂起后无法可靠恢复，才能消除本机复现的首次打开延迟和 I/O 错误。

## 变化摘要

### 修改的功能

- Device 的 BOOT 引脚由 PA3 调整为 PA7，LED 由 PA7 调整为 PA3；Host 的 LED 由 PA7 调整为 PA3。
- Device/Host 的 LED 驱动改为适配双 LED 接法，并增加连接状态指示。
- Host 增加 PA2 接地触发的远端下载 GPIO 中断输入，以及 PA7 接地触发的本机 Bootloader GPIO 中断；分立固件不再使用 PA7 角色切换。
- Device 增加 PA7 接地触发的本机 Bootloader 按键：下降沿中断只记录请求，主循环去抖后按 CH570 ROM 规定的软件复位条件进入本机 ROM Bootloader；目标 MCU 的远端 BOOT/RESET 脉冲路径保持独立。
- Host 增加 PA7 接地触发的本机 Bootloader 按键；PA2 仍只负责向远端 Device 请求下载。进入 ROM ISP 前先在普通任务中关闭应用 USB、复位 USB SIE/FIFO、停止 USB IRQ/DMA 并移除 D+ 上拉，再由 RAM 中的函数擦除 `0x0000` 起始的 4 KiB boot stub 并执行 WCH 软件复位序列。
- Host RF 缓冲区由官方 512 B 调整为 1024 B（此前曾为 1536 B）；Device 3 KiB UART 缓冲区和 512 B RF 缓冲区保持不变。
- USB 取数接口改为遵守调用方给出的最大长度。
- Host Vendor USB 不再在 USB 中断路径逐包打印整包十六进制数据，避免双向 30 ms 测试时阻塞 CH341 控制请求并触发 Linux `-110` 超时。
- Host USB 默认使用原厂商 CH341 兼容 Vendor 接口（`1a86:7523`），由 Linux 原生 `ch341` 驱动承载；CDC 实现保留为可选实验模式，但不作为当前推荐固件。
- Device 和 Host LED 改用启动时初始化的自由运行 SysTick 计数器，不再读取未初始化的计数器。
- 连接后的 LED 仍显示角色状态；USB 或 RF 有效数据会触发约 50 ms 的另一只 LED 活动脉冲，随后自动恢复角色灯，不在 USB/RF 中断中直接改 GPIO。

### 新增的功能

- Host PA2 按键触发远端 Device 的 BOOT/RESET 下载时序。
- Linux 可直接使用 `/dev/ttyUSB*` 的 CH341 兼容接口，不依赖额外自定义上位机协议。
- 使用 RF 包头原有 resv 字段传递 16 B 单位的缓冲区额度。
- Host 缓冲区不足时拒绝确认，依靠 Device 原序号重传恢复数据。
- 增加重传、缓冲区拒收、额度为零和安全回退计数器。

### 改进的功能

- 避免 Host 缓冲区溢出后仍发送 ACK 所造成的静默丢包。
- 避免 ACK 中的 USB 数据再次超过 Device 可用缓冲区。
- 将毫秒级 BOOT/RESET 脉冲从 RF 中断回调移到主循环，降低中断阻塞风险。
- 修正未连接 LED 不闪烁：初始化自由运行计数器，并按当前系统主频换算半秒间隔。
- 修正连接后活动指示被角色灯任务覆盖的问题：活动状态由 RF/USB 数据路径置位，主循环统一输出活动窗口。
- Host 对“Device→Host”数据只在 RF 包接收时置位一次活动标志，避免 RF 接收和 USB 读出各翻转一次而相互抵消。
- 保持空口包格式和单包长度不变，正常传输不增加 RF 字节开销。

## 2. 硬件引脚和 LED 变化

### Device（RF_Uart）

| 功能 | 官方原工程 | 当前修改工程 |
|---|---|---|
| 目标 UART | PA0/PA1 | 不变 |
| RESET | PA2 | 不变 |
| BOOT | PA3 | 改为 PA7 |
| LED | PA7 | 改为 PA3 |

Device 的 PA3 按用户双 LED 接法使用：输出低电平点亮低有效 LED；浮空时两只 LED 都熄灭。

### Host（RF_UartDongle）

| 功能 | 官方原工程 | 当前修改工程 |
|---|---|---|
| LED | PA7 | 改为 PA3 |
| 远端下载按键 | 无 | PA2，上拉输入，接地下降沿触发 |
| PA7 | 原本用于 LED | 本机 Bootloader 按键，上拉输入、下降沿中断 |

Host 的 PA3 使用输出高电平点亮另一只 LED；浮空时两只 LED 都熄灭。Host 和 Device 都采用“未连接约 1 Hz 闪烁、连接后角色灯常亮、传输时短暂切换另一只 LED”的状态指示。

## 3. 远端 BOOT/RESET 下载功能

官方 Device 代码已经支持接收目标 MCU 的下载同步字节 0x7F，但当前工程做了以下调整：

1. Host 的 PA2 按键由 GPIOA 中断捕获，按下后置位请求标志。
2. Host 在下一个 RF 状态响应的 resv 控制位中发送 RF_CTRL_ENTER_BOOT。
3. Device 收到控制位后，在主循环而不是 RF 中断回调中执行 BOOT/RESET 时序。
4. Device 将 BOOT、RESET 临时配置为推挽输出，执行：BOOT/RESET 初始高电平、RESET 拉低、RESET 拉高、BOOT 拉低，然后恢复到高阻/上拉空闲状态。
5. 脉冲完成后，Device 向目标 MCU UART 发送 0x7F，保持 ISP 同步顺序。

把脉冲从 RF 回调移到主循环，避免在无线中断上下文中执行毫秒级延时。当前实现假定板上 BOOT 选择电路使“高电平有效”，目标 MCU 的具体有效电平由硬件开关和外围电路保证。

### Device 本机 Bootloader 按键

Device PA7 空闲时为上拉输入和下降沿中断。中断只记录按下时刻；主循环去抖并要求持续按住 3 秒，PA3 快速闪烁提示确认后，松开才执行危险操作，短按会取消。入口检查 `R8_GLOB_CFG_INFO & RB_CFG_BOOT_EN`，随后在 `__HIGH_CODE` 中反复尝试擦除 `0x0000` 起始的一个 4 KiB 块，执行 `FLASH_ROM_SW_RESET()` 和 WCH 安全访问/软件复位序列，让 ROM ISP 接管。当前工程 `_start` 位于 `0x0000`，故擦除会破坏启动代码；ROM Bootloader 必须启用，恢复时须烧录包含 `0x0000` 启动内容的完整 HEX。进入前 PA2/PA7 被恢复为输入状态，不会产生远端 MCU 下载脉冲。

Host PA7 与 Device 一样采用“擦除 boot stub 后复位”的 ISP 流程。Host 仍保留 `0x0000` 的 4 字节 boot stub 和 `0x2000` 应用布局；PA7 长按确认后关闭应用 USB，随后在 `__HIGH_CODE` 中反复擦除 `0x0000` 起始的一个 4 KiB 块，执行 `FLASH_ROM_SW_RESET()` 和 WCH 安全访问/软件复位序列，让 ROM ISP 接管。应用主体位于 `0x2000`，不会被这次擦除影响；恢复运行时必须烧录包含 `0x0000` boot stub 的完整 HEX。Host 的 USB 会断开，随后由 CH570 ROM Bootloader 重新枚举为 `1a86:55e0`。PA2 的远端下载按键逻辑保持不变。

ROM ISP 的等待窗口由芯片内部 Bootloader 决定，当前工程没有可配置的 10 秒常量；未收到烧录通信时通常约 10 秒后自动退出并断开 USB。

## 4. RF 缓冲区和数据保护变化

### 缓冲区配置

| 缓冲区 | 官方原工程 | 当前修改工程 | 说明 |
|---|---:|---:|---|
| Device 目标 UART 环形缓冲区 | 3072 B | 3072 B | 保持原值 |
| Device RF 接收/输出环形缓冲区 | 512 B | 512 B | 恢复并保持官方原值 |
| Host USB 输入环形缓冲区 | 512 B | 512 B | 保持原值 |
| Host RF 接收环形缓冲区 | 512 B | 1024 B | 在保留 1 KiB 突发容忍度的同时，为 Host 栈和中断保留余量 |
| RF DMA 接收区 | 264 B | 264 B | 硬件例程要求，未修改 |

Host RF 缓冲区增大只提高突发缓存能力，不改变 RF 单包最大载荷，也不直接提高无线物理速率；从此前 1536 B 调整为 1024 B 会降低可缓存的突发长度，但可释放 512 B RAM。Device RAM 余量较紧，因此没有继续增大 Device 缓冲区。

### 新增额度（credit）机制

官方包头已有 1 字节 resv 保留字段。当前工程利用该字段按 16 字节为单位传递 Device 缓冲区剩余空间：

- Device 状态请求中报告自身 pRfBuf 的可用空间；
- Device 发出的数据包也携带该额度；
- Host 根据额度限制从 USB 取出的数据量，并限制 ACK 中返回给 Device 的数据量；
- 额度为 0 时发送空响应，不强行取数。

该机制没有增加 RF 包头或额外空口字节，正常情况下不会增加单包发送时间。额度不足时可能出现空 ACK、等待或重传，但可以避免缓冲区溢出造成的数据丢失。

### Host 满缓冲处理

官方 Host 在接收 RF 数据后没有检查 write_buf() 是否完整写入，仍会发送 ACK，缓冲区满时可能直接丢包。当前工程改为：

- 必须整包写入 Host RF 缓冲区才发送 ACK；
- 如果空间不足，不递增接收序号、不发送成功 ACK；
- 重新进入接收，等待 Device 超时后按原序号重传。

因此，当前机制解决的是“缓冲区溢出后仍确认导致的静默丢包”。连续重传超过 RESEND_COUNT 仍失败时，官方的最终放弃策略仍然保留。

### USB 读取长度

官方 USB_RxQuery() 会在部分路径固定按最多 251 字节读取。当前修改为尊重调用方传入的最大长度，使 RF credit 能真正限制 USB 到 RF 的单次取数长度。

## 5. 运行统计和调试辅助

当前新增以下 RAM 统计变量，不改变空口协议：

- Device：gRfResendCount、gRfResendFailCount、gRfUartDirectCount；
- Host：gRfBackpressureCount、gRfCreditZeroCount。

它们分别用于观察重传次数、重传最终失败次数、Device 安全直发回退次数、Host 因缓冲区不足拒收次数和额度为零次数。

## 6. 保持不变的功能

- RF 配对、绑定信息保存和自动回连机制；
- RF 序号、ACK、超时和重传框架；
- 2M PHY 和原有 RF 访问地址/频点配置；
- USB 厂商模式及串口参数配置（CDC 代码仍保留，但当前默认关闭）；
- Device 目标 UART 波特率、停止位、校验位和数据位设置；
- 原有 UART 环形缓冲区和 UART 中断接收流程；
- DTR_RTS_FUNC 的编译配置框架。当前单角色下载功能使用 RESET/BOOT 路径，不等同于同时启用独立 DTR/RTS 两路调制解调信号。

## 7. 当前编译和验证状态

两个单角色工程已通过 clean build：

- Device：长按擦除启动块的入口版本 RAM 为 11672 / 12288 B，静态区末端到栈顶约 616 B；PA7 要按住 3 秒并等待 PA3 快速闪烁，松开后才擦除地址 0 起始的 4 KiB 并复位。当前固件从地址 0 启动，需使用完整 HEX 恢复。`RF_UartTxInit()` 已移出 `.highcode`，仅在启动时从 Flash 执行，以回收 RAM 执行区。
- Host：当前 boot-stub 擦除入口版本 RAM 为 11176 / 12288 B，静态区末端到栈顶约 1112 B；PA7 长按确认后擦除 `0x0000` 起始的 4 KiB boot stub，再执行软件复位进入 ROM ISP。Host 应用链接在 `0x2000`，恢复运行必须烧录包含 `0x0000` boot stub 的完整 HEX。

代码层面已检查包长度、额度边界和 ACK/重传路径；尚未替代真实硬件测试。正式用于 200 KB 烧录前，仍应验证 BOOT/RESET 波形、按键触发、RF 干扰下的重传，以及 gRfResendFailCount == 0 和最终固件校验结果。
