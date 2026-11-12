# X9 USB2 device 开发

## AppSync / 原生 IPA 安装入口（2026-11-12，未实机验收）

`../appsync/` 现在提供独立的 iOS16 arm64 安装/FrontBoard hooks、递归 ldid
签名器及离线 runtime；不改 Apple executables。构建成功不代表运行库已经
staging，或 x9 原生 Apple USB backend 已经完成。

`install.py` 使用真正的系统 usbmuxd/lockdownd：现有 Pair record → verified
session → StartService(AFC/installation_proxy) → 唯一新目录上传 → 单次 Install
→ 指定 bundle/version 的权威 Lookup。服务要求 TLS 时验证 RootCA 并 pin
DeviceCertificate。不使用 private bulk/self-made mux，不替换 daemon。
协议布局参照 libimobiledevice `src/afc.c` 和 `src/installation_proxy.c`。

**以下是原生 backend/Trust/TLS 已验收后的操作者步骤，不要现在选别的设备
代替 X9 测试，也不要重复启动旧失败的 USB 候选：**

```sh
python3 -I x9/usb/pairing.py --list
# 仅当列表中确有 X9，使用那次明确的 device-id；Trust 由用户在原 UI 接受
python3 -I x9/usb/pairing.py --pair --device-id ID --record .local/x9/pair-NEW.plist
python3 -I x9/usb/pairing.py --session --device-id ID
# 必须先部署 AppSync runtime 并确认三个目标进程 hook 活跃
python3 -I x9/usb/install.py --install /path/to/Example.ipa --device-id ID \
  --receipt .local/x9/install-NEW.plist
```

IPA 输入不被消费；拒绝 ZIP path traversal/重复 entry、symlink、加密 archive、
非 thin-arm64 或 FairPlay-encrypted main executable。未知 nested code 最终由
设备签名器/原校验器决定；不是任意 IPA 都兼容。已安装同 bundle 的应用必须
显式 `--replace-existing`，默认不覆盖。任何上传/安装歧义均保留 0600 receipt
并停止，不删 staging、不重放、不打印设备身份/证书。

```sh
# 只核对之前的那一个 bundle/version，禁止把超时自动变成第二次 Install
python3 -I x9/usb/install.py --verify --device-id ID --receipt .local/x9/install-NEW.plist
```

receipt绑定原生设备 identity 的SHA256。Lookup只证明该bundle/version当前
在原生用户安装路径上，不证明未知的上次Install成功、实际启动或跨重启持久性。
Lookup缺少LookupResult不当成“未安装”；receipt在首次上传前用O_EXCL原子占有，
同receipt的两个并发调用不能互相覆盖in-flight状态后重复Install。
**这不是新增的可Finder配对boot；registry仍为空，NATIVE10的bulk backend未安装。**

**当前是已实现的 USB2 CDC-ECM + 自有 bulk 传输候选，不是已验收的完整 iPhone USB 栈。**
尚无本版本实机枚举、DMA、网络或配对结果。USB3、Type-C 双角色、原生
`IOUSBDeviceFamily → AppleUSBDeviceMux → lockdownd` 尚未实现；Finder/iTunes
配对、照片同步及 USB 音频不能据此宣称可用。

**USB-3 接口变化**：`--bus-cycle` 无参数，按原始20H392固定50ms；可变延时
改为显式 X9-only 的 `--bus-cycle-delay <0..1000 ms>`。旧 `--bus-cycle 100`
语法不再接受。新的描述符 metadata 支持只影响当前 ECM/private bulk profile，
不会使 Apple mux 自动绑定。

**USB-4 新增**：`--echo-tracked` 按请求等待真实 QMU TX completion；旧 `--echo`
仍只统计入队。lease 切换先 STOP/drain，新的客户端在屏障完成前得到 NotReady。
新增接口向后兼容，不改变旧128B status；旧内核返回 Unsupported，工具不会降级重放。

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

需要区分入队与 DMA 完成时，在 iOS 用 `usbctl --echo-tracked 60` 替代 `--echo`，
host 命令不变。`echo QMU-completed ...` 必须来自 HWO 清零、CPR 到达 terminator、
STOP 确认 ACTIVE 清零和长度一致四个条件；**仍须 host 完整读回才算 wire 链验收**。
超时、取消或故障均终止，不重新发送；STOP 成功的取消也可能已传出部分字节。

## iOS API

`x9usb.h` + `libx9usb.a` 链接 CoreFoundation/IOKit，无需修改系统 daemon。

| 用户函数 | 语义 |
|---|---|
| `x9usb_open(false, &connection)` | type0，只读状态 |
| `x9usb_open(true, &connection)` | type1，管理员独占 bulk lease |
| `x9usb_status(connection, &status)` | ABI1、状态/errno、真实 speed、generation、计数 |
| `x9usb_send(connection, generation, bytes, n)` | 非阻塞入队，1..4096字节 |
| `x9usb_receive(connection, generation, bytes, &n)` | 非阻塞取块，不够容量不消费 |
| `x9usb_send_tracked(connection, generation, bytes, n, &id)` | 单次入队并返回请求ID，不等于完成 |
| `x9usb_tx_result(connection, original_generation, id, consume, &result)` | 只读轮询；consume只移除终态记录 |
| `x9usb_get_description(connection, &snapshot)` | selector3，只读240B，旧内核返回Unsupported |
| `x9usb_set_description(&description)` | 原始IOKit SetDescription，显式bus-off后受控更新224B metadata |
| `IOServiceClose(connection)` | 撤销 lease；worker 停止/重置 raw DMA，防跨客户端 RX |

selector0/1/2分别对应状态/发送/接收，结构为固定128字节 `x9_usb_status`。
只接受同步 inband 数据，不支持 async port、用户 DMA 指针或 memory descriptor。
`kIOReturnNotReady` 是队列空/满，可在**同一 generation**等待；
`kIOReturnAborted` 是 reset/configuration 后旧 generation，不能重放旧写；
`kIOReturnOffline` 为未配置或硬件隔离，`NoSpace` 为输出缓冲不足。
客户端关闭可能已发送部分旧 TX，不能撤销；不会将旧 RX 交给新 lease。

