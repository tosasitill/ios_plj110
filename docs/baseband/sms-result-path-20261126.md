# SMS 存储失败至 MIPC：静态路径与实测边界（2026-11-26）

首轮基线 `12be8340`，本次补证基线 `ac6c45a4`。仅使用固定 modem 镜像、既有 nanoMIPS 解码器和保留的 Android/iOS 日志，未运行 modem 或访问设备。延续 [结果转换复核](sms-at-result-20261126.md)。

## 结果

`0607` 的 v1 确认路径已静态闭合。`CPMS` 和 `CMGL` 的失败确认均可产生 `0608=0x210000`；该值不是空列表。没有历史运行 trace 能区分这两个入口。

## v1 确认链

1. `mipc_sms_read_req_v1` 在 `0x9136780a` 选输出 `0608`，在 `0x91367890` 取 `mipc_sms_read_cnf_v1`，以请求号 `0xd9f2` 调用 `l5mipc_ilm_send`。后者在 `0x9133a26a` 写 context `+0x48` 回调、在 `0x9133a288..0x9133a28a` 写请求 local-param `+4` 的 context ID。`l5usms_read_req` 在 `0x91409e6a..0x91409e6e` 将 ID 复制到确认结构 `+4`、初始化结果槽 `+8`。
2. `l5usms_at_cnf_fail_cmgl` 的 read 子分支 `0x91408f02..0x91408f18` 从 AT 确认结构 `+6` 取半字，经 `l5usms_get_l5_res_from_at` 转换后写入原值为零的结果槽，再把确认结构交给 `l5usms_read_next_step`。`l5usms_at_cnf_fail_cpms` 的 `0x914094e8..0x914094f4` 同样写该槽；操作 8 的表项指向 `0x91409558`，以 step 5 把同一个结构送进 read 状态机。store 子分支另发 `0xda00`，不属于本链。
3. read 终止分支 `0x91408d80..0x91408d8e` 把原结构作为 `0xd9f3` 的 local-param 交给 `l5usms_send_msg`。该函数在 `0x9140616c..0x91406172` 原样传递该指针。中间状态可能等待异步 AT 确认，并非每个分支都立即终止。
4. 真正的分派在 `l5io_main`：`0x903b6b90..0x903b6bb4` 从 `ILM+6` 读消息号，按 `0xd9f3 - 0xd5d8 = 0x41b` 查 16 位表 `0x9298e792`，表项 `0x9298efc8` 为 `00 00`，落入 `0x903b6bb8`。此处以 `ILM+8` 指向结构的 `+4` 找 context，`0x903b6d24..0x903b6d2c` 把**原 ILM**交给该 context 的 `+0x48` 回调。
5. v1 packer 从 `ILM+8` 指向的结构 `+8` 读取结果（`0x91367a2e`、`0x91367c4a`）。`get_mipc_sms_result_code` 保留 `0x210000`，`mipc_sms_send_mipc_cnf` 在响应尚无 TLV 0 时以 4 字节写入此结果。v2 虽沿用 `0xd9f2` 和结果偏移，但选择独立回调和 wire ID `061a`（`0x91367ca0`），不能用来解释 v1 `0608`。

## 双卡样本与下一步

- `441.txt:18159`、`:18162` 两条 WMS List 经 X9 提交 `0607`，`storage=0 flag=1 103=1`；`:18178`、`:18180` 为同 SIM 的 `0608` 失败，均带 `103/u16=0`。MD 的 `0x913677a8` 把 wire storage 0 映射到内部 1，`0x9137d426` 的名字表首项为 `SM`，不是 `ME`。如果需要切换到该存储，固件先走 `CPMS`；不能因 count 0 更换存储或返回成功。
- `443.txt:21411` 的 SIM1 `0x21014b` 和 `443.txt:21837` 的 SIM2 `0x210004` 是 `0606` 失败。`l5usms_at_cnf_fail_cmgs` 也有从 AT 确认 `+6` 走同一转换器的路径；仅凭低位 `0x14b`、`0x04` 无法确认历史 AT 错误域或原因。`443` 和 `444` 捕获的成功 `0606` 只证基带受理；发送没有置 `TP-SRR`，没有入站 `8601` 或状态报告 `8603` 的验收样本。
- Android `x9/out/android-calls-20261115-183941/radio-baseline.txt:2341` 的 SIM2 存储可用通知发独立 `0601 {108/u8=1}`，相同事务的 `0602` 返回成功，之后同卡 `:4119` 收到 `8601`。RIL `RmmGsmSmsRequestHandler::handleReportSmsMemStatus` 在 `0x331378..0x33139c` 以请求所属 slot 构造这个字节，不是 `:1055` 的 host-ready `0601 {100=0, 105=1, 106=2}`。固定 MD 把前者经 `0x91366e88`、`0x914082fa` 路由至 `AT+EMEMS=1, %hhu`。X9 只实现 host-ready，不应把未测得的存储状态塞进去。当前差异不证明两者的入站因果关系，也未建立 iOS 的存储可用上报 owner。
- 先采现有 `x9_sms_diag=1` 同卡只读 `060b/060c` CPMS 查询和 `0607/0608` CMGR 数据，保留读标志。CPMS 查询成功仅证明查询成功，不证明生产列表中的 `CPMS` 切换成功；CMGR 固定诊断 index 1，不凭单个索引宣布收件箱为空。将数据与同 SIM 的原始 List 及新 `8601` 对照，再定位具体失败阶段。

## 重现与验证

主 checkout 中，以 `x9/out/call-ios-bridge/tools/md-dis .local/x9/modem/parts/modem_a.img START END` 解码下列半开 VA 区间；符号来自既有 `x9/out/call-ios-bridge/md-symbols.json`：

| 符号 | START | END |
|---|---:|---:|
| mipc_sms_read_req_v1 | 0x913677c6 | 0x913678c2 |
| l5mipc_ilm_send | 0x9133a24a | 0x9133a29a |
| l5usms_read_req | 0x91409e50 | 0x91409ee8 |
| l5usms_send_msg | 0x914060c8 | 0x91406196 |
| l5io_main | 0x903b6b5e | 0x903b6fba |
| l5usms_at_cnf_fail_cpms | 0x914094e0 | 0x91409562 |
| l5usms_at_cnf_fail_cmgl | 0x91408eec | 0x91408f70 |
| l5usms_read_next_step | 0x91408c60 | 0x91408dc2 |
| l5usms_store_next_step | 0x91408dc2 | 0x91408eec |
| mipc_sms_read_cnf_v1 | 0x91367a2a | 0x91367c5c |
| mipc_sms_read_cnf_v2 | 0x91367d4c | 0x91367e9a |
| get_mipc_sms_result_code | 0x913669a4 | 0x91366a58 |
| mipc_sms_send_mipc_cnf | 0x91366ca2 | 0x91366ce6 |

表项以 `od -An -t x1 -j "$((0x9298efc8-0x90000000+512))" -N 2 .local/x9/modem/parts/modem_a.img` 复核。首轮输出位于原工作树 gitignored `x9/out/x9-sms-storage-chain/`；本次只补文档，不改生产行为、不构建、不访问设备。
