# 蜂窝数据上网：当前证据与逆向续接

> 更新：2026-11-11。目标设备 PLJ110 / MT6993，iOS 20H392。
> **尚未联网成功。ACT 成功、接口有流量统计、DRB 消费成功，都不等于空口交付或收到回包。**
> 本文用于短上下文续接；历史细节按 `NOTES.md` 中的交接名称检索，不要整份输出。

## 1. 结论先看这里

- SIM、注册、拨号控制面已推进；真实 ACT/GET/EIF 匹配后能发布双栈 PDP3。
- AP 产生 DNS/ICMPv6 等上行；DPMAIF 的 TX 描述符能够全部被硬件消费。
- **最近专门探针测试 218：TX 303/303、RX 0；较新的整机运行 220：TX 241/241、RX 0。**
- 218 在 LTE 空闲阶段继续送包，三种 DRB 形态都没有触发 RRC 连接。重点转向 MD 内部 queue → netif/session → TFT/EPSB → RATDM 的 pending/释放边界；**尚未取得现场命中的分支。**
- 不能完全排除我们在 iOS 端的适配缺陷。尤其错误的 ACT/映射配置也可能使包在 MD 内部停住；这与“iOS IP 栈已经收到回包后丢弃”是不同问题。
- IMS 启用有确定缺口，但**未证明 IMS 缺失导致普通 Internet RX0**。名称查询已有双卡真实成功日志，也不是联网证明。

## 2. 设置显示二百多 KB，为什么仍可能 RX0？

**用户观察**：iOS 设置里有二百多 KB。尚缺具体页面、方向、统计周期及与日志的同次启动关联，不能认定它是本轮下载流量。

源码中两条统计路径明确不同：

| 计数 | 增加位置 | 证明到哪一层 |
|---|---|---|
| `pdp_ip` 输出字节 | `coachz_bb_pdp.cpp`：`coachz_modem_data_send_epoch` 返回 0 后调用 `ifnet_stat_increment_out(ifp, 1, n, 0)` | AP 有效包被有界 TX mailbox 接受；不等完成、空口发送或对端 ACK |
| WDS TX 字节 | `X9DPMAIF.inc::x9d_tx_poll`：PAYLOAD 描述符退休后 `r.stats.tx_bytes += c.size` | DPMAIF 硬件消费完成；仍不等 MD 无线提交 |
| WDS TX/RX 字节输出 | `X9WDS.inc::x9w_statistics`：TLV `19/1a`，last-call `1b/1c` | 各自 ledger 的方向计数，不是连通性探测 |
| `pdp_ip` 输入 / DPMAIF RX delivered | `bb_pdp_receive_counted → ifnet_input`，成功后 RX ledger 加包/字节 | 包进入 XNU 接口输入；依然不等 socket 收到或应用成功 |

因此，**总量可能来自上行及重试，也可能是历史累计**。本轮没有追到 Settings 最终取数对象，不声称已证明 UI 的二百多 KB 就来自上述某一个计数。

220 的同次日志交叉证据：

- LF21320 / 121.586s：`pdp_ip0` 对应 unit0 `tx 241 rx 0 drop 0`。
- LF21335 / 121.587s：DPMAIF `TX submitted 241 completed 241 RX delivered 0 dropped 0 queued 0 rejected 0`。
- LF11971/11972、18278/18279 的两次 `rx-none dl_sram`，PIT0/1 producer 字段 `+a4/+c4` 都为 0。它是定点快照，不代表连续采样，也不排除读取/配置本身的适配错误。

**判断**：现有证据不支持“回包已进入 `pdp_ip`，然后被 iOS IP 栈统一丢掉”；更支持回包未到我们观测的 DPMAIF RX 入口。若能确认同一轮有**接收字节增量**，就应返回最早不确定边界，复核计数来源、队列/寄存器读取与 RX→ifnet→socket，不用旧 RX0 结论压过新证据。

最小区分实验（尚未执行）：记录页面与当前值，关闭 Wi-Fi 后只做一次定点网络请求，比较 TX/RX 分向增量，并采集同一时段的新日志。**无需先清统计、重启或重试旧镜像。**

