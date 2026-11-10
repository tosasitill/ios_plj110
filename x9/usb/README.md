# X9 USB2 device 开发

**当前是已实现的 USB2 CDC-ECM + 自有 bulk 传输候选，不是已验收的完整 iPhone USB 栈。**
尚无本版本实机枚举、DMA、网络或配对结果。USB3、Type-C 双角色、原生
`IOUSBDeviceFamily → AppleUSBDeviceMux → lockdownd` 尚未实现；Finder/iTunes
配对、照片同步及 USB 音频不能据此宣称可用。

## 开启与回退

| Boot arg | 行为 |
|---|---|
| 未设置 `x9_usb` / `x9_usb=0` | 原有 EP0 日志模式，保持默认行为 |
| `x9_usb=1` | USB2 CDC-ECM，原生 BSD `ue0`，保留设备级 EP0 日志 |
| `x9_usb=2` | ECM + 自有 bulk interface 2，iOS 管理员独占访问 |
| `x9_usb_ip=0` | 不自动给 `ue0` 配 IPv4；默认是 `192.168.7.2/24` |

新 worker 与日志 worker **二选一**，不同时操作 MTU3。保留已验证的 LK
USB2 PHY/时钟初始化，不猜测完整冷启动上电序列；不会切 host 角色或输出 VBUS。
设备仍由用户手动临时 `fastboot boot`，不写 boot/GPT/固件分区。

iOS 用户工具由常规 kernel 构建生成到输出目录的 `usb/usbctl` 与
`usb/libx9usb.a`，**不自动安装**。首次往根盘 staging 必须另确认并按项目 extent
流程执行，不应直接挂载/覆盖 pinned 根盘。

## 验证 USB 枚举和真实网络

1. 启动含 `x9_usb=1` 或 `2` 的候选，接 Mac/Linux。
2. Mac 查看枚举：

   ```sh
   ioreg -p IOUSB -l -w0 | grep -E 'idVendor|idProduct|Device Speed'
   ```

   预期 `1209:0001`，USB2 HS。不能用参考文章的 **Device Speed 4 / USB3** 作本版标准。
   `host.py` 的 descriptor-only 操作不 reset、不 set configuration、不 detach ECM：

   ```sh
   python3 x9/usb/host.py
   ```

   依赖 `pyusb` + `libusb`。ECM interface 0/1；mode2 另有 vendor interface 2，
   bulk OUT `0x03` / IN `0x83`。正式产品需独立 VID/PID，目前沿用开发测试 ID。
3. 系统 ECM 驱动创建 host 网络接口，MAC 为 `02:58:39:55:53:01`。
   将该接口设为 `192.168.7.1/24`；不要修改 Wi-Fi 的接口或默认路由。
   例如 Mac 确认它是 `enN` 后：

   ```sh
   sudo ifconfig enN 192.168.7.1 netmask 255.255.255.0 up
   ping -c 3 192.168.7.2
   ```

   `ue0` 使用 `02:58:39:55:53:02`，MTU1500。没有 DHCP、Internet Sharing 或默认网关。
   需在日志核对真实 QMU completion、`net_rx_frames/net_tx_frames` 与 ping 回复。
   只有枚举/config/alt/filter 成功不等于 DMA 或网络已通。
4. 日志工具继续独立读 EP0，不 claim ECM 或 raw interface：

   ```sh
   python3 x9/tools/usb-live/usb_log.py --output ~/usb-x9.txt
   ```

## 验证 iOS bulk 数据链

先在 iOS 通过已确认的交付路径提供 `usbctl`。只读状态无需管理员；echo 需 root、
`x9_usb=2`、独占 lease。两端操作如下：

```sh
# iOS；60秒有界运行，Ctrl-C 关闭客户端
usbctl --status
usbctl --echo 60
```

```sh
# Mac/Linux；只写一次，不在 ambiguous timeout 后自动重放
python3 x9/usb/host.py --echo 'X9 USB host to iOS to host'
```

host 返回同样的完整字节才是端到端成功。iOS `send` 成功只表示入队，不能当作
USB wire ACK；状态的 DMA completions/byte counters 取真实 HWO 与 QCPR。
raw 接口是**字节块流，不是消息/文件/RPC/Apple mux 协议**；RX512字节块、
TX最多4096字节，可短包/ZLP。上层需要自定 framing 和确认协议。

## iOS API

`x9usb.h` + `libx9usb.a` 链接 CoreFoundation/IOKit，无需修改系统 daemon。

