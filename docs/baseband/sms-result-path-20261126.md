# SMS CMGL 失败结果至 MIPC：静态路径边界（2026-11-26）

基线 `12be8340`。仅使用固定 modem 镜像及既有 nanoMIPS 解码器和符号表，未运行 modem 或访问设备。延续 [结果转换复核](sms-at-result-20261126.md)。

## 结果

静态证据分别建立了：CMGL 失败 handler 将转换结果写入共享槽；read 状态机在满足终止条件时传递该结构；SMS MIPC read 确认 packer 从输入结构同一偏移 +8 读取结果并经转换器发送。**尚缺 ILM `0xd9f3` 的分发表与指针身份闭合**，不能将前后两段无条件接成同一运行时链，更不能认定历史 `0608` 必经此路径。

- `l5usms_at_cnf_fail_cmgl` 两个分支 `0x91408f02..0x91408f18` 和 `0x91408f1c..0x91408f50`：若局部结构 `+8` 结果为零则写入 helper 返回值，再将 `s0+0xc` 指针分别传入 `l5usms_read_next_step`、`l5usms_store_next_step`。
- `l5usms_read_next_step` 的 `s0` 为该参数；终止分支 `0x91408d80..0x91408d8e` 将 `s0` 作 `a2`、消息号 `0xd9f3` 作 `a1` 交给 `l5usms_send_msg`。中间有异步阶段，不保证每次立即终止。存储状态机对应 `0x91408ed8..0x91408ee6` 发消息号 `0xda00`。
- `mipc_sms_read_cnf_v1` `0x91367c48..0x91367c54` 和 v2 `0x91367e66..0x91367e72` 从 `a0+8` 指向结构的 `+8` 取字，调用 `get_mipc_sms_result_code`，再交 `mipc_sms_send_mipc_cnf`。两者入口均存在独立条件分支，不能假设每个输入都到此。
- `get_mipc_sms_result_code` 对 `0x210000` 走 `0x913669ae..0x913669b6`、再到 `0x91366a4e` 返回原 `a0`；`mipc_sms_send_mipc_cnf` 在 TLV 0 不存在时将该值写成 4 字节 TLV 0，在 TLV 已存在时不覆盖。该静态行为本身不证明输出 wire ID 或真实响应路径。

## 后续证据需求

定位 ILM `0xd9f3` 的分发表及入参传递，证明其调用的 read CNF 版本、结构同一性和外层生成的 wire 消息 ID。即使全部静态闭合，历史 `0608=0x210000` 的实际来源仍需真实有界 trace；不可合成空列表、成功响应或持久化结论。

## 重现与验证

主 checkout 中，以 `x9/out/call-ios-bridge/tools/md-dis .local/x9/modem/parts/modem_a.img START END` 解码下列半开 VA 区间；符号来自既有 `x9/out/call-ios-bridge/md-symbols.json`：

| 符号 | START | END |
|---|---:|---:|
| l5usms_at_cnf_fail_cmgl | 0x91408eec | 0x91408f70 |
| l5usms_read_next_step | 0x91408c60 | 0x91408dc2 |
| l5usms_store_next_step | 0x91408dc2 | 0x91408eec |
| mipc_sms_read_cnf_v1 | 0x91367a2a | 0x91367c5c |
| mipc_sms_read_cnf_v2 | 0x91367d4c | 0x91367e9a |
| get_mipc_sms_result_code | 0x913669a4 | 0x91366a58 |
| mipc_sms_send_mipc_cnf | 0x91366ca2 | 0x91366ce6 |

各段解码 exit 0，输出位于本工作树 gitignored `x9/out/x9-sms-storage-chain/`。源码仅修正了旧注释的过度归因；无行为变化、构建、CI 或设备操作。
