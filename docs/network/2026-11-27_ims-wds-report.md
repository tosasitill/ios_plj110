# 20H392 WDS 请求与 448 的 IMS 承载边界

`448.txt` 在两次 SIM2 拨号时都读到 IMS 原始注册状态 0。捕获中成功建立的是 SIM1 默认数据承载。WDS `0x5566` 和 `0x5568` 的请求名称已从 20H392 原件核实，分别属于蜂窝数据状态和动态数据换卡许可。现有桥对这两条请求返回 `0x5e`，仍缺对应的 MT6993 状态接口与确认合同。

## Apple 请求形状

静态输入是固定 20H392 的 `libCommCenterMCommandDrivers.dylib`，arm64 UUID `7C3F4685-46D6-3178-B3EA-B43E59F11923`。以下消息均为 WDS service `0x01`。其他 service 可以使用相同的消息编号。

| WDS | 20H392 构造与字段 | 448 观察 |
| --- | --- | --- |
| `0x5566` | `QMIDataCommandDriver::updateCellularDataStatusInfo_sync(bool)`，`0x1f19ccc64` 构造请求，`0x1f19ccdac` 写入 `CellularDataStatus`。TLV `0x10`，u8 状态。回调 `0x1f19d7c2c` 检查结果并在失败时记录 `SetCellularDataStatusInfo`。 | 8 次，TLV 摘录 `10 01 00 01`，全回 `0x5e`。 |
| `0x5568` | `QMIDataCommandDriver::notifyCellularDataSwitchingAllowed_sync(bool)`，`0x1f19dfc50` 构造请求，`0x1f19dfe3c` 写入 `DynamicCellularDataSwitchAllowed`。TLV `0x10`，u8 许可。回调 `0x1f19dffac` 检查结果并在失败时记录 `NotifyDynamicCellularDataSwitchState`。 | 3 次，TLV 摘录 `10 01 00 00`，全回 `0x5e`。 |
| `0x5569` | `QMIDataCommandDriver::updateCurrentDataSIM_sync(SwitchSimCause)`，`0x1f19d751c`。`NotifyCurrentDataSub` 携带 TLV `0x10` u8 订阅和 `0x11` u8 原因。桥在 `X9WDS.inc` 中只接受已经过 `020e` 确认的默认数据 SIM，并保护现有 bearer。 | 55.269 s 与 56.715 s 两次已确认的 SIM1 路由，原因值 3。 |

前两种请求的 TLV 类型可由构造路径引用的 RTTI `0x20dd2cd28` 与 `0x20dd2d638` 核对。表中的四字节摘录由日志 `10/1=01` 和 `10/1=00` 按 TLV 类型、一字节值的长度（小端 u16）、值还原。两个 Apple 回调在这条路径上只检查结果和记录失败。448 在两种请求都被拒绝后仍建立了默认数据会话，因而这个拒绝没有阻断该次默认数据会话。现有 `DATA_SET_CONFIG 0211` 还有默认数据 SIM 和启动设置字段，不能据请求名称把它绑定到这两个 Apple 通知。

## 448 的数据承载

`X9 BB: QMI request to …` 由 `X9Baseband.cpp` 的 400 条全局上限控制。应结合 `coachz_baseband.cpp` 的 COACHZ QMI 事务记录和 WDS 专用日志查询后段。448 最后一份 COACHZ 预算报告是 232.548 s 的 1,447 条、预算丢弃 0。WDS 在该预算内逐消息记录。

| 时间 | 同一捕获中的观察 |
| --- | --- |
| 57.453 s | WDS `0x20`，CID 15/14 分别请求 SIM1 mux 1 IPv4/IPv6，两条 Start 日志的 APN 长度均为 0。 |
| 58.412–58.413 s | 两个 client 在同一 SIM1 PDP3 上获得独立 lease。随后同一对 WDS CID 14/15 的 `0x2d` runtime settings 成功。 |
| 60.988 s、138.212 s | 两笔 SIM2 `0901` 后的 `0a05` 都报告 IMS 原始状态 0。拨号窗口未捕获 SIP 诊断或 ECPI2/132/6。 |
| 231.965 s | 同一对 WDS CID 发出 `0x21` Stop。 |

捕获可见的 `0x20` 和 `0x2d` 都归属于上述默认数据会话。448 没有可归属到 SIM2 IMS 的 WDS Start 或 runtime settings。这个观察覆盖 Apple 可见的 WDS 路径。MTK 内部 IMS context 是否曾建立、失败或由其他路径恢复，仍要靠同卡 MD 侧证据确认。

## IMS runtime settings 的后续合同

20H392 的 `QMIDataContextIPBase::parseImsSettingsFromResponse`（`0x1f1a97f14`）读取 `0x23` `ProxyIPv4List`、`0x2e` `ProxyIPv6List` 和 `0x32` `SnssaiPdnInfo`。普通 settings 路径在 `0x1f1a96508` 经 context 条件调用它，`ExtendedIPConfigChange` 触发的 IMS settings 路径在 `0x1f1a989b0` 也调用它。现有 X9 `0x2d` 分支只从有效 lease 中返回已知 APN 和 IP family settings，未提供对应的代理地址或 S-NSSAI。其 ACT 路径用 `APN_TYPE=1`，不能用该会话的结果补充 SIM2 IMS 数据。

下一轮要对齐同一 SIM 和订阅的实际 carrier bundle 与 provisioning 状态、IMS context 或 MD 侧等价承载、P-CSCF 来源、`0a05/4a01` 和 SIP REGISTER。只有同卡承载和字段来源成立，才有条件接 IMS runtime settings。拨号推进另以 SIP 与 ECPI2/132/6 验证。

## 离线复核

从集成工作树运行。`awk` 只输出 WDS 事务头、两类拒绝、WDS Start 和最后一条预算报告。

```sh
llvm=/opt/homebrew/opt/llvm@22/bin
lib=.local/apple/dsc-20H392/System/Library/Frameworks/CoreTelephony.framework/Support/libCommCenterMCommandDrivers.dylib
"$llvm/llvm-dwarfdump" --uuid "$lib"
"$llvm/llvm-objdump" --arch-name=arm64 --demangle -d \
  --start-address=0x1f19ccc60 --stop-address=0x1f19ccdb4 "$lib"
"$llvm/llvm-objdump" --arch-name=arm64 --demangle -d \
  --start-address=0x1f19dfc4c --stop-address=0x1f19dfe40 "$lib"
"$llvm/llvm-objdump" --arch-name=arm64 --demangle -d \
  --start-address=0x1f1a97f14 --stop-address=0x1f1a98228 "$lib"
awk '
  /COACHZ qmi: (host|to-host) .* svc 0x1 / && / msg 0x(20|21|2d|5566|5568|5569) / { print }
  /X9 QMI AT: reject service 0x1 msg 0x556[68] / { print }
  /X9 WDS: Start msg/ { print }
  /COACHZ qmi: .* held back by the budget/ { budget = $0 }
  END { if (budget) print budget }
' "$HOME/448.txt"
```

本轮是静态源码与已有捕获复核。基线 `bb5c8557` 加集成工作树已有补丁，两套主机回归共 111 项通过。桥行为、镜像和设备状态未改动。
