# Typed call action：身份与动作合同（离线汇总）

本节是既有 Apple 原件研究的合同整理，不是新运行证据或已实现 provider。更完整的原地址、资格化范围及限制见 [call-ios-bridge.md](call-ios-bridge.md) 中「Call driver 的 incoming record 与强 Model owner」和「真实 per-personality 路由与强 owner getter」。

## 已观察到的链路

| 对象 / 边 | 既有原件观察 | 不能推导 |
| --- | --- | --- |
| incoming record → driver map | driver 以 record 的 PersonalityID 查找强 Model pair；存在的 Model 接收原 incoming record pair | 本次 action 的 CallInfo/UUID 与该 record 的关联 |
| Model record owner | 已资格的 concrete Model 强持有 `+70/+78` record pair；更新先 retain 新 control、替换 pair、再 release 旧 control | 动作队列同步、revision、native epoch 或已退休回调 |
| driver dial → Model action | driver 保留输入 Model pair，并将新 Call pair 与本次 CallInfo/config/号码传给该 Model 的 `+118` | 该 Model 必然与独立 Client resolver 产生原子身份快照 |
| Manager → Client | Client getter 返回强 Client pair；构造与后续 getter 沿用 queue 参数 | 同一个 queue 参数就是动作与 record 更新之间的 barrier；getter 也返回 record |
| Call / broker | 既有研究中 Call 持有 UUID/clientId，但不 pin 强 Client/record；broker response 匹配 transaction | transaction、clientId 或 Call 指针等于跨生命周期 native identity |

限定：driver map 新建分支的 factory `+48` 目标及下游 ctor 未资格；不能因某 concrete Model 的 vtable 已资格就断定 map 中每个对象均为该类型。updater 的上游 dispatch caller 和实际 queue interleaving 未资格。

## 动作接入前必须闭合的合同

1. 对**同一实际动作**证明强 record 与强 Client 的捕获来源、具体对象类型和调用队列；证明它们对应同一 personality，而非分别查询后假定未变。
2. 证明该动作的 CallInfo/UUID 与上述捕获关联，并定义 record replacement、rebind 和 Client release 时旧动作及晚回调如何失效。record pair 的 retain/release 顺序本身不是失效屏障。
3. 为 native epoch/ID 与 QMUX allocation generation 保留独立域，证明进入、变更、退休及 callback 检查时点；不得以 UUID、clientId、transaction 或 VOICE44 success 代替。
4. 分别验证 action、native 请求/响应、真实状态通知和 typed terminal delegate；本地 enqueue、modem CNF、CallList 或注册均不是对端接通、音频或 UI 验收。

当前 `VOICE09`、受限 `VOICE20` 和 `CallList46` 已在源码发布/准入；它们不闭合上述 typed owner、完整生产 profile 或双向音频。不要据此新增 hook、猜测 ABI、合成状态或调用回执。

## 下一条有界研究

沿已资格的 driver incoming-record `+1e0` 入口向上找实际 dispatch caller 与 queue 边，再沿同一对象追 Model `+118` 动作发生时 record 与独立 Client 的捕获。优先证明可否建立同动作关联；找不到 barrier 时记录缺失，不把同 queue 参数当作证明。复用 `x9/out/call-action-owner/` 的既有原件及 `x9/tools/call_context_evidence.py`；任何新采样须使用新输出目录并单独记录命令和结果。

## 可复用的同 controller 切入点与缺失的原子边（2026-11-26 离线复核）

这次仅复核主 checkout 已保存的原件切片，不是新采样或新 ABI 资格。以下是同一个 **manager map lookup 路径**上的可验证切入点，不是建议直接 hook：

- `1f2e61a3c..6c` 的 Client getter block 以 manager `+68` 树及传入 PersonalityID 查 node，retain node `+38/+40` 的 controller pair；`1f2e61a74..84` 对此 controller 调 virtual `+60`，`1f2e68b38..58` 只从该 controller `+d8/+e0` 复制并 retain Client pair。证据：主 checkout `x9/out/call-context-producer/qualified-client/{manager-strong-client-getter-block,controller-strong-client-getter}.original.asm` 及 `controller-client-ref-vtable-slot.original-data.bin`。
- 同一个 manager added 已存在分支 `1f2e520cc..108` 用同一个 `manager+68` map 和同一 lookup `1f2e66404`、同一 node `+38/+40` retain controller；`1f2e528d8..8fc` 调 controller virtual `+38` 取 Model，再调用 Model virtual `+30` 更新 record。`1f2e421c8..1e8` 复制并 retain controller `+a0/+a8` Model pair；`1f2e77d7c..9c` 复制并 retain Model `+28/+30` record pair。证据：`x9/out/call-context-producer/qualified/{manager-personality-added,controller-model-getter,model-personality-update-getters}.original.asm` 及该目录 `original-producer-proof.json` 的 vtable/export 资格。
- **推断，不是现成 caller 合同**：若后续能在同一个已证 controller、同一同步范围内安全捕获上述 Client pair 和 Model→record pair，可构建候选的强所有权快照；原 getter block 实际只返回 Client，没有同时执行 Model/record getter。map key 一致也不证明不同时刻的 controller、record、Client 一致。manager 的 `isPresent` 发生于 dispatch 之前；tracker 的独立 child queue 和 record 更新路径使该检查不是此复合快照的 barrier。
- 下一步应资格化 getter block 的实际 queue 临界范围和 added/enabled 更新与该 queue 的交错，并证明 CallInfo 本次 slot→PersonalityID 到该 block 的唯一强关联；同时验证具体 controller/model 动态类型及 virtual slots。只有证实一致的串行化和 rebind/晚回调退休规则后，才能设计 capture。若 queue 或对象类型不能资格化，应继续 fail closed，而不是由 clientId、controller counter、PBI 或 getter 成功合成 epoch。

本次未运行 reader、重新采样私有原件或设备操作；未写 typed provider、调用原 ABI、构建或声称语音验收。
