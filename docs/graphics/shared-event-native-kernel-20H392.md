# Native IOSurface SharedEvent kernel dispatch, D22 / 20H392

Source: fixed read-only `.local/x9/usb/native-20H392/kernelcache.macho` and matching `inventory.json`, prelinked `com.apple.iokit.IOSurface` 336.60.1. This is static ABI evidence, not a stock kext port or device observation.

`0xfffffff005a60b84..bac` selects one of two 54-entry tables at `0xfffffff006d87678` and `0xfffffff006d87ee8` and tail-calls the stub at `0xfffffff005a674ec`. Its GOT target `0xfffffff0077b84b4` resolves to `IOUserClient2022::dispatchExternalMethod`. The 40-byte entry layout follows the local XNU `IOExternalMethodDispatch2022` declaration (64-bit action, four u32 validation values, u8 allowAsync + padding, pointer). Both tables agree on these rows; none of these rows sets a dispatch-table entitlement pointer.

| sel | scalar in/out | struct in/out | async allowed | stock wrapper VA suffix (prefix `0xfffffff005a`) |
| --- | --- | --- | --- | --- |
| 36 | 0 / 2 | 0 / 0 | no | `5d87c` |
| 37 | 2 / 0 | 0 / 0 | no | `5d8c8` |
| 38 | 1 / 3 | 0 / 0 | no | `5d8d8` |
| 39 | 5 / 0 | 0 / 0 | no | `5d8f0` |
| 40 | 0 / 0 | 0 / 0 | yes, required by wrapper | `5d904` |
| 41 | 1 / 0 | 0 / 0 | no | `5d91c` |
| 45 | 3 / 0 | 0 / 0 | no | `5d9e8` |

The selector 40 wrapper loads args+0x10 (asyncReference) and rejects null with `kIOReturnBadArgument` before entering the registration handler `0xfffffff005a60328`. The occupied-refcon branch at `0xfffffff005a61c3c` matches a duplicate and sets BadArgument. The normal handler calls into the listener object family and retains an object in a client-side collection. This is more than consuming a Mach reference. For selector 36, the handler `0xfffffff005a5ff9c` creates event and reference objects, maps the event's memory for that client, then exports a task port name. Selector 38 (`0xfffffff005a600e4`) resolves the input in the task, checks the reference class, derives the event, and writes three scalar outputs; not struct output. Notification selector 39 is synchronous IOKit registration of an asynchronous completion; it must not be confused with the async transport of listener selector 40.

X9 now gates selector 40 on a nonempty async reference/port and zero scalar/structure input/output (including no descriptors). The valid registration still follows the previous compatibility discard (consume send right, return success); this is not listener implementation. Error returns precede any manual release: MIG owns failed request cleanup. Next work must establish the lifetime of mapped signal memory, rights and observer objects together; no use of fabricated IDs, zero pointers or fake callbacks as full implementation.

Repro: from the repo root, `python3 -I x9/out/native-event-abi-20261126/inspect.py` lists the table rows, using `x9/usb/prepare_native.py:MachO`; `python3 -I x9/out/native-event-abi-20261126/inspect.py fffffff005a5d87c 188` reads the wrapper range, and `python3 -I x9/out/native-event-abi-20261126/inspect.py fffffff005a5ff9c 55c` reads bounded handlers. The private disassembly remains in ignored scratch.

## Shared counter map, capability, and wait