## 3. 已有实测，不把目录号当设备测试号

| 日志 | 实跑 / 目的 | 最后一条 DPMAIF 统计 | 已验证及边界 |
|---|---|---|---|
| 214 | cell157 / MTU、EIF 更新、生命周期 | 95.964s：242/242，RX0 | host MTU1450 保持；EIF5 后 GET 成功；无联网 |
| 215 | cell160 / IPv6 诊断 | 73.011s：138/138，RX0 | MD 上报始终为链路本地地址，没有观测到全球前缀 |
| 216 | cell161 / RRC、默认数据卡配置 | 50.880s：72/72，RX0 | 两卡名称真实 result0；首次 ACT 后约7.7s NR→LTE |
| 217 | cell162 / BAT 消费 | 61.093s：146/146，RX0 | 两次 BAT 提示确已消费；LTE 空闲中送包不触发连接 |
| 218 | cell163 `c91ffc0` / A/B/C 探针、276 | 144.679s：303/303，RX0 | flags / channel-1 / base 均失败，累计70/90/143包 |
| 220 | 较新的277整机运行，无A/B/C探针 | 121.587s：241/241，RX0 | 两次 ACT0；两卡名称 result0；仍无下行 |

218 的两个 ACT0：30.035s / LF8693、59.947s / LF17960；后续 host-qualified 对应 ccmni1/2，families3。52.302s 转 LTE 空闲后，持续到约155s没有新的 SIM1 4409 连接通知。

220 的两个 ACT0：31.427s / LF8884、59.360s / LF17079；插件身份为 LF1413 的 `active plugin=X9GeneralMetal277 from verified allowlist`。

**`cell219-md-ul` 和 `cell220-md-act-ul` 是离线分析目录，不是新增219/220上机。后者主要解释218，不可自动套到真实220的MD现场状态。**

## 4. 已完成的接线与已经排除的方向

控制面主线：`CommCenter/QMI WDS → X9WDS → MIPC → 真实MD ACT/GET/EIF → DPMAIF bind → pdp_ip`。

- DPMAIF 已移到 `BOOT_WAITING_FOR_HS1` 启动，四 TXQ、同型号 BAT/PIT 配置与初始 sequence 已核对；早期首批 TX 停住的问题已越过。
- Internet ACT 已按同机 RIL 构造808B/PDP3/APN-type1/URSP，含真实 MOBILE_DATA 配置确认；旧 IF_ID 猜测及 app3 packet-route 不是已证实普通 PDN 前置。
- retry mode1 是重置 mutation，mode0 才查询建议；失败 CID 不授予 ownership，停止/迟到 token、同 PDN family lease 已修过。实际成功发布仍需 ACT/GET/EIF 身份匹配。
- BAT CODA 提示消费在217/218/220实际生效，但 RX0 不变，**不是单独充分修复**。
- 同机健康 Android 的 `SYS_CONNECTIVITY_STATISTICS` 也为0，旧“统计为0证明空口 TX0”作废。
- EIF cmd5 可由多条 MD 路径发出，不能单凭它认定收到 RA或等待 RA超时。216已观察它跟随 NR→LTE；切网原因及其与 RX0 的因果仍未知。
- `pd_dl+c4` 与实际 `dl_sram+c4` 不是同一 bank，不能把前者 `0x120` 当漏收288个 PIT。
- A/B/C 的三个 DRB 字段探针已经失败；没有新证据不重复、不把 cell163 默认轮换探针作为日常方案。

剩余完整性包括：独立 IPv6 PDN、完整 RA/SLAAC 验收、carrier/auth/PCO、完整 LRO、休眠恢复等；这些未完成项不能自动当本轮 RX0 根因。

## 5. MD 静态逆向现在到哪儿了

输入是 `.local/x9/modem/parts/modem_a.img`，md1rom 文件偏移512，VA `90000000..92e80000`，版本 `MOLY.NR17.R2.MP2.TC16.PR3.SP.V2.P77`。以下都是静态路径，不是218现场命中证明。

### ACT / 身份 / session