### 获取单次 TX 的权威结果

selector4 是 tracked send（一个generation标量、1..4096B inband、一个ID标量输出）；
selector5 是结果查询（三个标量：original generation、ID、consume 0/1，64B结构输出）。
结果ABI1保存requested/actual、state和errno，没有用户DMA指针或异步回调地址。
最多16条记录，终态须消费才释放槽；满时 NotReady 是**未入队**，不能覆盖旧结果。

| state | 含义 |
|---|---|
| QUEUED | 尚未发布 HWO |
| ACTIVE | 已交给 QMU，不能把查询超时当成未发送 |
| COMPLETE | HWO/CPR/STOP及完整长度通过，actual才是已确认字节数 |
| CANCELLED | reset/halt后STOP成功，或发布前撤销；可能已部分发送，不可重放 |
| UNCERTAIN | 故障隔离，DMA/传输结论不足，保留prepared页，不可重放 |

reset后用**原generation**取取消结果，不用新generation冒认旧请求；结果读取不要求
bus仍配置。lease关闭清理其结果，新客户端看不到旧记录；ID不回绕。
`actual=0` 在非COMPLETE状态只表示没有确认字节计数，**不证明零字节上wire**。
NotFound表示ID未知/已消费，不等于未传输。`X9_USB_RAW_DRAINING` 仅报告worker屏障，
不是host枚举状态。old chunk的owner/generation从入队固定，HWO与QSTART前再次在锁内
验证；close之后可留下旧在飞TX，但不会给它重新标记成新lease。

### 原生 IOKit 控制入口（不等于原生 mux）

`native_iokit.c` 使用 **20H392 原始 IOKit** 的
`IOUSBDeviceControllerCreateWithService`、`ForceOffBus`、`GoOffAndOnBus`。
显式选择自有 `X9USBDevice`，不伪造默认 `IOUSBDeviceController` 类：

```text
original IOKit -> IORegistryEntrySetCFProperties
               -> X9USBDevice::setProperties -> sole MTU3 worker
```

已实现的命令为 `ForceOffBusEnable`、`ForceOffBusDisable`、`GoOffAndOnBus`；
字典键为 `USBDeviceCommand` / `USBDeviceCommandParameter`。
内核和用户端均要求 root。worker 断连后 STOP 全部 QMU、清队列、推进 generation，
再按指定时间重连；STOP/寄存器读回失败隔离。调用同步等待真实 worker barrier，
**不是等待 host 枚举或配对**。2秒等待超时视为歧义，保留正在执行的命令所有权，
不能自动重放。`SetDevicePreferredConfiguration` 仍返回 `Unsupported`。
`SetDeviceDescription` 仅支持下一节的真实 ECM/private bulk profile，不接受
未经绑定的 mux/PTP/USB3 配置。

工具交付后，可由操作者显式调用；以下操作会断开 ECM/bulk，**不自动执行**：

```sh
usbctl --native-status       # 只读：三个原生类的数量，不等待30秒/不打开接口
usbctl --bus-cycle           # root；原始20H392语义：固定50ms，仅发一次
usbctl --bus-cycle-delay 100 # root；X9-only延时命令，不冒充原生IOKit参数
usbctl --bus-off             # root；保持断连
usbctl --bus-on              # root；恢复软连接
```

ABI1的 `reserved` 字现定义 `X9_USB_BUS_FORCED_OFF` / `X9_USB_BUS_CYCLING`。
原生类出现不等于 mux 流量、Trust 或 lockdown session；`native_apple_stack` 仍为0。
本轮只完成编译及静态调用链确认，尚未上机验证这些命令。

### 将原生描述符字段送到实际 EP0

SDK 通过原始 `IOUSBDeviceDescriptionCreate`、导出的
`_IOUSBDeviceDescriptionGetInfo` 和 `IOUSBDeviceControllerSetDescription`
构造/发送真实原生对象，不手工制造 CFRuntime 布局。仍显式选择 `X9USBDevice`，
没有注册假的原生 controller 或改 configd/lockdownd。

`SetDeviceDescription` 的 `USBDeviceCommandParameter` 是20H392字典。支持的
metadata 为 `vendorID`、`productID`、`deviceID`（USB bcdDevice）、固定
`deviceClass/deviceSubClass/deviceProtocol=ef/02/01`、`BcdUSBVersion=0200`，
以及1..63字节可打印ASCII的 `manufacturerString/productString/serialNumber`。
只接受一个默认配置，`Attributes=80`、`MaxPower=250`、
`Description="X9 ECM transport"`、`AllowMultipleCreates=true`；其
`Interfaces=["X9USBECM"]`（mode1）或再加`"X9USBBulk"`（mode2）是**自有函数
绑定名**，不冒充Apple接口类。ECM函数对应wire interface0/1，bulk对应interface2。
端点、alt、ECM MAC与电源策略不变；其他配置/接口直接Unsupported，不能假存后返回成功。

内核要求 **root + `com.apple.private.usbdevice.setdescription=true`**（与原始
controller权限门一致），工具仅带该entitlement，不改内核AMFI/daemon权限。
必须先完成显式bus-off、QMU STOP与generation barrier；在线、cycling或另一命令
在飞时拒绝，输入校验失败不改任何状态。唯一worker再次检查SOFTCONN清零才原子
提交metadata，推进独立revision与传输generation；不自动重连/重放。forced-off时
不处理旧EP0事件，重连前清掉已锁存的旧setup/reset，避免重新启用旧DMA。
字段实际用于 `GET_DESCRIPTOR` 的device descriptor及UTF16LE string descriptor，
不是仅写进IORegistry。默认VID/PID和字符串不变；修改只在RAM，本次boot结束即丢失。

工具已通过交付路径提供后，可由操作者执行一次下面的窄流程（本轮未执行）：

