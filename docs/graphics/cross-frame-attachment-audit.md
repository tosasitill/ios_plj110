# Cross-Frame Attachment and Buffer Lifecycle Audit

- Task 10 / investigation D. Read-only source audit; no production code was modified.
- Audit date: 2026-11-02. Branch `feat/G1Ultra`, HEAD `646b7cd` (runtime/Mesa sources
  unchanged from `681f64e`; the working tree also holds a rev 232 manifest and a
  frontend histogram edit that are outside this audit).
- Sources: `metal2vulkan-pan205-source` (runtime, frontend), `x9/metal/General`
  (Metal bridge, buffer manager), `x9/kernel/src/iokit/Kernel/arm` (render engine,
  framebuffer).
- Evidence tags: `SOURCE CONFIRMED`, `CAPTURE`, `INFERENCE`, `UNKNOWN` as before.

## 1. Question

Can an attachment or IOSurface be reused while an earlier GPU operation still
accesses it, or while the display still consumes it? The question is only meaningful
after separating four events that are routinely conflated: the submission returning,
the GPU finishing execution, the GPU's writes becoming visible to the CPU, and the
display releasing the surface.

## 2. What an attachment is across frames

### 2.1 Imported IOSurfaces (presented drawables)

An IOSurface texture wraps the surface's pages directly. The import cache is keyed by
`{IOSurfaceGetID, base address, page-rounded allocation size}` in a
strong-to-weak `NSMapTable` capped at 256 entries
(`NativeBridgeMetalFrontend.mm:300-332`). A hit reuses the existing `NBBuffer`; a miss
creates a new no-copy import of the same pages (`:315-320`). The runtime never zeroes
an imported surface. `SOURCE CONFIRMED`.

Consequences:

- "Same surface" across frames means "same physical bytes". Prior contents are visible
  to any `Load` on that surface.
- A cache hit is not a content guarantee. It only means the import object is still
  alive; the pixels are whatever the compositor last wrote through either mapping.
- Evicting a cache entry is harmless: the owners still retain the import
  (`:322-328`). Eviction cannot by itself detach a live surface.

In the saved capture the presented surface alternates between IOSurface IDs 2 and 3
(`X9 FRAME source ... id=3`, then `id=2`, then `id=3` again in
`.local/analysis-blur212/runtime-latest.raw.log` at `15.893 / 31.690 / 33.390`), i.e.
two drawables are in rotation. `CAPTURE`.

### 2.2 Runtime-owned images

Recycling applies only to images with identical `ImageMaking` and excludes shareable
(IOSurface) textures, heap textures, host-page-backed images, non-own storage and
stand-in storage (`recycle.rs:36-39`). A recycled image is handed over `UNDEFINED` and
its first move writes zeros or the Metal pattern; it never exposes the previous owner's
pixels (`recycle.rs:23-35`, `texture/first_move.rs:13-24`). `--cfg m2v_no_recycle`
disables the path. `SOURCE CONFIRMED`.

So the presented surface is never recycled by the runtime, and an internal attachment
never inherits another frame's pixels through recycling.

### 2.3 Within one frame

Instance splitting and internal intermediate images are covered in
`first-frame-rendering-audit.md` §2. The relevant point here is that every intermediate
that the runtime allocates starts from zero/pattern, and only the IOSurface has prior
contents.

## 3. The four completion events, separately

| Event | What it means here | Evidence |
|---|---|---|
| Submission returns | The native ABI accepted the command stream | `x9_pan_kmod_submit` (`X9PanVKMemory.c:318-340`) |
| GPU execution complete | GPU fence equals `submission_id`, CSG suspended, address space detached | `SweetX9RenderEngine.inc:423-459` |
| Writes visible to CPU | Clean/invalidate of GPU-written, CPU-exposed resources | `SweetX9RenderEngine.inc:461-478` |
| Display released | IOSurface in-use count decremented, one vsync after the surface left the panel | `SweetFramebuffer.cpp:54-60, 1039-1081` |

### 3.1 Submission

`x9_pan_kmod_submit` accepts only a **synchronous** acknowledgement:

```c
if (actual.completed_id != request.submission_id || actual.status || ... )
    d->failed = true, ret = error(EPROTO);
```

(`X9PanVKMemory.c:331-338`). A queued or pending reply poisons the device. There is
therefore no window in which the client regains control with the submission still in
flight. `SOURCE CONFIRMED`.

### 3.2 GPU execution

