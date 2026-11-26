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

Repro: in `x9/out/native-event-abi-20261126/`, `python3 -I inspect.py` lists the table rows, using `x9/usb/prepare_native.py:MachO`; `python3 -I inspect.py fffffff005a5d87c 188` reads the wrapper range, and `python3 -I inspect.py fffffff005a5ff9c 55c` reads bounded handlers. The private disassembly remains in ignored scratch.