```sh
# iOS；不要自动重试失败命令；任何失败先保存结果，不继续bus-on
usbctl --description
usbctl --bus-off
usbctl --set-product 'X9 USB Metadata'
usbctl --description
usbctl --bus-on
# Mac/Linux；只读实际wire产品字符串，不读取serial、不reset/detach
python3 x9/usb/host.py --product
```

最后一行应从EP0读到`X9 USB Metadata`，配置、端点与旧ECM/bulk仍保持。内核snapshot
revision增加不是wire成功；只有host读回才验收此metadata路径，也不代表DMA/mux/配对。
`--description`不输出serial、不MMIO；SDK完整snapshot含操作者设置的synthetic serial。
本轮没有安装工具、更新描述符、上机或原生配对证据。

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
2. 已恢复 **20H392** 原始 kernelcache 和三个 USB 依赖输入，但不能直接 prelink：
   原始 `IOService` 为 **168 slots**，当前冻结的 SWEET `IOService.cpo` 为
   **272 slots**。虽然405个内核导入地址均能找到冻结对象中的同址符号别名，
   **导入名字齐全并不能修复虚调用 ABI**。原始 `IOUSBDeviceController` 为
   **0x1f8 bytes / 232 slots**（含尚需恢复签名的 pure virtual 方法），不是24A446的
   AppleUSBXDCI。需要隔离的、经过调用点确认的 ABI 适配或自有 DeviceFamily 实现；
   不全局盲改 XNU vtable，不把 MTU3 模拟成 DWC3。
3. 接原始 configd USBDeviceConfiguration 和 lockdownd，验证 Trust UI、配对记录及
   lockdown session；之后才按 D22 identity核验chip/board/security-domain、真实fsid和锁状态。
4. 再实现 MT6993 XS-PHY USB3、MT6379/charger Type-C role、orientation及原生 xHCI。
   不直接移植 QMP、UCSI/GLINK或 Qualcomm SMMU 参数。

### 离线准备原生栈输入

`prepare_native.py` 只解析原始 Apple 输入和冻结 `MH_OBJECT`，不读取已成功的
boot 候选做二次校验，也不装载或修补 kext。输出只能在新的 `.local/` 或
`x9/out/` 子目录，拒绝覆盖旧证据。固定输入 SHA-256 为
`2ec297bac95bcba01046fd0148e97e7e5885c05a6c18827f39f534ba375c1221`。

```sh
python3 -I x9/usb/prepare_native.py \
  --kernelcache .local/x9/usb/native-20H392/kernelcache.macho \
  --build-manifest .local/x9/usb/native-20H392/BuildManifest.plist \
  --io-service-object .local/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/iokit/DEVELOPMENT/IOService.cpo \
  --objects-file .local/x9/usb/native-20H392/frozen-object-list.txt \
  --out .local/x9/usb/native-prepared-new --extract --require-compatible
```

`--objects-file` 可省略；每行是一个冻结对象路径，不是构建后的镜像。
输出 `abi.json`、`bindings.json`、三个模块的 metadata/segments 和可选原始段。
这些仍是 **prelinked bytes，不是可装载的独立 kext**。当前
`--require-compatible` 返回 **2** 是明确的 ABI/load-readiness 门拒绝，
不是编译错误；不提供强行绕过该门的装载参数。

### 复现端点与 request 的原始调用契约

`recover_contract.py` 对同一固定原始输入做有界反汇编，当前检查266处关键指令、request
分配类型及具体 controller 的虚表/import，输出 `contract.json` 和63段 `.dis`。
这是静态分析命令，不是测试套件或原生驱动装载器：

```sh
python3 -I x9/usb/recover_contract.py \
  --kernelcache .local/x9/usb/native-20H392/kernelcache.macho \
  --out .local/x9/usb/endpoint-contract-new
```

需要 `capstone`。输入须是原始20H392 Mach-O，输出须是新私有目录，不分析已成功
boot候选。确认创建pipe的settings输入是**5×32-bit word**，不是wire descriptor；
hardware endpoint创建是**slot173→187**（slot169是外层gate），销毁是slot174→188。

**USB-5 静态里程碑：普通 count 是 actual，不是 remaining。** 原始
`IOUSBDeviceIORequest` 是 **0x68B 的普通 typed allocation，不是 OSObject**；
slot223 分配→retain memory descriptor→复制24B completion→gate slot225 提交。
同一原始 cache 的 `AppleSynopsysOTGDevice` 提供了具体生产路径：普通提交先清零
request `+0x48`，端点中断和 abort 路径累加
`programmed chunk bytes - min(sum(descriptor residual16), programmed chunk bytes)`；
Family completion 取该值，EP0 data-stage write completion 再直接累加到进度。
这是原始代码的数据流证据，**不是 MTU3 DMA 的实机结果**，不复制 OTG 寄存器实现。
完成 request 经 `+0x20` 链入 endpoint 的 `head@0x40/tail@0x48`，drain 脱链后
调用 slot252→Family completion。**endpoint 的 `+0x48` 是队列尾，不是 request count**。

| 请求种类 | request `+0x40` | request `+0x48` / 回调第4参数 |
|---|---|---|
| 普通 slot223，flag30=0 | requested bytes64 | accumulated actual bytes；回调只取低32位 |
| 等时 slot224，flag30=1 | frame count32 零扩展到64位 | caller frame-array pointer64，**不是 byte count** |

回调前3参数仍为 target、parameter、status32。`flag@0x31=0` 时，**回调之后**
release descriptor、清 request `+8`，再经 slot227 free request；非零跳过两种清理。
gated submit 非零返回时，外层直接 release/free，**不由外层再发 completion**。
slot226/227 的默认实现是分配/释放，不应沿用“request recycle pool”的猜测。

原生 error/abort 回调可能带部分 actual；自有 tracked TX 在非COMPLETE状态仍没有
合格的部分字节计数，`actual=0` 不能证明零wire，**不可直接映射为原生失败计数**。
原生 timeout 的到期/取消/在飞所有权、callback reentrancy、全部pure virtual、
168→272局部ABI和IOUSBDeviceInterface/mux绑定仍未完成；输出保持
`abi_compatible=false` / `runtime_loadable=false`，不生成可执行C++ ABI。
本里程碑没有改运行内核、重建成功候选、安装工具或操作设备；USB-5是交接号，
不表示新的USB协议版本。

