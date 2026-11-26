# ALS 连续性与时间域：离线复核（2026-11-26）

基线 `f239883cb2b1ec9ed0769add0bfa3c8b21675816`；本次只读源码和既有 `~/430.txt`，未访问设备。本文纠正从单条日志推断只有一个 ALS 事件的结论。

## 证据与结论

- `X9AmbientLight.inc` 的 `poll` 在每个新 sequence 均尝试 dispatch，但仅在 sequence 为 1 或 diagnostic 时打印 dispatch。因而 430 的一条 `dispatch seq=1 lux=824` **不能证明只有一次 dispatch 或只有一个样本**。
- 430 在 2.677 s 有 type 116 ENABLE 成功响应，2.782 s 有 `sample valid` 和首次 dispatch；随后可见周期性 type 0 cmd 10。心跳本身不是光线样本。日志无后续 `sample invalid/stale` 行；若定时器持续执行、日志完整且没有状态重置，则这与后续接收保持新鲜相容，但不能单独证明每次 dispatch、实际采样率或物理光照变化。
- `publish()` 以 `mach_absolute_time()` 记录接收时刻；`x9_als_copy_sample()` 的 5 秒限制只衡量接收年龄。wire 时间仅做正值和单调检查，未转换至 AP 时钟；不能证明测量年龄或在此基础上安全修改时效策略。
- worker 在 HID ingress 持有且 LIST 有 gain 时发 ENABLE，之后仅在请求 interval 与已 ACK 的 interval 不同时重新配置；不能从日志频率直接认定 iOS 没有请求连续 ALS。SCP producer 的实际 on-change/周期语义及 FIFO 排队年龄仍需独立证据。
- PS 极性没有新增能排除原报告疑点的绑定或遮挡边沿证据；`oneshot == 0` 仍为未验证假设，不作翻转。

## 下一判别点

已有 `X9ALSSequence`、`X9ALSLastLux`、`X9ALSDataValid` registry 属性在实际 dispatch/有效性变化时更新，可在后续经明确授权的设备观察中按同一时钟读取至少两次并记录场景，不把属性变化等同测量年龄；若需要采样率，还需同时获取真实 SCP timestamps、接收时间以及 FIFO/时钟映射，不能以合成输入代替。若缺日志完整性或 timer 存活证据，也不能将没有 stale 行当成连续性的证明。

## 复核方法

```sh
rg -n 'X9 ALS: dispatch|X9 ALS: sample|X9 ALS SCP:.*(ENABLE|control)' ~/430.txt
rg -n 'host_time|scp_time|MAX_AGE|records|rejected' \
  x9/kernel/src/iokit/Kernel/arm/X9ALS.h \
  x9/kernel/src/bsd/dev/sweet_x9_als.c \
  x9/kernel/src/bsd/dev/x9_als_scp.inc
```

源码另见 `x9/kernel/src/iokit/Kernel/arm/hid/X9AmbientLight.inc`。本次无驱动修改、编译、CI、设备写入或新设备采样；以上为静态审查与既有日志观察，不是 feature acceptance。
