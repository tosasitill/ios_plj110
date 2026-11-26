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

## Metal consumer / IOSurface 进一步静态边界（2026-11-26）

固定输入 `.local/apple/dsc-20H392/System/Library/Frameworks/Metal.framework/Metal`，`xcrun llvm-objdump --macho --disassemble --no-show-raw-insn` 输出到 gitignored `x9/out/shared-event-research-20261126/metal.dis`；IOSurface 输入仍为 `/tmp/iosurface.dis`。这些是离线静态观察，未证明设备进程执行到所述路径。

- `-[MTLSharedEventListenerInternal init]` `0x181b1cd24..cd88` 与 `initWithDispatchQueue:` `0x181b1cdd8..ce34` 分别建立通知端口、绑定队列并注册 listener；端口或注册失败释放并返回 nil。它们是 Metal listener 的独立注册，不能由 event 创建失败推断不会发生 selector 40。
- `-[MTLSharedEventListenerInternal _notifyEventPort:event:atValue:block:]` `0x181b1cef4..cf2c` 调用一个外部入口，非零结果进入 `raise:format:`。Metal 的 indirect-symbol 表依次为 `_IOSurfaceSharedEventAddEventListener`、`_IOSurfaceSharedEventNotifyEventListener`、`_IOSurfaceSharedEventRemoveEventListener`，与这三个 12-byte 间隔外部调用的顺序及参数流一致；故将 `0x186d85fac` 识别为 NotifyEventListener 是基于 stub 顺序和参数流的推断。IOSurface 的该函数 `0x1a39885d0..8670` 在 selector 39 前保存 block；selector 39 返回非零时移除 block 并返回错误。**故在该 consumer 路径下无条件拒绝 39 会使 Metal 显式抛异常，不是已证明的止崩修复**。现有默认成功则未建立发送机制，可能留下未触发的 block；无法据此估计泄漏或断定崩溃原因。
- `-[IOSurfaceSharedEvent signaledValue]` `0x1a3988518..8520` 解引用 initializer 保存的共享地址；initializer 36/38 的两个成功返回会消费输出。现有 fallback 清零输出并返回成功，若原生对象经此路径创建并读取该值，静态上存在零地址解引用风险；日志中的 SIGABRT 不是零地址解引用的直接证据，也未证明 AegirPoster 执行了这个 getter。
- `NBSharedEvent` 是 X9 自有 Metal frontend 的不同实现，其 `notifyListener` 经 Rust 的进程内 shared event 注册，`newSharedEventHandle` 返回 nil；原生 Metal listener 则仍可能被独立创建。因此不能把 Metal listener 的 selector 40 记录视为证明特定 event 是原生对象或 bridge 对象。

复核命令：

```sh
xcrun llvm-objdump --macho --disassemble --no-show-raw-insn \
  .local/apple/dsc-20H392/System/Library/Frameworks/Metal.framework/Metal \
  > x9/out/shared-event-research-20261126/metal.dis
rg -n 'MTLSharedEventListener|_MTLSharedEvent' x9/out/shared-event-research-20261126/metal.dis
sed -n '276565,276820p' x9/out/shared-event-research-20261126/metal.dis
xcrun llvm-objdump --macho --indirect-symbols \
  .local/apple/dsc-20H392/System/Library/Frameworks/Metal.framework/Metal | grep IOSurfaceSharedEvent
sed -n '8448,8517p;8675,8820p' /tmp/iosurface.dis
```

上述命令 exit 0；没有修改驱动、插件、root 或设备，没有构建新候选。下一步需取得实际异常名称/抛出点或已证明运行路径，及对 event 的真实所有权、Mach rights、并发、销毁与 callback payload 合同；此前不适合声称存在小范围可安全修复。

### 430 日志的区分证据

首次 AegirPoster SIGABRT 之前，实际 Metal factory 返回 `X9GeneralMetalDevice`，M2V prime 成功，而且启动日志显示 CSG slot steal 已开启；不能沿用旧候选“device 为 nil”的归因。相同日志内 MapsSpotlightInd 的终止例程相似，但较深的帧经过 `objc_exception_throw`、CoreFoundation/PlugInKit，而 AegirPoster 经 libdispatch/FrontBoardServices。仅凭共同终止帧不能将两者归为同一异常。两份首次栈以 430 的 slide 和现有原件索引经 `atos -arch arm64e -o <dylib> -l <unslid-base> <unslid-pc>` 离线符号化，节选存于主 checkout `x9/out/shared-event-log430-review-20261126/stack.txt`；不是异常抛出点、前置 selector 或消息协议的证据。
