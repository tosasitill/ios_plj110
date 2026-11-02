# Display Backend and Scanout Boundary (Task 7)

Status date: 2026-11-02. Revision 230 (`feat/G1Ultra`, HEAD `5b3caad`). No
device run. The display service is the kernel's built-in IOMobileFramebuffer;
this task does not reimplement it and does not change panel timing, brightness,
TE handling, holds or the selected present path.

## 1. Existing implementation (`SOURCE CONFIRMED`)

### 1.1 Service and selectors

`x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp` implements `AppleCLCD`
(IOMobileFramebuffer). CoreAnimation uses selectors 3/4/5/6/7/8/12/18/28/72
(`:140-151`). `SwapBegin` returns an id; `SwapEnd` carries up to four layer
surface ids, source rects, destination rects and an enabled mask
(`:15-17`, `:1181`).

### 1.2 Frame admission and holds

- `swap()` copies each enabled surface reference, increments the IOSurface
  in-use counter with `sweet_hold()` (`:58-72`), and replaces `fPending` /
  `fPendHold` (`:1195-1199`). A surface replaced before it was shown is
  released immediately.
- `vsyncFired()` moves `fPendHold → fShowHold → fRetireHold` and releases
  retired holds one vsync later (`:1056-1087`). `fLastSurf` separately keeps
  the directly presented surface alive (`:1529-1543`).

### 1.3 Present paths

`present()` (`:1679`) selects, in order:

1. `directPresent()` (`:1482`) when exactly one enabled layer exists and its
   surface is scanout-registered.
2. A staged GPU path for the default panel surface when the kernel LK GUI is
   idle (`sweet_x9_desktop_frame_begin/queue`, `:1723-1729`, `:1969-1980`).
3. CPU composition into a cached back buffer or the panel mapping, including
   per-layer format conversion, premultiplied blending, nearest-neighbour
   scaling and a multi-layer shadow (`fX9Compose`) when the panel mapping is
   write-combined (`:1753-1972`).
4. `flipBack()` when a double-buffered console/back buffer exists, otherwise
   `x9QueuePresent()` → LK GUI submit on a 100 µs timer with a 50 ms bound and
   a fresh-TE requirement (`:917-944`).

`SweetX9DesktopWorker.inc` runs the staged GPU path on a kernel thread with its
own pool, at most three published frames, CPU verification and generation
matching.

### 1.4 Direct path is validation plus copy, not client-surface scanout

`directPresent()` accepts only:

- `fVig0 != NULL` (display controller mapped),
- exactly one enabled layer with a surface id,
- `scanoutPhys != 0` (surface registered with the display pool),
- `bytesPerElement == 4`, surface at least panel-sized,
- fetch address 16-byte aligned and the whole panel window below 4 GiB,
- a non-zero kernel mapping.

When `fFlip[0]` exists it **copies into the back buffer and flips** instead of
pointing the MDP at the client surface (`:1520-1526`). The comment records why:
CoreAnimation can begin the next frame in the same surface immediately, and
direct scanning let the panel show half-drawn frames. So the code path is
named "direct" but is not direct client-surface scanout.

## 2. What was extracted (`BUILD VERIFIED`)

New `x9/kernel/src/iokit/Kernel/arm/X9DisplayBackend.inc`, included at
`SweetFramebuffer.cpp:1474`. It is a display-backend boundary, not a new
service:

| Element | Purpose |
|---|---|
| `Surface` (`:20`) | explicit description of one candidate buffer: scanout address, offset, mapping, format, element size, geometry, stride |
| `Scanout` (`:34`) | computed window: fetch address, kernel address, cleaned window, stride, logical offsets |
| `Format`, `format()` (`:54`) | the three accepted input formats; anything else is unsupported |
| `blend_enabled()` (`:71`) | the existing `sweetfb_blend` policy |
| `to_bgra`, `blend_pixel`, `row`, `rows`, `scaled` (`:115-191`) | the legacy CPU copy presenter primitives, moved verbatim |
| `validate()` (`:194`) | the **same** pre-mapping acceptance conditions `directPresent()` applied inline, plus a `Reject` reason for diagnostics |
| `mapped_addresses()` (`:262`) | fills the kernel/cache windows after the caller has obtained the mapping |

`SweetFramebuffer.cpp` now builds a `Surface`, calls `validate()`, then calls
`s->kernelAddress()` and `mapped_addresses()` (`:1488-1535`). The mapping call
stays behind the same checks and in the same order as before, because
`kernelAddress()` may create a mapping lazily. The former helper functions are
thin wrappers (`:1556-1590`) so every call site keeps its previous name,
arguments and order. `present()` is otherwise unchanged.