- AP入口：`mipc_data_act_call_req@91326c36 → L5UPDN → D2 ACT`。成功侧 `l5updn_act_data_call_cnf_handler@913d74d4` 发 EIF IFUP、`dac0`，再由 `mipc_data_act_call_cnf@913272d2` 转 AP 响应。**不是所有 ACT 都直接经过 DDM CGDATA。**
- 独立绑定支线：NIM `b3ce → DDM ad01 → CGDATA → afbd → L4BPDN → 74f6 → UPCM → 74df → IPC session`；dispatcher `7548` 也共用 IPC handler。
- **未闭合**：pending `b234` 入 NIMQ 的来源、ACT external-PDN callback 的实际 target、218 txn144 具体经过哪条绑定支线。`d2_msg_send6` 的 b234 分支只复制已有 IFID，不直接入 NIMQ。
- 完整 IFID=`generation*100+interface_index`；AP0125通常取 `%100`、012a保留 transition/generation。**host call、ccmni号、raw CID、IPC context、完整 IFID 不可互换。**
- IPC context=`pdn_low6|(SIM<<6)`。`ipc_find_session_by_netif@90104d8c` 要求 netif/session **对象state2且+8=0**，IPv4从netif+44、IPv6从+40找session；session+0c取PDN。业务state在+10，与对象state不同，业务6仍可 lookup。
- `ipc_on_pdn_bind_bot@90736664` 的0x1100有条件跳过业务state4；UPCM后续bind_state4也不直接证明普通UL TFT/EPSB映射健康。

### UL pending / 释放 / 无线提交

| 边界 | 已还原的条件 / 动作 |
|---|---|
| IPC允许门 `9011862c` | false时queue保留pending，true才调真正consumer；queue base2787bcf4/stride34，consumer在+2c |
| metadata分类 `901c1376` | word3 bits10:8 的u16相对表9260a7b4；case7是软件读真实IP头，不是“硬件错误必丢” |
| session缺失 | `901c2d78` / DHL4c500052，标meta bit27后release；未见218该现场记录 |
| UPCM暂停 | global27490629是N26位图，非零保留pending；discard274906c5由7518/7519置清，sender未闭合 |
| RATDM容量 | `9013ea14` 是任务队列剩余容量判据：flush已有工作、保留当前PIT；不是RRC连接判据 |
| 普通TFT | 每SIMctx=`290f8ba8+SIM*9d08`；ctx+84da+2*pdn查u16。N26 fallback后仍ffff→release reason5；0分支的完整所有权语义未定 |
| EPSB | ctx+79fc+bearer查u8索引，ff→NULL；对象位于ctx+664+index*28。bearer≥984 / NULL / +1c=0 / +1e=ffff均有release出口 |
| test-mode位图 | ctx+640 flags&16非零才检查27490630/34；不能当全部Internet dataallow门 |
| 无线下游 | 有效路径到`set_ratadp_ul_sdu_by_epsb → ratdm_ratadp_rcv_ul_sdu_5g_nas`，RAT3→LTE、5→NR；到此仍不等空口成功 |

AP020d实际是 `DATA_SET_DATA_ALLOW_REQ`；内部7524更新ctx+9b0d并通知b750，不直接写N26/test-mode位图。不能把MD内部6b30紧急呼叫通知、124a扫描节流等当AP可直接补发的MIPC命令。

## 6. 名称与 IMS：独立处理

- 名称：NAS44→MD0429/042a 已实现，216/217/218/220均有两卡 `result0(real MD)`。最终 Settings 新鲜缓存/UI仍须用户确认；“网络不可用”那一行读运营商名称，不是下载连通性测试。详见 [关于本机网络显示](about-network-20H392.md)。
- IMS：214真实 NAS33/TLV33=2（WithVoLTE）5次被error94拒绝；查询实现不能替代启用。尚无完整IMS修复。
- 同机RIL0a00配置是六字段，前两包含VoLTE/VoNR及ViLTE/ViNR组合；`03 03 00 00 01 01`不是六个布尔全部启用，不能原值照抄Apple枚举2。
- EXIMS1213稀疏字段会把省略值0继续下发；setter可能写NVRAM **LIDb58、record=SIM+1**。临时boot不保证回滚。0a02只是缓存，不是可靠后端状态/NV备份。
- NAS72↔EXIMS1211/0a0e字段及真实注册所有权仍未闭合；不能先假ACK、伪造注册或默认写NV，也不能据NAS72单独断言SIP一定在host/MD。