- Selector 36's first scalar is a task-local **Mach send-right name**, not the event array index. The audited stub at `0xfffffff005a6745c` resolves to `IOUserClient::copyPortNameForObjectInTask(task*,OSObject*,uint32_t*)`; the handler passes the retained reference object and copies the name into output 0. Output 1 is the caller-task virtual address of the event's 8-byte counter; the userspace getter loads the first qword at that address. Use separate handle and slot types in any X9 implementation.
- Selector 38 resolves its Mach name in the client task via `copyObjectForPortNameInTask` (stub `0xfffffff005a67450`), then `safeMetaCast` (`0xfffffff005a674d4`). The helper outputs are **0 = acquire-loaded signaled counter value**, **1 = caller-task address of the mapped read-only counter**, **2 = event trace ID**. Offset assignments are explicit at `0xfffffff005a60150`, `...60160` and the call at `...60180`; counter loads and trace field access are `...45308` and `...45300`. The importing userspace initializer copies 1 and 2 into its object; the creating initializer keeps the address from 36 and copies only the trace ID from 38.
- The mapping wrapper (`...62ccc`) locks client mapping state and passes flag `0x1001` and stride 8 to `...62b54`. At `...62bd4` the resolved call is `IOMemoryDescriptor::createMappingInTask`; the address returned at `...62c60` is mapped base plus row index × 8. `IOMapTypes.h` identifies `0x1001` as `kIOMapAnywhere | kIOMapReadOnly`. The stock map uses bank descriptors; no Apple page size, slot count or memory-layout constant is transferred to X9 by this observation.
- Selector 45 performs the same task-scoped object lookup/type check, converts timeout to a deadline via `clock_interval_to_deadline` with scale 1,000,000 ns, and invokes wait at `...45228`. The local XNU clock API takes a 32-bit interval; don't assume arbitrary 64-bit timeout semantics. The wait reads the counter with acquire ordering and tests unsigned value ≥ target before and after enrolling. The handler returns success on the real predicate and `0xe00002d6` (Timeout) on its false result.

For X9 implementation, map only dedicated event state read-only, with bounded per-task and system resources; require Mach lookup and a class match before mapping or mutating. Do not put unrelated kernel state on an exposed page. Hold event/notification/mapping ownership through delivery and detach it when the last relevant right is released; prove right-death and client-close handling before exposing non-nil events. Stock bank layout is not a requirement for a compatible allocator. This pass does not modify driver behavior.

Offline repro: `python3 -I x9/out/x9-event-mapping-20261126/inspect.py` resolves GOT stubs via the fixed image. Bounded ranges `fffffff005a62b54 178`, `fffffff005a452e0 78`, `fffffff005a605cc 104`, `fffffff005a45228 108` were read by the same script with VA and length arguments. Userspace counterpart: `IOSurfaceSharedEvent init`, `initWithMachPort:`, `signaledValue` in existing 20H392 `/tmp/iosurface.dis`.

## X9 counter implementation (not yet exposed)

`SweetSharedEventCounter.inc` is compiled in the IOSurface translation unit. It owns a dedicated zeroed page per event for an aligned u64 counter, bounded to 128 live objects. Counter updates are monotonically increasing, release-published and wake real waiters. Wait tests the acquire-loaded predicate under the event lock, then uses the kernel deadline wait and checks the predicate again even on timeout. A caller-authorized map uses read-only protection and returns an owned map; the caller must retain the counter while that map exists. `make()` is not called by any selector and no Mach port is published by this layer. The eventual capability layer must separately cap maps, type-check rights and synchronize cancellation/close with async notifications.

Host fixture: compile `x9/tools/tests/shared_event_counter_host.cpp` with Xcode 15 (C++17). It exercises the actual counter code using test stand-ins for locks, VM maps and clock waits, including page-zeroing, map flags, monotonic signaling, spurious wake, timeout/signal coincidence and quota rollback. It does not exercise XNU scheduling, user mappings or rights teardown.

The X9 capability helpers now implement task-local Mach export and import via the XNU IOKit identity APIs, with an event-class check and explicit owning-task check. Create maps before publishing the right; an invalid export rolls back the mapping. Import reuses an existing map for the same event on that client and returns value/VA/trace ID. There are at most 64 views per client and 512 system-wide; closing a client releases its maps before releasing the retained event objects. These helpers are **not exposed through selectors**. Cached views currently retire on client close, not on last right deallocation; this is bounded but must not be mistaken for stock long-lived-client behavior. The capability host fixture simulates task ports and maps and checks rollback, wrong-class/foreign-task rejection, reuse and close; it does not prove Mach/VM lifecycle on the phone.