### 原生 ABI 与真实配对会话入口（尚不使原生内核栈可装载）

`native_abi.py` 从同一20H392原始cache与冻结`IOService.cpo`恢复**双向、精确符号**
映射，不使用统一`+104`偏移。没有资格证据的无名/歧义槽保留未映射；三个USB模块中的相邻
`ldr/blr`仅是虚调用候选，receiver类型未确认就不生成补丁。结果保留
`abi_compatible=false/runtime_loadable=false`；仍需逐类几何、override反向调用、
request timeout/DMA所有权、原生MTU3后端及模块注册，不可直接加载原始kext。

**USB-NATIVE7**：`IOService` slot0已通过原始非释放析构→D0 typed-delete及冻结
D1/D2/D0的真实branch relocation确认其角色，映射到current slot0；仍是
**167个精确符号 + 1个析构角色**，不伪造原始D1符号或称字节等价。

```sh
python3 -I x9/usb/native_abi.py \
  --kernelcache .local/x9/usb/native-20H392/kernelcache.macho \
  --io-service-object .local/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/iokit/DEVELOPMENT/IOService.cpo \
  --out .local/x9/usb/native-abi-new
```

`native_bridge.py`继续内核侧局部ABI施工，恢复19个原生类的metaclass initializer、
父类、allocation size及实例vtable；7种父类的原始/冻结allocation size均相同，
**不代表字段布局或构造/析构兼容已经证明**。生成symbolic expanded-vtable模板：
从current parent defaults建立新表，再按精确映射覆盖native override/tail，从而
同时描述native→current及current→native dispatch，不修改全局XNU虚表。

| 原生类 | stock slots | 局部模板 slots（尚不可调用） |
|---|---:|---:|
| `IOUSBDeviceController` | 232 | 336 |
| `IOUSBDeviceInterface` | 199 | 303 |
| `AppleUSBDeviceMux` | 168 | 272 |
| `IOUSBDevicePipe` | 29 | 53 |

有界must-dataflow跟踪vtable origin与实际x0，分支合流只保留一致事实，调用丢弃
caller-saved provenance；高半区ARM64地址须从Capstone signed immediate归一化，
不能漏掉taken branch。另分别处理实例vptr与含两字头部的`__ZTV`导入，避免将
DMA的super `clear/prepare/complete`错移两槽。当前386个调用receiver已静态确认，
其中284个有不同的load编码计划；它们覆盖instance与super/header dispatch，
**不是旧451个相邻候选全部完成，也不是补丁已应用**。未知间接调用还包括callback，
不把其数量当成全部未知虚调用数。

```sh
python3 -I x9/usb/native_bridge.py \
  --kernelcache .local/x9/usb/native-20H392/kernelcache.macho \
  --object-root .local/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET \
  --out .local/x9/usb/native-bridge-new --require-loadable
```

输出private `bridge-plan.json`保存模板、before/after instruction word及固定输入。
NATIVE16另按**同一load而不是同一个call**进行有界may-taint分析：寄存器别名传播、
分支合流取并集，逐一核对全部间接consumer；存储、未知使用、可能传参/返回或带值
越出CFG都拒绝。`load_consumer_plans`保留每个load的消费者、失败PC/原因与method
边界；`load_consumers_qualified`只说明这个局部CFG内没有额外使用，**不等于所有
producer/入口已证明或可以改指令**。原件中`0059cc6c8`载入的函数先存`[sp,#8]`、
`0059cc6d4`调用，随后`0059cc72c`重取并在`0059cc740`尾调用，旧单call receiver
分析漏了后者；新分析在stack spill即拒绝，不把第一次调用的证明用于整个load。
尚缺完整父类field layout、metaclass虚表、DMA/timeout/abort及relocation/bootstrap，
故`producer_entries_qualified=false`、`instruction_patch_qualified=false`、
`callable=false`、`runtime_loadable=false`，`--require-loadable`退出**2**。
不输出/装载修改后的Apple段，不启用原生controller或USB mode，不提供绕过门开关。

### USB-NATIVE8：设备端请求所有权组件（已编译，尚未接入原生后端）

`X9USBRequestOwner.h/.inc`由`SweetX9USB.cpp`编译，是真正的kernel C++组件，
不是新的ABI统计脚本；**目前没有实例或运行调用者**，不注册原生controller，
`X9USBAppleNativeStack`仍为false。它只解决普通request的内存／完成所有权，
不等于MTU3原生端点后端、Apple模块装载或Finder配对已完成。

后续**唯一USB worker**上的调用顺序：

1. 一份长期存活的owner执行`bindWorker()`。ABI-qualified adapter将普通native
   request作为opaque pointer传入`accept()`；descriptor必须已适配current XNU
   虚表。iso／reusable flags必须透传，非零拒绝；requested超过32-bit也明确拒绝。
   owner额外retain descriptor；非零长度执行prepare，失败只撤自己的retain，
   原始Family producer仍处理gated reject，不发completion。
2. 最终native callback必须与原始gated-submit串行；使用下述USB-NATIVE9 bridge时，
   worker只调用不释放request的queue hook，真实callback交给同一workloop gate。
   `beginChunk()`向worker的已pin bounce填USB IN数据，最大4096B；
   `publishChunk()`成功后立即发布HWO，不插入其他owner调用。
   Staged未发布可以`rejectUnpublished()`；发布后只能走真实STOP／retirement。
3. worker用单调ns调用`expire()`，或显式`cancel()/close()`。
   Active只改Stopping，不发回调、不complete、不重试；Ready／Staged才可以安全退休。
   timeout从首chunk发布开始，跨chunk保持同一deadline；0禁用，不猜stock的5000ms条件。
