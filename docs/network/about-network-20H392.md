# iOS 16 / 20H392「关于本机 → 网络」显示逻辑

日期：2026-11-11。只读分析原 Apple 组件、已有 214 启动日志与桥源码；未修改设备、系统组件或运行代码。

## 结论

**这行显示每张 SIM 的本地化运营商名称，不是 Internet 连通性检测结果。**

20H392 的原生 `PSGAboutDataSource._loadValues` 读取
`PSUICoreTelephonyRegistrationCache.localizedOperatorName:`，名称长度为零
（包括 `nil`）时显示 `NO_NETWORK`，简体中文正是“不可用”。此分支不直接检查
IMS 注册、PDP、IP 地址、DNS 或收发包计数。

当前已确认的相关实现缺口是 **NAS `0x0044 / Get PLMN Name` 未实现**：214
运行日志中有 10 条明确的 `error 94` 拒绝记录。原 Apple 响应处理器在此错误
分支直接结束，不产生该次运营商名称更新。这是名称链路的实际断点，不需要先
假设“因为没有 IMS，所以设置显示不可用”。

**证据边界**：本轮没有在设备 Preferences 进程里采样最终 NSString 或跟踪
CommCenter 全部名称来源/回退。因此确认的是显示规则和实际名称查询断点，
不是证明所有可能名称来源都为空，也不是已完成修复验收。

## 显示层：地址级证据

`GeneralSettingsUI` UUID：`51354c5d-9641-3395-8762-72d182a20f5d`。
以下地址均为 20H392 arm64 未加 ASLR slide 的 VM 地址。

`-[PSGAboutDataSource _loadValues]`：`0x207455b4c`。

```objc
// 依据机器码整理的伪代码，不是 Apple 源码。
for (context in subscriptionContexts) {
    if (!mobileEquipmentInfo(context)) continue;
    key = format(@"%@.%li", @"NETWORK", context.slotID);
    name = [registrationCache localizedOperatorName:context];
    if (name.length != 0) {
        setValue(key, name);
    } else {
        reject = [registrationCache rejectCauseCode:context];
        setValue(key, mainBundleLocalizedString(@"NO_NETWORK"));
        // reject 为 2、3、6 时另增加运营商拒绝原因详情入口。
        // 这不改变“不可用”的上述判定。
    }
}
```

- `0x207455cb4`：调用 `localizedOperatorName:`。
- `0x207455cc8..ccc`：调用 `length`，零值跳向 `0x207455ce4`。
- `0x207455d34..d60`：取 `NO_NETWORK` 本地化文案并赋给该槽网络字段。
- `/Applications/Preferences.app/Localizable.loctable`：
  `NETWORK` = `Network` / `网络`；`NO_NETWORK` = `Not Available` / `不可用`。
- reject 的 `2/3/6` 来自 `cmp #6` 与 bit mask `0x4c`，不是“不可用”的必要条件；
  没有 reject code 时同样显示“不可用”。

## 名称缓存与 CoreTelephony

`SettingsCellularUI`：

| 方法 | 地址 | 行为 |
|---|---|---|
| `fetchLocalizedOperatorName` | `0x1f130b358` | 遍历 subscription contexts，调用 `getLocalizedOperatorName:error:`；无错误时按 slotID 存入字典 |
| `localizedOperatorName:` | `0x1f130b648` | 整个字典不存在时首次抓取；随后按 slotID 查值，不是在每次取值时重新查询 |
| `operatorNameChanged:name:` | `0x1f130b720` | 收到变化后重新获取本地化名称；名称非 nil 且无错误才更新槽缓存 |

`CoreTelephonyClient(Registration).getLocalizedOperatorName:error:` 位于
`0x1881b7d40`，`0x1881b7e1c` 调用 `copyLocalizedOperatorName:completion:`。

因此即使底层以后补好，仍需要正确的名称变化通知/缓存失效或新鲜 Settings
进程来验证；不能仅观察基带注册状态，就认定设置缓存已经更新。

## 当前实际断点：Get PLMN Name

原生 `libCommCenterMCommandDrivers.dylib`：

- `QMINetworkRegistrationDriver::getPlmnName` = `0x1f1afe400`；
  `0x1f1afe450` 以消息号 `0x44` 构建请求。
- 响应 callback = `0x1f1b0ad74`；`0x1f1b0add0..add4` 检查错误。
  非零时记录 `Get PLMN Name failed: Error in response with code %d (%s)`，
  跳到 `0x1f1b0b20c` 收尾，跳过名称解析和通知。