**Supersedes the earlier close-only cache behavior (3f829d17).** The Mach name now identifies a separate reference object owning the counter and bounded references to per-client view owners. The view owner does not retain the client or the right; there is no right→owner→right cycle. Closing a client detaches its maps; destruction of the Mach identity object can detach views before connection close. Local XNU `IOMachPort::noMoreSendersForObject` drops the registry's tagged object reference after unlocking the registry, so cleanup is driven by the last retained object reference (including any in-progress import). The host fixture simulates last-right reclamation and two-client close; real Mach topology, task death and map teardown remain device verification. A right may retain closed owner objects up to its finite owner-array capacity; do not expose this infrastructure until that capacity behavior, wait/delivery ordering and selector integration are addressed.

## Selector 42：surface ordering 与 QuartzCore 回退（2026-11-26）

Stock wrapper `0xfffffff005a5d928` 使用四个 scalar：`u32 surface ID, u64 mode, u32 event port, u64 value`。匹配用户态 `_IOSurfaceClientSignalEvent` `0x1a3986f80..6fe8` 从 surface client、event port 和其余两参组成此顺序；`_IOSurfaceSignalEvent` 取得 event port 后转调它。stock handler `0xfffffff005a604f8` 先在本 client 查 surface、再以当前 task 解析并检查 event reference，最终调用 `0xfffffff005a4dae4`。后者对 mode==1 返回非零，等待 surface 某状态解除后：surface `+0x40` 有绑定对象时调用 `0xfffffff005a56944`（向绑定回调提交 operation 9），无绑定才调用 `0xfffffff005a44c84` 直接推进 counter。不能把 callback 提交、返回、GPU 完成和物理 scanout 混为一谈；尚未证明绑定对象的完成与信号顺序，也未证明 X9 不需要该顺序。

匹配 QuartzCore `layer_private_present` `0x18829e100..164` 以 `(surface, event, drawableID, 0)` 调用优化 stub `0x18dbcc748`，其 dyld cache 实际机器码直接跳转 `0x1a3988a30`，即 `_IOSurfaceSignalEvent`。非零返回转到 `0x18829e1ac` 的普通 `layer_private_present_impl`，不会走随后 `notifyListener`；零返回才可能注册通知。该证据说明此**有条件路径**需要真实 42 才能走 fence 分支，不证明 AegirPoster 曾走到它，也不证明其他消费者能处理错误。

X9 之前在 `x9_surf_events=1` 下 selector 42 会落入默认分支并伪成功，即没有更新计数器却可能让 QuartzCore 等待通知。现改为仅在该显式实验开关下路由 42，执行 stock 4-in/0-out 形状校验后返回 `kIOReturnUnsupported`；不写 event、不执行虚假的直接 signal。默认不开启时的行为不变。这是本地 QuartzCore 有证据的回退策略，不是 selector 42 实现、GPU fence 或设备止崩验收；其他上层对 unsupported 的反应仍未知。

复核：

```sh
python3 -I x9/tools/tests/inspect_shared_event_42.py \
 /private/tmp/x9sys/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.04
python3 -I x9/out/native-event-abi-20261126/inspect.py fffffff005a5d928 18
python3 -I x9/out/native-event-abi-20261126/inspect.py fffffff005a604f8 d4
python3 -I x9/out/native-event-abi-20261126/inspect.py fffffff005a4dae4 e8
sed -n '7195,7230p;9067,9088p' /tmp/iosurface.dis
sed -n '25110,25208p' x9/out/shared-event-consumer-20261126/quartz.dis
```

上述命令 exit 0（既有固定 SystemOS DMG 只读挂载；无新挂载或设备操作）。Xcode 15 的 `shared_event_capability_host.cpp` 编译运行 exit 0，覆盖 42 合法形状 Unsupported 与错误形状 BadArgument；recipe `SweetSurface.o` 单对象编译 exit 0，560920 B、12 warnings，日志 `x9/out/event42-host-20261126/compile.log`（worktree gitignored）。静态 stub 脚本另检查外层 selector 42 被实验 gate 包含；不模拟实际 caller、GPU 或 Mach。