4. `retire()`只接收worker实采证据，不是poll/IRQ本身。正常完成需HWO clear、CPR
   到terminator、STOP inactive、actual可信；abort也必须有STOP＋独立可信partial
   actual。OUT先在worker失效bounce cache，再交字节拷回descriptor；actual始终是
   wire bytes而非remaining或拷贝成功长度。0-byte请求也需要真实ZLP退休证据。
5. `drainCompletions()`先detach有界batch，complete成功后才交给**尚待实现／资格化的
   Family completion adapter**，该adapter负责原始callback及release/free。
   owner只在adapter返回后release自己的额外descriptor retain，不再解引用request。
   回调新提交／取消只进入下一批，递归drain拒绝；同一descriptor在当前回调结束前
   不能复用。完成失败返回真实error；STOP／actual／complete不确定会永久隔离request、
   保留memory及完成义务并关准入门，worker也必须保留其QMU allocation。

ticket包含generation和不重用serial，chunk另有serial；重复或旧代事件不能完成新请求。
`close()`先关门，再要求workerSTOP所有Active；旧代只能退休。只有全部slot（包括
回调及quarantine）为空才能`reopen()`换代，无自动reset或quarantine清理入口。

**边界**：这些是本组件的显式保守合同，不宣称已复刻stock OTG全部时序。
原生IOCommandGate到唯一worker的同步交接、逐endpoint队列、QMU publication绑定／
abort partial计数、EP0 native data stage、Family completion双向ABI及模块bootstrap
仍未接通；当前ECM/private transport路径没有调用此组件，行为不变。
仅生产`SweetX9USB.o`本地arm64交叉编译通过，未运行测试套件、kernel链接、boot构建
或实机DMA／Trust／Pair／TLS验收，不能据此启用原生栈或交付Finder boot。

### USB-NATIVE9：原生 gate／唯一 worker／完成事件源交接

`X9USBRequestBridge.h/.inc`与生产`SweetX9USB.o`一起编译，C侧唯一worker已调用
`x9_usb_native_worker_poll()`。**当前registry为空，没有install调用者、MTU3 native
endpoint backend或Family completion adapter**；mode0仍是EP0日志worker，mode1/2
仍走原ECM/private路径。`native_apple_stack=0`、原生service与USB identity未改变。
后续NATIVE10补充了未安装的bulk backend；本节记录NATIVE9交接合同。
这不是“原生栈已经能加载”的开关。

未来必须在current-layout原生controller workloop、逐endpoint backend及ABI全部资格化
后才能`install()`；当前真实backend必须提供`validateEndpoint/poll/rearm`，不填成功stub。
registry持有bridge/workloop/event source到本次boot结束，backend/context与callback
context也必须保证该寿命。opaque request/context不被猜成OSObject，安装不启用native mode。

窄流实现如下：

1. 原生gate内`submit()`复制输入到16个有界exchange entries，并独立retain descriptor。
   worker未绑定则立即NotReady；同request/descriptor重用拒绝；不把调用方stack指针
   留给worker。**2秒timeout只可撤未claim的Queued**，锁内撤销后才能返回Timeout。
2. worker先Claimed，再在exchange锁外validate endpoint／owner.accept／prepare。
   一旦Claimed，调用方无deadline地等真实accept/reject，不能将已接受request误当reject
   返回错误交给Family释放。prepare长期阻塞会阻塞worker／等待方：本轮没有可取消prepare
   或watchdog恢复合同，不用返回模糊timeout掩盖这个约束。
3. admission result与completion result分字段。调用方读到真实success才ack；scheduler
   经`workerNext()`按serial取一次acknowledged request，再交真实MTU3 backend执行
   begin／publish／STOP／retire。ticket和descriptor保留到最终完成，不能复用accepted槽。
4. worker完成hook只记录CompletionQueued并signal事件源，不同步进入原生gate。
   `checkForWork()`在**同一IOWorkLoop gate**内detach有界batch，再执行真正Family adapter；
   descriptor的exchange retain延续到该callback返回之后，避免owner队列hook返回就释放
   最后内存引用。回调新提交能再次等worker，因为worker并未等待原生callback；递归
   completion drain拒绝，当前Delivering槽在callback结束前不可复用。
5. complete失败被latched并可由`workerFailure()`读到，原请求仍隔离／保留；bus-on不自动
   清理quarantine或reopen。跨代、重复completion及非唯一worker调用不会被算作正常完成。

同20H392原件新资格证据：`submit_normal`在`5cf10dc`成功直接branch到`5cf135c`，
之后只有状态／栈／寄存器返回尾声，**没有再次解引用request**。`recover_contract.py`
现在检查该branch及全部epilogue指令并记录`accepted_return`。这仅证明这个普通producer，
不是所有上层caller／native gate字段／completion receiver都已资格化；仍不可加载旧vtable。

**尚缺**：installer与真正native controller、endpoint FIFO/QMU backend、原生abort gate
命令、在bus/reset/fault之前完成native quiesce及generation映射、descriptor/layout/
Family callback adapter和bootstrap。当前poll hook不能代替这些硬件／生命周期合同。
只完成两份生产对象交叉编译与固定原件静态资格，无kernel link、新boot或设备请求。

### USB-NATIVE10：MTU3 ordinary bulk 后端（未安装／未实机验收）

本节保留NATIVE10已提交合同；当前NATIVE11增加显式控制命令和IN ZLP，见下一节。

`X9USBMTU3Backend.h/.inc`提供boot-static `operations()`，可交给未来资格化的
controller installer。C侧`x9_usb_native_qmu.h/.inc`与现有私有传输共用
`prepare_gpd/publish_gpd`，实际写MTU3 GPD/HWO/SAR/QCSR，不实现成功stub。
**没有installer或native profile/state setter**；`native_apple_stack`仍0，原生
enable/validate/stage均拒绝，不会接管现有mode0/1/2或冒充原生Apple类。

- 只接受USB地址`01/81/03/83`、普通非零bulk；direction必须与地址一致。
  endpoint须由未来native控制面先独占enable；不能与legacy已启用端点共享。
  FIFO用经capacity检查的固定非重叠512B槽，复用原五份16KiB boot-pinned分配。