## 7. 续接入口与最小重放

主要源码：`x9/kernel/src/iokit/Kernel/arm/{X9WDS,X9WDSResult,X9DPMAIF,X9QMIAT,X9NetworkName}.inc`、`x9/kernel/src/bsd/dev/x9_mipc_bridge.inc`、`x9/baseband/qcom/baseband/coachz_bb_pdp.cpp`。

私有证据（不入git）：

- `.local/x9/work/cellular-summary-20261111/`：本次 `verify.py/verification.json`、scope/timeline；核对既有日志和13个路径，不读设备。
- `.local/x9/work/cell220-md-act-ul/{ap-act,ipc-bind,upcm,upcm-control}/`：最新ACT身份、绑定、TFT/EPSB及暂停证据。
- `.local/x9/work/cell219-md-ul/`：48个定点asm、跳表、caller候选与 `decode-ul.py/trace-calls.py`。
- `.local/x9/work/cell189-md-consumer-20261110/md-dis.py`：现成定点nanoMIPS反汇编入口。
- `.local/x9/work/ims214-{audit,native-continue}/`：IMS启用、配置与持久副作用证据。
- `.local/x9/android-dpmaif-20261111/`、`.local/x9/android-boot-20261111/`：健康Android驱动/DRB/RIL基准。

仓库根离线复核（已执行第一条，exit0；其余是此前已验证的续接命令，本轮未重复执行）：

```sh
python3 -I .local/x9/work/cellular-summary-20261111/verify.py
python3 .local/x9/work/cell220-md-act-ul/ap-act/collect-act.py
python3 .local/x9/work/cell220-md-act-ul/ipc-bind/export-evidence.py > .local/x9/work/cellular-summary-20261111/ipc-replay.json
python3 .local/x9/work/cell220-md-act-ul/upcm/collect.py > .local/x9/work/cellular-summary-20261111/upcm-replay.json
```

解码注意：日志按 `split(b'\n')` 保留LF行号，忽略末尾空项；孤立CR会使 `splitlines()` 漂移。nanoMIPS ALUIPC要先取page；压缩两操作数算术/逻辑输出是source,destination；BALC索引不含全部MOVE.BALC/BC，literal命中不等caller。

**下一窄动作**：先取得设置页面的统计方向与同次增量；静态主线则优先闭合 pending b234 / external-PDN callback → 同IFID/CID绑定，再追普通TFT/EPSB结果→LTE RATDM/RRC。若要现场读MD状态，必须先证明合法可读映射，**MD VA不可直接当AP物理地址**。当前没有针对RX0的新修复或新候选，不能重跑旧探针当新方案。

## 8. 短上下文 / daybreak

`daybreak` 未在本次检索的项目文档及选定私有文本日志中命中；没有工具报错全文，**未定位原因**。当前 `NOTES.md` 约1.59MB，一次完整输出会截断且引入大量历史矛盾；短文档和定点文件读取可以避免这一问题，但不能据此断言就是daybreak根因。本轮不删除其他窗口交接。

可直接用于新会话：

> 先读AGENTS.md、NOTES.md开头及docs/network/2026-11-11_cellular-data-report.md，不要cat完整NOTES。蜂窝未联网：218/cell163/276 TX303/303 RX0，三种DRB探针均失败；新整机220/277 TX241/241 RX0。用户设置看到二百多KB，先核对总量还是接收增量，不能把TX计数当回包。MD最新证据在cell220-md-act-ul，先闭合pending b234/external-PDN callback到同IFID/CID和IPv4/IPv6 session，再追普通TFT/EPSB→RATDM/RRC。对象state与业务state分开，test-mode不是普通dataallow。没有MD现场命中分支，不盲写MD VA/NV，不重复旧探针，不混276内核与277/278插件。IMS独立推进，未证明它造成RX0。

本轮仅离线复核及文档整理：无设备操作、运行代码修改、编译/boot/CI run/push，未解决RX0；完成的是证据汇总，不是联网验收。
