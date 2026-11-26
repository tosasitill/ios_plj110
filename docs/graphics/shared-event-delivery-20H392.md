# Stock IOSurface SharedEvent 通知路径：20H392 静态切片

固定输入为 `.local/x9/usb/native-20H392/kernelcache.macho`，原版 IOSurface 336.60.1；地址均为未滑动 VA。以下是二进制控制流与调用约定观察，不是 X9 实现或设备证据。与 [dispatch 合同](shared-event-native-kernel-20H392.md) 一起阅读。

## 参数与发送

Selector 39 的 wrapper `0xfffffff005a5d8f0` 取 5 个 scalar，依次为 event port、listener 标识、event 对象、block 标识、目标值；后四项与匹配版 IOSurface.framework `_IOSurfaceSharedEventNotifyEventListener` 的用户态构造一致。wrapper 和 `0xfffffff005a601dc..02cc` 传递后三项至 `0xfffffff005a453a4`；此后 `0xfffffff005a45424` 和 `0xfffffff005a459e0` 将值、event、block 分别存到 notifier 的 `+0x10/+0x18/+0x20`。这些是用户空间不透明标识，不是允许内核解引用的指针。

`0xfffffff005a444d4..451c` 构造 4 个 64-bit async 参数 `[block, event, target_value, target_value >> 32]`，调用经 GOT 解析的 `IOUserClient::sendAsyncResult64(reference, 0, args, 4)`。匹配的用户态 callback `__ioSurfaceEventNotificationCallback2` 读取前三项并在用户态调用 block。第四项的用途尚未独立证明，不能仅按猜测删减。XNU `sendAsyncResult64` 负责封装 Mach 消息；内核并不调用传入的用户态函数指针。该 stock 发送点未观察到返回结果检查，不能把排队视为用户回调已执行。

## 生命周期及队列的已知边界

- selector 40 创建 listener 对象；`0xfffffff005a447f8..4830` 从 async reference 拿 wake port 和两个引用值，调用 `setAsyncReference64` 保存到对象内 `+0x38`。`0xfffffff005a4484c..4864` 在非空 reference 时调用 `releaseAsyncReference64`。要实现此路径，必须让成功的 MIG 调用把该 send right 的所有权转给 listener 对象，而非按当前 X9 丢弃策略立即释放；错误路径仍由 MIG 清理。
- 通知对象构造 `0xfffffff005a459e0..5a98` 存 listener 引用并经其 vtable `+0x20` 保留，故不能仅保留裸的用户 callback/port 值。具体任务退出、listener 移除及未完成通知的完整跨对象释放顺序尚待逐条验证。
- `0xfffffff005a453a4` 先比较 event 当前值与目标值；不足时进入 `0xfffffff005a4532c`，在锁下重新比较，再登记 pending 或转入发送队列。这一二次比较是防止 signal/register 竞争丢失的关键，不能以先无锁检查、后插入替代。
- `0xfffffff005a44250..42a4` 分配 lock 与 `thread_call_allocate`；`0xfffffff005a44458..44c0` 在锁下链接并保留队列项、按调度标志调用 `thread_call_enter`；`0xfffffff005a442bc..43a4` 分批取链、在解锁后发送并释放，处理新入队后才清标志；`0xfffffff005a44400..4430` 对空队列对象释放 thread call 与 lock。通过 GOT 符号确认实际调用；这不证明进程退出时所有 pending 条目的撤销策略。

## 未闭合事项 / 不能实现的原因

要从局部协议升级为可用实现，还须固定 `0xfffffff005a44d80` 的 event/reference 关联和映射生命周期、`0xfffffff005a6005c` signal 与 `0xfffffff005a4532c` 锁顺序/内存可见性、`0xfffffff005a60448` selector 41 移除后对 pending listener/notifier 的影响、`0xfffffff005a600e4` 跨任务 Mach right 与同一 event 身份的解析、客户端 close/task death 和并发发送的最终所有权。不能只实现计数器和 callback 消息便报告完整 SharedEvent；也不能假造值或成功通知。

## 可复现静态检查

在仓库根目录运行：

```sh
python3 -I x9/tools/tests/inspect_shared_event_delivery.py
```

该脚本读取固定原版 kernelcache，使用 `x9/usb/prepare_native.py:MachO`、Python capstone 检查指定指令及 GOT 符号，输出地址和 PASS；是静态 ABI 断言，不运行内核或模拟收发。2026-11-26 在分支 `feat/shared-event-delivery` 基线 `16cc7c53` 执行 exit 0，输出 `x9/out/shared-delivery-20261126/check.log`（gitignored）。对 stock 代码范围的可重查切片可用既有 `x9/out/native-event-abi-20261126/inspect.py <VA> <hex-length>`；关键入口分别为 `fffffff005a5d8f0`、`fffffff005a601c0`、`fffffff005a453a4`、`fffffff005a459e0`、`fffffff005a444d4`、`fffffff005a447f8`、`fffffff005a4484c`、`fffffff005a442bc`、`fffffff005a44400`。

本次未修改驱动、构建候选、操作设备或验证收发。上面脚本需要现有 capstone；不属于构建依赖。