| 用户函数 | 语义 |
|---|---|
| `x9usb_open(false, &connection)` | type0，只读状态 |
| `x9usb_open(true, &connection)` | type1，管理员独占 bulk lease |
| `x9usb_status(connection, &status)` | ABI1、状态/errno、真实 speed、generation、计数 |
| `x9usb_send(connection, generation, bytes, n)` | 非阻塞入队，1..4096字节 |
| `x9usb_receive(connection, generation, bytes, &n)` | 非阻塞取块，不够容量不消费 |
| `IOServiceClose(connection)` | 撤销 lease；worker 停止/重置 raw DMA，防跨客户端 RX |

selector0/1/2分别对应状态/发送/接收，结构为固定128字节 `x9_usb_status`。
只接受同步 inband 数据，不支持 async port、用户 DMA 指针或 memory descriptor。
`kIOReturnNotReady` 是队列空/满，可在**同一 generation**等待；
`kIOReturnAborted` 是 reset/configuration 后旧 generation，不能重放旧写；
`kIOReturnOffline` 为未配置或硬件隔离，`NoSpace` 为输出缓冲不足。
客户端关闭可能已发送部分旧 TX，不能撤销；不会将旧 RX 交给新 lease。

独立交叉编译，不运行设备或测试套件：

```sh
python3 -I x9/usb/build.py --out .local/x9/usb/client
```

## 参考和硬件差异

- 用户指定 [USB porting](https://unixnu.com/porting/usb) 与固定
  [QcomDwc3 e4efe95](https://github.com/UniXNU/QcomDwc3/tree/e4efe95c903a737e0f5955e5b50a8fb4d2a7d718)：
  其复用 AppleUSBXDCI 依赖 DWC3 寄存器和选定 kernelcache ABI，**不能套在 MTU3**。
- 本地 UniXNU/hackios 无 MT6993 原生 Apple USB device 后端；旧 sweet USB ECM
  网络模式提供 XNU ifnet 用法，不移植 SM7150 DWC3 TRB/IRQ/电源参数。
- OPPO `oppo/mt6993_b_16.0.0_find_x9`，`kernel_device_modules-6.12`：
  `mt6993.dts`、`drivers/usb/mtu3/{mtu3_core.c,mtu3_gadget.c,mtu3_gadget_ep0.c,mtu3_qmu.c,mtu3_hw_regs.h}`。
- 板 DTS：MAC `0x16751000/0x2e00`，IPPC `0x16753e00/0x100`，
  device GIC SPI501 = INTID533，host SPI502 =534；本版 worker1ms poll，不注册IRQ。
  USB2 XS-PHY `0x16760000`，MT6379 eUSB2 repeater，USB3 PHY `0x1676b000`；
  沿用原 USB2 初始化，未接管 USB3 PHY/selector/Type-C charger。
- `MISC_CTRL[31]` 是 **36-bit DMA capability**，按 OPPO 只读取；没有把它当 enable
  强置。缺 capability 时明确拒绝，因为 XNU 当前没有 <4GiB 可用页。
- QMU GPD16字节，gen2由 IPPC trunk version ≥`0x1003`选择，包含36-bit high nibble。
  每方向固定16KiB物理连续/MapperNone prepared allocation，两个256字节分隔的
  descriptor + 独立4096字节 data span；提交前 clean，完成后 invalidate。
  STOP须确认 ACTIVE清零，才能重用；错误/停止超时断开并永久保留 prepared pages。
  DTS无 ssusb `iommus` 不代表 live EMI/MPU已允许这些页：仍需一次真实 DMA 验收。

## 未完成的原生 iPhone USB 路线

本实现不发布假的 `IOUSBDeviceController`，`native_apple_stack` 始终0；没有把
ECM或自有 UserClient 当成 lockdownd 配对。完整目标还需：

1. 先验收本板一次真实 QMU OUT/IN 和断连/重接，再接 IRQ、更多在飞 GPD及吞吐优化。
2. 恢复/分析 **20H392** IOUSBDeviceFamily、AppleUSBDeviceMux 的 controller/endpoint/
   completion ABI，建立真实 MTU3 controller adapter，并纳入本项目 kernel 构建；
   不能使用文章的24A446 ABI slot或伪造 DWC3 MMIO。
3. 接原始 configd USBDeviceConfiguration 和 lockdownd，验证 Trust UI、配对记录及
   lockdown session；之后才按 D22 identity核验chip/board/security-domain、真实fsid和锁状态。
4. 再实现 MT6993 XS-PHY USB3、MT6379/charger Type-C role、orientation及原生 xHCI。
   不直接移植 QMP、UCSI/GLINK或 Qualcomm SMMU 参数。