The kernel engine writes a fence value into the command stream
(`x9_cs_sync_set64`, `SweetX9RenderEngine.inc:390`), then polls the resident fence page
until it equals `submission_id`, with a 10 s deadline and the same sentinel-byte checks
(`:423-451`). Only after that does it halt the CSG group, `flush_gpu`, and detach the
address space (`:452-456`). `SOURCE CONFIRMED`.

The fence is observed on CPU-held private pages; the engine synchronizes those pages
before each poll (`:429-438`). `SOURCE CONFIRMED`.

### 3.3 Cache visibility

Order around submit:

1. Before submission, CPU writes to client resources are cleaned/invalidated
   (`:348-361`).
2. After the fence and detach, resources flagged `X9RenderGPUWrite` that are
   `cpu_exposed` are cleaned/invalidated (`:461-478`). Never-exposed allocations skip
   this; coherent ones skip it too.
3. Only then is `*reply` written with `completed_id = submission_id` and status 0
   (`:480-481`).

So `completed_id` already implies "GPU quiescent **and** its writes visible to the
CPU". A successful submission reply is stronger than "the GPU started", but it still
says nothing about the display. `SOURCE CONFIRMED`.

One consequence worth stating explicitly: the display's panel copy is a CPU read of
the surface. Because the cache handoff happens before the reply, a copy that begins
after the reply sees the GPU's writes. Cache visibility is therefore **not** a
plausible cause of a frame that mixes old and new pixels *within one submission*.
`INFERENCE` from `:461-481`.

### 3.4 Display consumption

The kernel does not hand the display a fence. It holds the surfaces of the most recent
swap record and advances the holds on the vsync workloop:

- `swap()` copies the record into `fPending` and takes holds on its surfaces
  (`SweetFramebuffer.cpp:1191-1200`).
- At vsync, if `fDirty`, the record and its holds move to local `taken`, then
  `present(rec)` composites/copies the layers (`:1039-1068`).
- After `present`, the holds rotate: previous `fShowHold` becomes `fRetireHold`, and
  the new `taken` becomes `fShowHold` (`:1076-1081`).
- At the next vsync the previous `fRetireHold` is released
  (`sweet_hold_set(retire, false)`, `:1061-1064`), with the comment "frames replaced at
  the previous vsync have been latched by now".

`sweet_hold` increments/decrements the IOSurface's in-use counter at offset `0x18` of
its shared state page, which is what `IOSurfaceClientIsInUse` reads, "so
CoreAnimation does not render into it" (`SweetFramebuffer.cpp:54-70`). `SOURCE
CONFIRMED`.

The rotation is one frame *slower* than the copy: a surface stays in use from the swap
that records it until one vsync after the next recorded frame has replaced it on the
panel. `SOURCE CONFIRMED` from the code; no capture measures the counter.

The Buffer Manager defines `x9_buffer_display_acquire/release`
(`X9BufferManager.c:97-122`) but **nothing calls them** — the symbol appears only in
that file and its header. Display lifecycle counters therefore always read Idle and
cannot be used as evidence of display release. `SOURCE CONFIRMED` (grep of `x9/`).

## 4. Can a buffer be reused during an earlier GPU access?

### 4.1 Within the Metal → PanVK path: no

- One group's submission is synchronous (§3.1), and the kernel holds the render owner
  lock across submit, fence, detach and cache handoff
  (`SweetX9RenderEngine.inc:269-481`, "submits are serialized by the render owner lock
  held here", `:417-419`).
- GPU references in the Buffer Manager are acquired at `commit` and released in
  `finish`, which runs only after the native completion callback (or a refusal)
  (`NativeBridgeMetalFrontend.mm:1223-1240`, `1255-1268`, `1321-1335`).
- `finish` retains its resources when the status is not `Completed` and not a local
  failure (`:1235-1240`), so an uncertain completion cannot release an IOSurface.

`SOURCE CONFIRMED`.

### 4.2 Against display consumption: guarded by the IOSurface in-use count

The only guard is the counter in §3.4. If the compositor's drawable acquisition honors
it — CA's own drawable allocation does, per the kernel comment — a surface cannot be
re-rendered while it is on the panel or pending. The audited stack does not implement
drawable allocation itself (no `CAMetalLayer`/`nextDrawable` in `x9/metal/General`), so
the honor decision is CoreAnimation's, not this code's. `INFERENCE` on the honor;
`SOURCE CONFIRMED` on the mechanism and its timing.

With two drawables, the safety margin is one frame: the compositor may only touch the
surface that is neither shown nor pending. Whether a frame is presented at 60 Hz
(compositor) against a 60 Hz panel leaves no extra buffer; if the compositor runs
faster, `nextDrawable` blocks. No capture measures this. `UNKNOWN`.

### 4.3 What a violation would look like

A reuse during display copy would mix pixels written by the GPU into the region being
copied: a boundary moving with the copy order, not a fixed geometric shape. It would
not produce a stable corner-to-corner diagonal on every card. That distinction matters
for the Control Center comparison. `INFERENCE`.

## 5. Lifecycle of the presented surface, end to end

```
CA renders (Load on prior bytes)                     <- surface still holds frame N-2
  commit: Buffer Manager gpu_refs++                  NativeBridgeMetalFrontend.mm:1265
  PanVK submit (synchronous)
    kernel: clean/invalidate client bytes            SweetX9RenderEngine.inc:348-361
    kernel: fence == submission_id                   :423-451
    kernel: CSG halt, flush_gpu, AS detach           :452-456
    kernel: clean/invalidate GPU-written bytes       :461-478
    reply: completed_id == submission_id             :480-481
  completion callback -> finish: gpu_refs--          NativeBridgeMetalFrontend.mm:1321-1335
