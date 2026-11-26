# SMS AT 结果转换：离线反汇编（2026-11-26）

## 结论与范围

固定 `modem_a.img` 的 `l5usms_get_l5_res_from_at` 独立反汇编确认：默认分支返回 `0x210000 | input`；`l5usms_at_cnf_fail_cmgl` 从一个结构偏移 +6 读取 16 位值并调用此 helper，且仅在结果槽原值为零时写入转换结果。故此失败路径输入为零时可形成 `0x210000`。符号名和代码支持它是 CMGL AT 确认失败路径的一种转换结果；但**未证明历史 0x608 实际必经此路径**、未判定产生输入零的根因，也未证明收件箱为空或 SIM storage 可用。

## 静态证据

- 基线源码 `15f75087`；输入 `.local/x9/modem/parts/modem_a.img`，SHA-256 `575f927298ea96a399d686b10597dcdca52a64ebd75a1167d6fed804165e829b`，与既有 `x9/out/call-ios-bridge/md-call-evidence.json` 一致。符号表为既有 `x9/out/call-ios-bridge/md-symbols.json`；其地址是该映像上的静态分析结果，不是运行时 trace。
- helper `0x914062de..0x9140635c`：输入保存在 `s0`；`0x136 -> 0x3c0001`、`0x142 -> 0x200013`、`0x141 -> 0x200001`；其他值流向 `0x91406354 LUI a0, %hi(2162688)` 和 `0x91406358 OR s0, a0`，即默认结果按位 OR `0x210000`。符号边界前后的语义不能代替调用链证明。
- `l5usms_at_cnf_fail_cmgl` `0x91408eec..0x91408f70`：结构选择条件为 byte +8 值 7 或 8；分别在 `0x91408f1c`/`0x91408f02` 从 context +8 指向结构的 +6 读取无符号半字，调用 helper；若状态 +8 为零，分别在 `0x91408f26`/`0x91408f0c` 写入结果。`l5usms_at_cnf_succ_cmgl` 从 `0x91408f70` 起是独立符号。此处并未把每个结构字段的未证语义写成事实。
- 输入为 16 位时 `0x210000 | input == 0x210000` 当且仅当 input 为 0；这只约束经该 helper 的路径，不能倒推所有可能生成同一结果码的路径。

## 重现

以下在主 checkout 执行，仅离线读取，不运行 modem：

```sh
shasum -a 256 .local/x9/modem/parts/modem_a.img
x9/out/call-ios-bridge/tools/md-dis .local/x9/modem/parts/modem_a.img 0x914062de 0x9140635c
x9/out/call-ios-bridge/tools/md-dis .local/x9/modem/parts/modem_a.img 0x91408eec 0x91408f70
x9/out/call-ios-bridge/tools/md-dis .local/x9/modem/parts/modem_a.img 0x91408f70 0x91408fba
```

以上命令 exit 0；现有 decoder 的 VA→文件偏移为 `VA - 0x90000000 + 512`。本轮反汇编输出保存在主 checkout 的 gitignored `x9/out/x9-sms-storage-followup/l5usms-result.asm`。未修改驱动、未构建、无 CI 或设备操作；未进行运行时或持久化验收。
