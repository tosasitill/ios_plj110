# Phase Report — Task 7 Display Backend Boundary

## 1. Scope

Audit the existing display path and perform the first step of spec §7.3: give
the legacy copy presenter and display-buffer validation an explicit boundary,
without changing which path is selected. Direct scanout, a separate display
fence and hardware validation remain later steps.

## 2. Existing Implementation

- `AppleCLCD` is the built-in IOMobileFramebuffer. `SwapEnd` records the latest
  frame and holds its surfaces; `vsyncFired` shifts pending → show → retire and
  releases holds one vsync later.
- `present()` selects `directPresent()`, the staged LK GUI path, or CPU
  composition into a cached buffer, then flips or submits at TE.
- `directPresent()` validates a single full-screen surface inline (scanout
  address, 4-byte elements, size, 16-byte alignment, below 4 GiB, kernel
  mapping). With a flip buffer it copies into that buffer instead of scanning
  the client surface, because CA can start the next frame in the same surface.
- CPU copy primitives (format conversion, premultiplied blend, nearest-
  neighbour scaling, row copies) were file-static functions inside
  `SweetFramebuffer.cpp`.

## 3. Changes

New `x9/kernel/src/iokit/Kernel/arm/X9DisplayBackend.inc`:

- `x9_display::Surface` and `Scanout` descriptions.
- `format()`, `blend_enabled()`, `to_bgra()`, `blend_pixel()`, `row()`,
  `rows()`, `scaled()` — moved verbatim.
- `validate()` — the same pre-mapping acceptance conditions as before, plus a
  diagnostic `Reject` reason;
- `mapped_addresses()` — fills the CPU windows after the caller obtains the
  mapping.

`x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp`:

- includes the new file at `:1474`;
- `directPresent()` `:1482` builds a `Surface`, uses `validate()`/`Scanout`,
  then calls `s->kernelAddress()` and `mapped_addresses()` (`:1488-1535`).
  `kernelAddress()` may create a mapping lazily, so it remains behind the same
  checks and in the same order;
- `x9_layer_format`, `x9_blend_enabled`, `x9_layer_row`, `x9_layer_scaled`
  remain as thin wrappers (`:1556-1590`) so `present()` call sites are
  unchanged.

## 4. Architecture

The display backend now has two parts:

```text
AppleCLCD (service, swap, holds, vsync, TE, path selection)
        │
        ▼
x9_display (buffer description, acceptance validation, copy primitives)
        │
        ▼
panel mapping / MDP page flip / LK GUI submit
```

`x9_display` owns no hardware access, lock, allocation or policy. `AppleCLCD`
still owns every decision and every write to MMIO.

## 5. ABI Impact

None (`VERIFIED` by inspection). No IOMobileFramebuffer selector, structure or
userspace-visible behavior changed.

## 6. Memory Impact

None (`VERIFIED` by inspection). No allocation, mapping, cache operation or
surface hold changed. `Surface.memory_length` is recorded for future bounds
checks but not used by `validate()`; acceptance is unchanged.

## 7. Synchronization Impact

None (`VERIFIED` by inspection). No lock, lock order, hold, vsync cadence or
TE deadline changed. `validate()` is pure and runs on the existing present
thread.

## 8. Build Result

`BUILD VERIFIED` end to end: `build.py all --revision 230` compiled 51/51
overlay objects (including `SweetFramebuffer.cpp` with the new include), linked
and stripped `kernel.development.sweet` (17995904 bytes), and packaged
`boot.img` (9420800 bytes, SWEET-XNU candidate, entry 0x1000). No compile error
or new warning came from the new file. Boot packaging needed `llvm-objcopy`/
`llvm-nm` from `/opt/homebrew/opt/llvm@22/bin` and the `lz4` module in the
packaging interpreter; those are host tooling gaps, not source failures. No
flashing and no device write was performed.

## 9. Runtime Result

`DEVICE VERIFIED` for the extracted path under the copy presenter: the rev 230
boot log shows `X9 DISPLAY: pan168 present entered` on the first frames, the
extracted `validate()` accepted the same full-screen surfaces, and the copy
path ran for the session with cadence `copies=27–45` per 2 s, `missed=0`,
`copy_avg_us≈1.3–1.8 ms`, and `X9 DISPLAY TE: frames=… timeouts=0`. No display
panic and no crash referable to the extraction. Direct scanout was not enabled,
so no `directPresent()` DMA A/B exists (`NOT TESTED`).

## 10. Performance

The copy path's own counters from the device log are the only numbers: average
copy 1.3–1.8 ms, max 2.4 ms, `late_max_us≈1 ms`, no missed cadence ticks. They
describe copying into the flip buffer, not scanout of the client surface. No
performance claim is made for direct scanout.

## 11. Regression

`DEVICE VERIFIED` in the captured session: no black/garbled full-screen
presentation attributable to the extraction, TE observed with zero timeouts,
and the desktop stayed up until the capture ended (72.5 s). Visual correctness
of individual layers over long sessions and the Control Center platter remain
`NOT TESTED`/unresolved.

## 12. Rollback

Revert `SweetFramebuffer.cpp` to inline validation/helpers and delete
`X9DisplayBackend.inc`. No boot arg, persistent state or partition artifact is
involved.

## 13. Remaining Issues

1. Direct scanout of a client surface is `NOT IMPLEMENTED`: the current path
   copies into the flip buffer to avoid half-drawn frames.
2. A surface used by `directPresent()` is not enrolled in the show/retire hold
   arrays; a real direct path needs that queue first.
3. Display DMA/IOVA domain compatibility is `NEEDS AUDIT` on hardware; a GPU
   VA is not assumed to be display-fetchable.
4. Modifiers, multi-plane and 10-bit formats are `NOT IMPLEMENTED`.
5. There is no separate display fence; GPU ownership remains a TryLock plus
   frame deferral.
6. Control Center platter triangles remain `UNRESOLVED`; this phase does not
   touch rendering and does not claim to explain them.
7. Device test (spec step 10) is pending and must be performed by the owner.