- 16-entry scheduler按同endpoint owner serial排队，一方向一次一chunk、每poll
  一chunk（最多4096B）。`beginChunk`拷入bounce，C先stage/cache所有可拒绝输入，
  最后调用owner `publishChunk`并立即发布HWO。不在4096B分块间插入ZLP。
- Probe只看当前serial/descriptor的HWO、CPR、STOP和长度；IN还要求**STOP/flush
  之前**TX_FIFOEMPTY=1且TX_TXPKTRDY=0，避免把DMA写入FIFO当成已经发上wire。
  OUT失效bounce cache后才拷回current-layout descriptor。成功实际字节累计后交
  原workloop gate的completion，worker不进入Family callback。
- Timeout/offline/reset只会STOP并采样；HWO/CPR未退休的partial actual没有证明，
  不能报0或用IN programmed length。owner和physical记录一起quarantine并断开，
  保留descriptor/DMA到本次boot结束。quarantine错误现在latched，包括retire失败。
- `reset_data`先关闭native准入、drain/STOP，再检查physical barrier；不确定记录
  阻止reset/重用。fault也先native quiesce，legacy EP0/data不会处理native端点。
  offline completion早于submit ack时，ack重新signal事件源，不丢完成通知。

**仍缺**：native controller installer、exclusive profile/configuration/event状态
路由、EP0 SETUP/data/status、gen2 TX ZLP的BMU/length-error路径、显式worker
rearm/generation合同、abort gate命令、Family request flags/descriptor/object geometry
和completion双向ABI、模块relocation/bootstrap。当前普通bulk合同不够装载原始
DeviceFamily；不能启用native flag、声称Finder/Trust/Pair/TLS/App安装完成。
编译及固定源码结果见`NOTES.md`活动交接；本轮无设备操作或额外测试套件。

### USB-NATIVE11：显式 rearm／定点 abort／普通 IN ZLP（未安装）

`X9USBRequestBridge::rearm()`和`abort(ticket, &result)`只允许在同一current-layout
controller workloop gate内调用，没有新的用户态selector、自动执行入口或native flag。
单个有界control槽复制参数；2秒deadline只可撤回尚未Claimed命令。Claimed后等待
真实worker结果，不在STOP/complete期间返回模糊timeout。

- **Rearm**：exchange必须完全Free（包括尚未deliver及Delivering回调）、owner空且
  无latched failure、scheduler空、physical records空、QMU inactive、真实bus连接／
  speed／configuration正常，且独占bulk IN/OUT成对enable。只有这些条件通过才
  `owner.reopen()`换64-bit generation。不会connect、configure、reset端点、改身份或
  清quarantine；offline/reset恢复必须另行显式请求，bus-on不自动rearm。
- **Abort**：target须匹配generation+serial，只有Accepted或已CompletionQueued请求
  可处理。取消Ready可安全终结；Active先Stopping，再由原backend STOP/retire；
  worker等到descriptor complete成功且完成事件已入原gate队列，才返回success和
  `completionStatus/actual/actualQualified/completionQueued`。回调尚未执行；其原始
  完成义务及exchange retain保留。未知actual／quarantine返回error且资格位为false。
  **任意abort返回都不授权调用方free/release/re-submit accepted request**。native回调
  内对同批Delivering请求abort、或rearm，直接Busy而不是等待自己的gate。
- **IN ZLP**：发布真实0-length GPD，仍须HWO-clear/CPR-terminator/STOP/flush前FIFO
  drained证明后才能成功。仅在当前ticket、当前GPD、普通IN requested=0、sole TX
  LEN_ERR、idle FIFO全部匹配时，按OPPO `qmu_tx_zlp_error_handler`的DMAREQEN off →
  TXPKTRDY → GPD BPS/HWO → DMAREQEN on → RESUME顺序发BMU ZLP。只ack这个观察到
  的LEN_ERR，同ticket只可发一次；该轮返回EAGAIN，不把BMU publication当成完成。
  BMU前另核native endpoint、suspend/configuration、SOFT_CONN/speed和bus-cycling，
  不能仅信外层worker的coarse available；失败转STOP/隔离，不延迟重放。
  timeout/abort/offline时不新增ZLP。普通0-length OUT／EP0、iso和reusable仍拒绝。

这些是当前组件的typed控制合同，**不是已证明的原始AbortPipe虚方法ABI**。
registry仍无installer、`native_apple_stack=0`；没有DeviceFamily装载或Mac配对。
仍需native controller、EP0 SETUP/data/status与configuration/event路由、原生字段／
flags／descriptor／Family双向dispatch与completion、模块relocation/bootstrap，再验
真实枚举/Trust/Pair/TLS/安装。编译和源码来源见`NOTES.md`活动交接。

### USB-NATIVE12：原始普通 request 提交／完成适配器（未绑定）

`X9USBNativeRequestAdapter.h/.inc`作为生产kernel C++组件编入`SweetX9USB.cpp`。
它沿原20H392 slot223→225请求到`X9USBRequestBridge`，不是Apple类声明或加载器。
0x68B请求字段宽度及offset均有编译断言，来源是原始wrapper、plain allocator和
Family completion的读写；没有对request做OSObject cast或猜虚表。

- `bind(loop, controller, familyComplete)`只可在已安装bridge的**同一**workloop gate
  调用一次。binding加锁发布，loop保留到boot结束；controller及原Family completion
  thunk必须由后续loader先资格化并pin。函数指针非空本身不证明ABI正确。
- `submit(controller, request, memory)`要求live原始plain allocation，且`memory`
  **已具备current-layout descriptor几何／dispatch资格**、与request+8为同一对象。
  适配器不从raw+8盲转指针；原`ioGated`还须有资格化glue来取得这个typed memory。
  只接01/81/03/83普通bulk；flag30/31、旧DMA@10、next@20、非零status/actual及
  超32位长度均拒绝，不清掉旧所有权。timeout按原word32传递，不擅加5000ms。