- 成功路径解析 `PlmnNameInfo` / `NetworkNameSource`、解码名称并构造
  `OperatorNameUpdate`（`0x1f1b0b110`），随后调用更新接收方。

214 原始日志的 LF 行号：

```text
LF8327  30.988s  NAS request port 0x4101 txn17 msg0x0044
LF8328  30.988s  reject service0x3 msg0x0044 txn17 error94
LF8338  30.990s  NAS request port 0x4102 txn15 msg0x0044
LF8339  30.990s  reject service0x3 msg0x0044 txn15 error94
```

共 10 条明确拒绝：LF8328/8339/8450/8528/8634/8650/18937/18947/19077/19095，
覆盖约 30.988–99.018 秒。单独的 request 格式只打印了 6 条，不能将其误报为
只有 6 个事务；拒绝日志和请求摘要的数量不必一致。

`x9/kernel/src/iokit/Kernel/arm/X9QMIAT.inc`：

- 第 8 行定义 `X9Q_UNSUPPORTED = 94`。
- `x9q_supported`（626–630 行）NAS 普通查询白名单只有
  `0x20/0x24/0x4d/0x4f`，没有 `0x44`。
- 第 724 行未支持请求直接 `x9q_reject(..., X9Q_UNSUPPORTED)`。
- COPS 解析（501 行起）接受 numeric 格式提取 MCC/MNC 和 AcT，并没有因此完成
  Get PLMN Name 或运营商文本名称更新。
- 此文件与 214 实跑源码基线 `0e3681b` 无 diff，源码与拒绝日志一致。

## 不要混为一谈的几项

| UI / 功能 | 本轮确认的来源或边界 |
|---|---|
| 关于本机“网络” | 每槽本地化 operator name |
| 关于本机“运营商”（例如中国广电 54.1） | `_carrierVersion:` `0x207453f30`，读取 **CarrierBundleCache** 的 `carrierName:` 与 `carrierBundleVersion:`，不是上面的 RegistrationCache |
| IMS 状态 | 独立 `_IMSStatusString:` `0x207454618`，读取 `IMSStatusVoice:`、`IMSStatusSMS:` |
| 信号 / 4G / 5G | 注册、RAT、信号路径；其成功不能替代名称查询 |
| 能否联网 | 还须真实下行、DNS/TCP 等；改对名称不能证明 214 的 RX0 已解决 |

## 后续修复与验收

应实现按真实 SIM/请求 PLMN/RAT 获取名称的 NAS `0x44` 路径，正确处理长短名、
编码/长度、来源、无名称及迟到响应，并驱动原生名称更新。先核实 MTK 实际名称
接口和返回格式，不用 carrier.plist 名称或硬编码中文冒充当前注册网络。

验收分两件独立进行：

1. 新日志确认不再走该 unsupported 分支、真实名称被 Apple 接收；新鲜设置缓存
   的 `getLocalizedOperatorName:error:` 返回非空且无错误，“网络”显示真实名称。
2. 单独确认蜂窝 RX、DNS/TCP/网页，不将第一项成功算作数据联网成功。

## 本地证据与重放

私有目录：`.local/x9/work/about-network-20261111/`。不将 Apple 原文件或设备日志入 Git。

```sh
python3 .local/x9/work/about-network-20261111/trace.py
python3 .local/x9/work/about-network-20261111/collect-runtime.py
git diff 0e3681b -- x9/kernel/src/iokit/Kernel/arm/X9QMIAT.inc
```

`trace.py` 依赖已有 `cc-5gsa-20H392/macho_evidence.py`、Capstone、本地 dsc 提取树
与 `/tmp/x9root` 根盘快照的 cache stub islands；不执行 iOS 组件。10 段函数原字节
均与现有根盘快照 cache 相同，记录于 `range-verification.json`；这只是快照一致性，
不是当前设备进程的内存采样。辅助反汇编器会启发式注释 ADRP/LDR，不能将无关
字符串猜测当作运行证据；上述结论使用明确符号、指令、CFString 和分支。

其他材料：`image-provenance.json`、`constants.json`、`localization.json`、
`runtime-evidence.json`、`214-network-excerpts.txt` 与 `*.asm`。原始 214 文件若之后
被替换，应先按 provenance SHA256 核实，不能把不同启动日志拼接复放。