No rule was added or removed: a surface accepted before is still accepted, and
the fallback remains "return false → copy path". The extraction is mechanical
so it can be reverted independently.

## 3. Display backend responsibility map (spec §7.2)

| Responsibility | Where it lives now | Status |
|---|---|---|
| Display mode / resolution | `start()` from device tree, logical iPhone-X size | `SOURCE CONFIRMED` |
| Pixel format | `validate()` (4-byte) and `format()` (BGRA/RGBA/ARGB) | `SOURCE CONFIRMED`; no 10-bit |
| Display buffer | `SweetSurface`, `scanoutPhys`, `sweet_hold` | `SOURCE CONFIRMED` |
| Layer / plane | four IOMFB layers composed on CPU; one display plane | `SOURCE CONFIRMED` |
| Buffer queue | pending/show/retire hold arrays; one pending swap | partial, `SOURCE CONFIRMED` |
| VSync / TE | 60 Hz `IOTimerEventSource` and LK fresh-TE submit | `SOURCE CONFIRMED` |
| Page flip | `scanFrom()` latched at vsync, or `flipBack()` | `SOURCE CONFIRMED` |
| Scanout lifetime | holds from `SwapEnd` until one vsync after replacement | `SOURCE CONFIRMED` |
| Display fence | no separate display fence object; GPU ownership is a TryLock and frames defer | `NOT IMPLEMENTED` as a fence |
| GPU fence | kernel render owner / `resident->uncertain`; userspace never waits in display | `SOURCE CONFIRMED` |

## 4. Direct scanout prerequisites (spec §7.5)

| Question | Current answer |
|---|---|
| Pixel formats | 4-byte BGRA/RGBA/ARGB inputs; 8-bit scanout only (`NativeBridgeMetalFrontend.mm:1826` withholds XR10) |
| Width/height | surface must be at least the 1080×2400 panel window |
| Row stride | taken from `SweetSurface::bytesPerRow`; no separate display-stride check |
| Alignment | fetch address must be 16-byte aligned |
| Plane count | one plane; planar/tiled surfaces are not direct-eligible |
| Modifier | none modelled; `NOT IMPLEMENTED` |
| GPU IOVA as display DMA | `scanoutPhys` is a display-side physical address, not the GPU VA; no shared IOMMU domain is programmed (`NEEDS AUDIT` on hardware) |
| Address width | below 4 GiB, enforced |
| Concurrency | copy path exists because CA may rewrite the surface during scanout |

The most important open question is unchanged: a GPU-accessible IOVA is not
evidence that the display controller can fetch it. The code does not assume
that; it requires a separate `scanoutPhys` registration.

## 5. Task 7 step status

| Step | Status |
|---|---|
| 1 Extract legacy copy presenter | partial: primitives + validation extracted (`BUILD VERIFIED`); orchestration remains in `AppleCLCD::present` by design |
| 2 Display buffer lifecycle | audited (`SOURCE CONFIRMED`), no new lifecycle object |
| 3 Verify display DMA address space | validation extracted; hardware IOMMU domain `NOT IMPLEMENTED` / `NEEDS AUDIT` |
| 4 Verify format and stride | code-level checks extracted; device confirmation `NOT TESTED` |
| 5 Buffer queue | existing pending/show/retire holds, single pending frame; no triple-buffer queue object `NOT IMPLEMENTED` |
| 6 VSync / TE page flip | existing timer and fresh-TE submit `SOURCE CONFIRMED`, not changed |
| 7 GPU fence integration | existing TryLock/defer model `SOURCE CONFIRMED`; separate display fence `NOT IMPLEMENTED` |
| 8 Direct scanout | `NOT IMPLEMENTED` as client-surface scanout; the current path copies into a flip buffer to avoid tearing |
| 9 Legacy copy fallback | present and verified in source (`SOURCE CONFIRMED`); device behavior `NOT TESTED` |
| 10 Real-device test | `NOT TESTED`; must be run by the device owner |

## 6. Why direct scanout was not enabled here

Enabling client-surface scanout requires a queue that guarantees CA cannot
reuse a surface while the MDP is fetching it. The hold mechanism exists, but
`directPresent()` does not enroll its surface in the show/retire arrays, and
the previous direct attempt produced half-drawn frames. Changing that is a
display-behavior change that must be validated on the panel; it cannot be
declared correct from a compile.

## 7. Next safe increments

1. Enroll `directPresent()` surfaces in the existing hold arrays and add a
   boot-arg-gated direct mode that still falls back to copy (device A/B).
2. Record per-path counters (`copy`, `direct`, fallback reason) in the display
   backend and expose them through the existing telemetry consumer.
3. Only after a stable direct A/B, add the explicit three-buffer queue and a
   display fence distinct from the GPU completion record.