- 成功后保持16-entry adapter记录直到原gate实际回调结束。backend真实退休并且
  descriptor complete成功后才写原request status32@28和actual64@48，再调用
  **原Family completion**；原callback record不替换，原Family负责callback及其
  original descriptor release/free。调用后不再读取request；bridge随后仅释放
  自己额外持有的current-layout descriptor。完成前的身份／字段被非法修改会触发
  明确ownership panic，不调用可能已经变成另一个对象的callback。
- reject不修改、release/free或回调原request；定点abort仅按已记录ticket，不读取
  任意raw对象。同批Delivering时Busy；任意abort返回不授权caller free/re-submit。
  Rearm要求adapter与bridge记录、callback都已退干净，仍不启用native mode。

当前**没有bind/submit调用者，也没有native controller/bootstrap**；`native_apple_stack`
仍为0。本组件缩短了原始普通请求到worker的接线缺口，但不解决原始AbortPipe ABI、
EP0/配置事件、全部pure-virtual、descriptor/class-local双向ABI和模块加载。

### USB-NATIVE13：独立 EP0 PIO／同 gate 控制事件（未安装／未实机验收）

原件的`inputSetupPacket`把8字节写到独立的`IOUSBDeviceControlRequest+0x10`，
state@0xc置2；`processSetupPacket`区分IN=3、OUT有data=4、OUT无data=7，经controller
slot213通知，再到原始controller setup dispatcher。原OTG先从其memory对象取得
SETUP输入；这不是0x68B普通bulk request，不复用普通request adapter或其callback。
静态恢复现在包含35个窄region和两条原始import cell校验；仍非callable ABI。

- `x9_usb_native_ep0.h/.inc`在**原有唯一USB worker**中处理native EP0（目前flag
  仍0）。只读8B SETUP；按OPPO MTU3的W1C和DPHTX方向序列进入data stage。
  USB2 EP0固定64B packet，最多65535B复制式PIO，IN短响应的64B整数倍补真实ZLP。
  FIFO写入／TXPKTRDY置位不增加actual；实际TX ISR＋TX/FIFOFULL清零才推进。
  OUT逐包按剩余容量校验，最终RX保持到真正gate handler接受status。
- `X9USBControlBridge`必须在已经安装request bridge的**同一workloop gate**一次安装。
  SETUP／OUT_DATA／COMPLETE带boot-unique ticket和exact sequence；event source
  在原gate调用未来资格化handler，不在worker、usb_lock或registry锁内调Apple代码。
  数据在两份boot-static缓冲间复制，不让64KiB数组进入16KiB metaclass allocation
  或线程栈；不retain未知native object/descriptor，也不把函数非空当ABI证明。
- 回复成功只表示命令入槽；worker等callback返回及精确ack后才消费，不使用native
  指针/调用者栈。status发布后另一次poll观察DATAEND及TX/RX/SETUP/STALL/FIFOFULL
  清零、无reset/SETUPEND/offline，才给success/status_complete。SET_ADDRESS沿OPPO
  的DEV_ADDR-before-status寄存器顺序，**实机地址锁存尚未验证**；不提前写配置状态。
- reset/bus-off/fault/new SETUP保留claimed快照，撤销旧reply，先给旧ticket失败终态
  再处理新SETUP；未知pending IN actual不伪造0。5s是自有控制watchdog，不冒充原生
  timeout语义。suspend时保留TX ISR到resume，不发布数据；离线无新的FIFO/MMIO提交。

**没有installer或handler调用者，native_apple_stack=0，不能枚举/配对。**
还须原始control object/response/completion双向glue、独立bus reset/suspend/resume事件、
configuration/alternate/endpoint生命周期、全部父类/descriptor/metaclass/load consumers
和模块bootstrap；不能仅凭PIO状态机启用native flag。AppSync runtime未staging，
完整越狱bootstrap和实际Trust/Pair/TLS/安装/启动/持久性验收仍未完成。
不得把`bind`、单编／完整构建或原件资格统计当成原生配对／App安装成功。

### USB-NATIVE14：EP0 普通请求的分阶段完成（未绑定）

原件 `sendControlInDataGated` 在首个 EP80 data request **入队成功后立即**另排
一个 zero request，条件是非零短响应、整 packet 倍数、control+88 bit0 清零。
这不是 status：data callback 为 `dataStageWriteCompletion`，zero callback 为
`zlpCompletion`；后者忽略 error 参数并直接宣布 state8。OUT/no-data 的 IN
status 则是另一个 zero request，callback 为 `statusStageWriteCompletion`。
`recover_contract.py` 已记录 **216 observations／50 bounded regions**，包括
原 control 的独立 descriptor retain/prepare(direction0)/complete/release 配对。
这些仍是静态证据，`abi_compatible=false/runtime_loadable=false`。

- `X9USBNativeControlAdapter` 独立持有两条 EP80 普通 request 记录，保留原始
  callback record／Family cleanup。绑定必须与普通 adapter 的 controller、
  workloop、completion thunk **全部一致**，并另资格化 packet64／flags bit0=0；
  原 Family speed>2 用512，不能直接传 MTU3 raw speed3。
- native-request PIO mode **不自动补**短响应 ZLP。第二 zero request 可在 SETUP
  callback 返回前排入独立命令，不覆盖 data。真实 TX 退休 → 同 gate data callback
  返回并 ack → 唯一真实 ZLP → zero callback 返回并 ack → status；旧 standalone
  `SEND_IN` 的自动补包行为保留。
- descriptor 由 adapter 额外 retain、独立 nested prepare(kIODirectionOut)、复制到
  boot-static buffer；原 Family 的 prepare/complete 责任不转移。data callback 前
  先 complete 本次额外 prepare，original request 可在 Family callback 内释放；
  此后仅释放额外 descriptor retain。不把 DATAEND／入队／FIFO write 当完成。
  data 和 zero 允许共享原 fallback descriptor（zero 不 prepare），不释放它的原始引用。
