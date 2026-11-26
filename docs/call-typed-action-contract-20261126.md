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
