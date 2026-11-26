# IOSurface SharedEvent：离线合同复核（2026-11-26）

基线 `bc6a83d615014f39b6cc3eab80839466a5fb2bab`。只读取现有 iOS 20H392 IOSurface.framework host 反汇编 `/tmp/iosurface.dis` 和仓库源码；没有设备操作。地址为对应原始映像虚拟地址，需使用同一版本的静态输入复现。

## 已确认

- `-[IOSurfaceSharedEvent init]` 的 `0x1a3988180..8198` 调用 selector 36，返回非零时走释放并返回 nil；成功后消费两个 scalar，再调用 selector 38。此事实仅说明该 initializer 的局部失败行为，不证明 Metal 或 AegirPoster 上层会安全降级。
- `-[IOSurfaceSharedEventListener initWithDispatchQueue:]` `0x1a3982f10..2f38` 单独建立 notification port 对象、绑定队列后，将该对象传给 `_IOSurfaceSharedEventAddEventListener`。因此 listener 注册不依赖 selector 36 的事件创建；只让 36 返回失败不能推断能够阻止 selector 40 的执行。
- `_IOSurfaceSharedEventAddEventListener` `0x1a39829f8..2a34` 从传入对象取得非零端口名称后调用 selector 40；`x2` 为该名称，`x3` 指向以回调与 context 初始化的 async reference，`w4=3`。这是异步通知注册参数，不应写成已证实的 event ID + 普通 struct 输入。端口名为零的 cold 分支位于调用 selector 40 之前，不能解释已记录 selector 40 之后的异常。
- selector 40 调用返回值原样返回 listener initializer：零时返回对象，非零时走错误处理后返回 nil（`0x1a3982f3c..2f54`）。上层对 nil 的处理仍未知。
- `SweetSurface.cpp` 的未匹配 selector 默认清零输出并返回成功（大输出缓冲例外）。该逻辑没有建立通知端口注册、事件生命周期或跨进程通知，因此返回成功不代表支持 SharedEvent。

## 推断与未决

`~/430.txt` 中首次 selector 40 记录紧邻 AegirPoster 的 SIGABRT，加之 host 栈位于 ObjC 异常终止路径，支持优先调查注册及后续动作；但没有异常名称、完整抛出栈或监听回调证据，无法证明 40 直接触发异常，也不能证明无条件返回错误或实现某部分 selector 会止崩。`sweet_surf_log_once` 使后续同 selector 不再打印，首次记录不能作为每次崩溃前最后一次操作。

正确实现至少须验证 IOKit 异步 reference/notification-port 注册和 teardown、Mach 权限及消息格式、事件身份和共享、signal/wait/并发与进程退出；不可直接在内核调用用户态 callback 指针。下一步追踪 `_MTLSharedEventListener` 和 AegirPoster 的 nil/异常处理、通知 payload 与发送路径，再选择完整实现或经证明的安全降级。此前不提供伪成功或声称 device/runtime 验收。

## 复核命令

```
sed -n '2427,2474p;2754,2810p;8448,8517p;16695,16722p' /tmp/iosurface.dis
sed -n '1513,1628p' x9/kernel/src/iokit/Kernel/arm/SweetSurface.cpp
```

上述命令 exit 0；`/tmp/iosurface.dis` 为既有可丢失 host scratch，文件行号仅适用于该次生成。未编译、未构建候选、未进行设备测试。

## 有界观察候选（2026-11-26）

未能证明 consumer 对 nil 的处理或完整异步通知及跨进程 event 合同，因此没有更改 selector 返回值，也没有实现事件或声称止崩。默认关闭的 `x9_surf_event_diag=1` 仅记录 selector 36–41、45 的至多 64 条调用：AegirPoster 的前 48 次、其余调用方合计前 16 次：进程名、selector、是否存在 async wake port、async reference 数量、scalar 输入/输出容量和 struct 输入/输出大小；不记录或解引用 reference、端口、scalar 值或用户指针。计数有并发原子保护，其余进程不消耗 AegirPoster 的专用额度；不能推断缺失调用或返回值，更不能从调用顺序证明异常因果。没有该参数或其值不是 1 时不产生新增日志；原有默认分支语义不变。

本地 XNU `IOUserClient.h` 定义独立的 `asyncWakePort`、`asyncReference`、`asyncReferenceCount`；`IOUserClient.cpp` 的异步入口将 wake port 和 reference 填入参数。它们不是证明 IOSurface 事件消息内容或生命周期的证据。现有 framebuffer `setAsyncReference64`/`sendAsyncResult64WithOptions` 仅示例 IOKit 发送机制，不能据此推导 IOSurface 的 payload、引用保活和 teardown。NativeBridge 的 `NBSharedEvent` 使用自有进程内实现，跨进程 `newSharedEventHandle` 返回 nil，也不能替代原生 SharedEvent 合同。

Host 单对象构建：通过 `x9/build/build.py` 的 `Builder.compile_one` 编译 `SweetSurface.o`，exit 0，421344 B，12 warnings；日志在 `x9/out/shared-event-diag-host-20261126/build.log`（gitignored）。这是编译检查，非设备、功能或崩溃修复验证。单对象结果对应初始诊断提交。整包候选及验证边界见 NOTES §6.16；设备启动及日志采集需另行授权。