CA present -> kernel swap(): fPending + holds        SweetFramebuffer.cpp:1191-1200
vsync: copy/composite, rotate holds                  :1039-1081
vsync+1: release previous fRetireHold (in-use--)     :1061-1064
```

Two observations that are easy to get wrong:

- A successful submission reply is **not** display release. The surface stays in use
  through the vsync that shows its successor.
- GPU completion is **not** cache visibility in general; here it is made to imply it
  because the engine performs the clean/invalidate before replying. That ordering is
  load-bearing and should be treated as a contract, not an accident.

## 6. Ranked hazards for a first-frame/transient artifact

1. **`Load` of an imported surface that the first frame does not fully cover.**
   `SOURCE CONFIRMED` mechanism, and the capture shows the drawable's clear is a
   `Load`+`Store` quad draw (`damage-tracking-audit.md` §6.3). Not a
   lifecycle/reuse hazard, but it is the hazard the lifecycle creates: the surface's
   prior bytes are legitimate `LOAD` input, so any coverage gap shows them.
2. **A clear draw whose triangle coverage is incomplete.** `INFERENCE`. The only
   captured full-surface paint from zero is a two-triangle quad with `Load`; a missing
   or mispositioned triangle leaves a triangular stale region and nothing else in this
   audit explains a triangular shape.
3. **Display reuse racing the panel copy.** `UNKNOWN`. Guarded by an IOSurface in-use
   counter with one frame of margin; no caller-side counter in the Buffer Manager to
   confirm it. Would look like a moving tear, not a fixed triangle.
4. **Recycled internal image read by `LOAD`.** Mechanism exists, always reads zeros,
   and excludes IOSurfaces (`recycle.rs:36-39`). `LIKELY` irrelevant to the observed
   folder/CC defect.
5. **Cache visibility lag.** `NOT A MECHANISM` for pixels produced before the reply:
   the engine cleans/invalidates before setting `completed_id`
   (`SweetX9RenderEngine.inc:461-481`). Any claim that the defect is "CPU cache" needs
   to explain this ordering.

## 7. Unavailable instrumentation

- No row records the IOSurface in-use counter value or its transitions.
- `x9_buffer_display_*` is dead code, so the Buffer Manager cannot report display
  state.
- No per-frame surface identity is attached to `X9 GRAPH` rows; `X9 GRAPH TEX` shows
  `slot/tex/parent/surface`, but only for sampled draws (six per shader pair), and the
  drawable rows did not include a surface ID in the analyzed captures
  (`surface=0` on the folder blur `slot=3` row).
- No kernel row records which swap record's surfaces are held, or the release vsync.
- The 32×32 point sampler cannot verify pixel-level reuse effects
  (`SweetFramebuffer.cpp:1160-1166`).

## 8. Conclusions

| Statement | Status |
|---|---|
| IOSurface imports share physical bytes across frames; never zeroed | `SOURCE CONFIRMED` |
| Runtime recycling never exposes prior pixels and excludes IOSurfaces | `SOURCE CONFIRMED` |
| Submission is synchronous and poisons the device otherwise | `SOURCE CONFIRMED` |
| `completed_id` implies GPU quiescent + writes CPU-visible | `SOURCE CONFIRMED` |
| Display release is an IOSurface in-use count, one vsync late | `SOURCE CONFIRMED` |
| Buffer Manager display lifecycle counters are never driven | `SOURCE CONFIRMED` |
| A buffer can be reused during an earlier GPU submission | **No**, for the audited path |
| A drawable can be reused during display consumption | Prevented by the CA/IOSurface protocol; honor not measured (`UNKNOWN`) |
| Cross-frame reuse explains a fixed triangular region | **Not supported** |