- unknown actual／descriptor complete 失败明确隔离并保留 request；failed ZLP
  不调用忽略 error 的原 callback，避免暂时宣布成功。failure latch 拒新 SETUP，
  新 ticket 仅用于取消，不能清掉旧隔离。typed cancel 不触碰 MMIO 或伪造计数。
- bulk rearm 现在也检查 control callback／adapter／PIO 全部 idle；同 gate 的
  EP0 或 bulk callback 内 abort/rearm 返回 Busy，worker 二次检查 pending SETUP
  和真实 EP0 packet/status bits，防止绕过控制请求责任。

**通用 COMPLETE handler 仍待 loader 实现**：OUT/no-data status callback 已由
adapter 完成，不能再合成第二次；IN 只用真实 status OUT 证据进入原 control
inputDataPacket/final callback。错误还需原 control 的终态清理。
这些接口没有调用者、没有 native controller installer，不设置 native flag；
typed abort 仍只覆盖 bulk，不是原 AbortPipe ABI。原生枚举／配对、AppSync staging
与完整越狱 bootstrap 的缺口未因本适配器消失。

### USB-NATIVE15：逐包 OUT 与原 control 方法接线（无模块 installer）

原 `inputDataPacket` 的状态跳表确认 **state4 会 panic，state5/6 才接收数据**。
先由 `receiveControlOutData` retain/prepare descriptor 并通知 state5/6，才能启用
PIO RX。聚合 payload 不等价于 packet：64B 数据＋最后一个 zero 短包必须是两个
调用，不能把 actual64 当成整个短响应。原 slot210 接收的 packet-report 是临时
零填充 **0x68B stack record**，不是可以放进普通 adapter 的 heap request。
当前分析为266条指令／63个region，另资格化packet/reset跳表与inputData/reset import。

- `RECEIVE_OUT_PACKETS` → 每个真实RX（包括zero）生成独立`OUT_PACKET`，带本包
  offset/length/final及累计wire actual。每包RX保持到gate返回、exact ack与显式
  `CONTINUE_OUT`；final只能走原status request，不能continue或提前发送status。
  `RECEIVE_OUT`聚合模式仍留给原独立handler，不是原Family packet入口。
- `X9USBNativeControlGlue::install` 将真正的handler接到同gate bridge及普通EP0
  adapter。未来loader必须提供**已资格化**的inspect/inputSetup/processSetup/
  inputData/reset leaf thunks并boot-pin所有目标，不能把opaque control转成当前
  OSObject或硬编码原件地址。安装失败不让永久bridge成为半活的实现。
- SETUP8 → 原inputSetup（匹配slot210 event12的真实stack记录）→ state2 → 原
  processSetup。原controller slot213 thunk接`stateChanged`，OUT只有已prepared
  state5/6才启用逐包PIO；随后每包进入原inputData，本包descriptor write/progress/
  event11/state通知返回后才继续。当前仅接受native receive requested==setup.wLength，
  不冒充已支持“调用者更小的receive buffer＋原drop/clipping”合同。
- IN COMPLETE只在真实status退休及state8后调用原inputData(nullptr,0)，执行原
  final callback/clear/state1；OUT/no-data status callback已由普通adapter执行，不
  合成第二次。known error且所有普通请求已settled才调原reset；unknown actual、
  native几何/状态错误或任何quarantine保留原对象/descriptor直到boot结束，不清隔离。
- 原stallSetupRequest对OUT0/IN80的两个halt-on调用和state9，由同gate
  `requestStall`幂等复制到worker；callback返回前不MMIO。可撤未发布命令但不撤
  accepted request的完成义务，in-flight actual不猜。bridge在adapter callback前
  观察terminal error，native错误回调里的stall不再发布新包；clear-halt/非EP0不映射。
  rearm现在同时核对glue原对象state1/无data memory、adapter及worker全部idle。

**仍无Apple模块loader／native controller实例或profile开关。** 新handler有接线实现，
但没有提供以上原件leaf thunks／controller slot210/213/halt入口的安装者；父类字段、
逐load全部consumer、expanded/metaclass vtables、其它pure virtual、endpoint配置／
bus事件和模块relocation/bootstrap必须先资格化，`native_apple_stack`仍为0。
不能把这份生产编译当作Finder枚举、Pair/TLS或AppSync安装已成功。

### 真实主机配对入口（依赖原生内核路由）

新增`pairing.py`走**系统原生usbmuxd、真实lockdownd**，不使用旧自有mux草稿，
不改守护进程或信任状态。它是主机端代码，不是缺失的DeviceFamily驱动。
仅`--list`的原生daemon发现已实跑；其余操作当前没有X9原生USB入口，**未实跑**。

```sh
# 当前可只读执行；设备不在原生列表就停止，不自动选择别的设备
python3 -I x9/usb/pairing.py --list
# 原生栈接通后，显式选择上一步的USB设备ID
python3 -I x9/usb/pairing.py --query-type --device-id <ID>
```

首次配对必须由操作者明确选择执行，依赖Python `cryptography`：

```sh
python3 -I x9/usb/pairing.py --pair --device-id <ID> \
  --record .local/x9/usb/pairing/private-record.plist
# 仅收到真实PairingDialogResponsePending时，用户在设备解锁/接受原始Trust UI后，
# 才可手动再次执行同一命令；不自动轮询/重发，不把pending或denied算成功。
# Pair真实接受并保存主机record之后，独立验证会话：
python3 -I x9/usb/pairing.py --session --device-id <ID>
```

`--pair`读取原生BUID/设备公钥，生成真实RSA证书，发送原始Pair请求；接受后才
保存主机pair record。不清旧记录、不Unpair、不修改设备USB限制。私有record只可在
`.local`下，0600/原子落盘；Pair或Save超时留in-flight状态，禁止下一次隐式重放。
`--session`只读取所选设备的原生记录：StartSession→按原始响应升级TLS→验证根CA
并精确pin已配对DeviceCertificate→会话内QueryType→StopSession。没有关闭证书
验证或自动降级TLS/密钥长度。普通QueryType、Pair接受、会话成功分别报告，不互相
冒充。证书、私钥、BUID、设备标识与SessionID不打印、不入git。
