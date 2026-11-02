# First-Frame Rendering Audit — Metal pass semantics, attachment contents, tile clear reach

- Task 10 / investigation A. Read-only source audit; no production code was modified.
- Audit date: 2026-11-02. Branch `feat/G1Ultra`, HEAD `646b7cd` (the runtime and
  Mesa sources read here are unchanged from `681f64e`; the working tree also holds a
  rev 232 manifest and a frontend histogram edit that are outside this audit).
- Sources read: `metal2vulkan-pan205-source` (runtime + frontend), X9 Mesa
  `mesa-source` (PanVK v14 + panfrost common), `x9/metal/General` frontend.
- Evidence tags: `SOURCE CONFIRMED` = read at the cited line; `CAPTURE` = read from a
  local runtime log; `LIKELY` = implied by code, not measured; `UNKNOWN` = needs new
  instrumentation. Nothing here is `DEVICE VERIFIED` for the defect itself.

## 1. Question

The suspected defect is that the first frame after an interface transition (folder,
App Library) is rendered against incorrect attachment contents, and that a later
redraw/composition overwrites the affected region. This document audits the exact
Metal→Vulkan load/store mapping, the runtime's transformations of it, and the points
at which previous attachment contents can be loaded, cleared, discarded or stored.
It does not assert a root cause.

## 2. What Metal asks for, and what the runtime emits

### 2.1 Load and store actions

`LoadAction` maps 1:1 to Vulkan in `render_target.rs`:

```rust
LoadAction::Load     => vk::AttachmentLoadOp::LOAD,
LoadAction::Clear    => vk::AttachmentLoadOp::CLEAR,
LoadAction::DontCare => vk::AttachmentLoadOp::DONT_CARE,
```

`render_target.rs:374-382` (`load_op`). Store actions are conservative:
`Store`/`StoreAndMultisampleResolve` become `STORE`; `DontCare` and
`MultisampleResolve` go through `DontCare::of`, which is only allowed to discard where
a driver fact says MoltenVK behaves like Metal. PanVK has no such fact, so those
stores become `STORE` (`render_target.rs:384-397`). `SOURCE CONFIRMED`.

Consequence: **on PanVK, a Metal `DontCare` store still writes the attachment.** A
stale cross-frame discard caused by a store action is therefore not the default
mechanism; the interesting direction is the load side.

### 2.2 Instance splitting

A Metal encoder becomes one or more render-pass instances. The first instance keeps
the encoder's load action; every later instance loads `LOAD`. The last instance keeps
the encoder's store action; earlier ones store `STORE`
(`render_target.rs:473-489`, `load_at`/`store_at`). `SOURCE CONFIRMED`.

An instance boundary therefore converts "the client wrote `DontCare`" into "load the
image". That is safe for correctness only if the image actually holds the pre-pass
contents. On the first frame of a freshly imported or freshly created attachment it
does not necessarily.

### 2.3 The `DontCare` rewrite (the strongest load-side transformation)

Metal's measured answer for a `DontCare` load of an attachment the pass stores is not
Vulkan's `DONT_CARE`: in a pass whose draws touch a tile, the tile holds **zero**
(color) or the attachment's clear value (depth/stencil) before the draws; tiles no
primitive touches keep their texels (`clear_reach.rs:19-25`).

The runtime reproduces this with a host-side rewrite (`render.rs:4289-4326`):

| Pass reach | `DontCare` load becomes |
|---|---|
| no tile reached | `LOAD` (contents preserved) |
| every tile reached | `CLEAR` to format zero / clear value |
| only the tiles the draws' primitives touch | `CLEAR`, but only over the tiles the draws' **viewports ∩ scissors** reach (`clear_reach.rs:34-44`, `dont_care_loads` `:204-268`) |

The third row is a near-equivalent, announced once per device as
`Degradation::DontCareLoadTiles` (`render.rs:4317-4326`); the announcement is
diagnostic only, the rewrite is unconditional. `SOURCE CONFIRMED`.

The tile grid is 32×32 pixels at 1–2 samples, 32×16 at 4 samples
(`clear_reach.rs:21-22, 96, 452-480`). The rewrite can therefore change pixels in
rectangles aligned to that grid whose extent is bounded by the draw's scissor, and it
never invents content outside the scissor.

**Reachability in the captured runs:** no captured pass in either the folder or the
swipe run uses `DontCare` load together with `Store`. The observed combinations are
`load0/store0` (declared-but-unused second color attachment), `load1/store1`, and
`load2/store1` (`CAPTURE`, see §6). The rewrite is not exercised by any captured draw;
it remains a live mechanism for uncaptured draws.

## 3. PanVK's side of the same contract

`get_att_fb_load_op()` (`panvk_vX_cmd_draw.c:20-51`) maps:

- `CLEAR` → `PAN_FB_LOAD_CLEAR`
- `LOAD` → `PAN_FB_LOAD_IMAGE`
- `NONE`/`DONT_CARE` + `STORE_OP_NONE` → forced `PAN_FB_LOAD_IMAGE` (preserve contents
  through partial renders; there is a TODO to relax this)
- `NONE`/`DONT_CARE` + a real store → `PAN_FB_LOAD_NONE`

Colour load targets set `.border_load = PAN_FB_LOAD_IMAGE` unconditionally and
`.always = (loadOp == CLEAR)` (`panvk_vX_cmd_draw.c:192-205`; Z/S at `:423-431`).
`pan_fill_fb_info()` turns that into `.preload = (in_bounds_load == IMAGE)`,
`.clear = (border or in-bounds == CLEAR)`, `.discard = !store` (`pan_fb.c:326-333`).
`SOURCE CONFIRMED`.

Two consequences matter for first-frame contents:

1. **Border pixels are always loaded.** Only pixels inside the render area can be
   affected by a load-op transformation. If the render area equals the tiling area
   (the normal case), the whole attachment is in-bounds.
2. **`CLEAR` is the only load that enables clean-tile write-back**:
   `pan_fb_load_target_always()` returns `target->always` (`pan_fb.c:396-404`), and
   `always` is set only for `CLEAR` (`panvk_vX_cmd_draw.c:195`); clean-tile write is
   gated on that bit or on AFBC (`pan_fb.c:421-434, 580, 656`). A `LOAD` pass writes
   back only tiles it dirtied, which is correct because untouched tiles are unchanged
   in memory. A `DONT_CARE`+store pass neither preloads nor clean-writes untouched
   tiles: those tiles keep whatever the image held, which is exactly Vulkan's
   `DONT_CARE` and exactly Metal's "untouched tiles keep their texels" only if the
   image already held the prior frame.

There is no software dirty-tile mask and no damage-rect consumer in PanVK.

## 4. Framebuffer fetch

Metal framebuffer fetch is lowered to input attachments when
`rasterizationOrderColorAttachmentAccess` is available, else to storage images under
the ordered pixel interlock, else refused (`framebuffer_fetch.rs:109-124`).
`SOURCE CONFIRMED`.

The subpass declares only a self-dependency (src subpass 0 → dst subpass 0, three
variants) and **no `VK_SUBPASS_EXTERNAL` dependency**
(`render_target.rs:1785-1815`, with the rationale at `:944-956`). PanVK lowers the
input-attachment load to a **tile read**, not a read of the resolved image:
`nir_load_tile_res_pan` for read-only, `nir_load_tile_pan` for read-write, and the
same for Z/S (`panvk_vX_nir_lower_input_attachment_loads.c:145-160, 189`). `SOURCE
CONFIRMED`.

Therefore:

- A fetch reads live tile memory. Whatever preload did to the tile is what the fetch
  sees before any draw writes it.
- If preload is `NONE` (a `DONT_CARE`+store pass), the first fetch in a tile reads
  unloaded tile memory, while the second primitive in the same tile can read what the
  first wrote. This can produce a **primitive-order-dependent** difference, including a
  seam along a quad diagonal, if the client's shader feeds the fetched value back.
- No external dependency means the pass cannot wait on earlier work through the
  render-pass dependency itself; ordering comes from the explicit
  `Barrier::everything()` recorded between instances and from the tracker elsewhere
  (`render/executions.rs:3290-3293`, `render.rs:5310-5313`). `SOURCE CONFIRMED`.

## 5. Attachment contents on the first frame

Three storage classes behave differently. `SOURCE CONFIRMED`.

| Class | Initial content | Evidence |
|---|---|---|
| IOSurface (presented drawable) | Whatever the surface held; the runtime does not zero imports | `NativeBridgeMetalFrontend.mm:307-331` (import by surface ID/base/bytes, `nmb_buffer_new_no_copy_releasing`), no clear |
| Runtime image (`Private`/`Shared`/`Managed`) | Zero, or Metal's pattern; written by the first submission that may reach it | `texture/first_move.rs:13-24, 297-311`; `recycle.rs:23-35` |
| Recycled runtime image | Zero/pattern again, never the prior owner's pixels; handed over `UNDEFINED` | `recycle.rs:23-39` |

The key asymmetry: **a `LOAD` on a runtime image is guaranteed to read zero (or a
pattern) if that image was never written, and a `LOAD` on a recycled image reads zero
even though a previous frame used the same memory.** An IOSurface `LOAD` reads the
surface's real prior bytes.

Recycling excludes shareable (IOSurface) textures, heap textures, host-page-backed
images and storage/stand-in images (`recycle.rs:36-39`), so the presented surface is
never recycled. The risk is confined to internal attachments.

## 6. Captured passes

`X9 GRAPH PASS` records, per draw, the requested size and each color attachment as
`cN=<traceID>/<pixelFormat>/<WxH>/m<level>/s<slice>/load<action>/store<action>/resolve<id>`
(`NativeBridgeMetalFrontend.mm:1171-1176`). `MTLLoadAction`: `0`=DontCare, `1`=Load,
`2`=Clear. `MTLStoreAction`: `0`=DontCare, `1`=Store.

### 6.1 Folder run (rev 211, `.local/analysis-blur211/runtime.raw.log`)

- 90 captured draws total. Attachment load/store combinations:
  `load0/store0` ×90, `load1/store1` ×37, `load2/store1` ×53 — zero `load0/store1`.
- Folder event at `[48.200]`; the first blur draw is `[48.933]` id=85,
  `vs=downsample_blur_vert_lph fs=single_pass_blur_3_lph count=6`,
  `requested=316x692`, `c0=588/80/316x692/m0/s0/load2/store1`, `c1=589/.../load0/store0`.
  A `Clear`+`Store` blur target with an unused `DontCare`/`DontCare` companion.
- Drawable-sized passes (`requested` within 6 px of the panel) were captured in two
  groups: 10 at `34.726–35.257` (`1256x2760`) and 7 at `42.914–45.408`
  (`1258x2762`/`1262x2766`), i.e. **before** the folder opened at `[48.200]`. All use
  `c0 load1/store1` plus `c1 load0/store0`; only ids 75, 76 and 81 at `42.9–44.4 s`
  use `c0 load2/store1`. No panel-sized colour-0 pass uses `Clear` at or near the
  folder open. `CAPTURE`.
- `X9 GRAPH PASS` records texture IDs, not surface IDs, so which of these textures is
  the presented IOSurface is not established by this log.
- `X9 FRAME source` samples at `[43.790]` (seq 120) and `[51.141]` (seq 240) both
  report `nonblack=1024 alpha=1024` on a 1024-sample grid, i.e. the 32×32 grid samples
  saw no black or alpha-zero region around the folder open in this run.

`CAPTURE`. The draw log stops early because `traceGraph` logs at most six draws per
vertex/fragment pair unless a pixel window is selected
(`X9RenderGraphCapture.inc:21-25`); the window composite hit that cap during startup.
**There is no draw-level capture at the moment of the folder transition.**

### 6.2 Swipe run (rev 212, `.local/analysis-blur212/runtime-latest.raw.log`)

- 142 captured draws. Combinations: `load0/store0` ×142, `load1/store1` ×68,
  `load2/store1` ×74 — again zero `load0/store1`.
- Swipe touch down `[151.390]`, up `[151.604]`; blur passes at `[151.703]` id=102
  (163×351) and id=103 (316×692), both `load2/store1`; later repeats at `[151.803]`
  id=107/109 switch to `load1/store1` on the same textures (`tex 92`, `96`), i.e. the
  same target is cleared once and then loaded for later passes.
- 43 panel-sized passes, in three groups:
  - 30 at `33.741–35.875` (`1256x2760`), all `c0 load1/store1`;
  - 6 at `40.013–40.016` (`1256x2760`, ids 96–101), all `c0 load1/store1`;
  - 7 at `157.652–162.335` (`1258x2762`/`1262x2766`, ids 134–142), of which six are
    `c0 load2/store1` blur intermediates (134/135/138/139/140/141) and id 142 is
    `c0 load1/store1`.
  None is at the swipe; all are startup or later blur recomputation. `CAPTURE`.

### 6.3 The `attachment_clear` helper does not zero colour 0

Both runs contain a 4-vertex `path_blit_vert_lph` / `attachment_clear_frag_lph` draw
inside a panel-sized `load1/store1` pass (6× in rev 211, 8× in rev 212; e.g. rev 212
id=96 at `[40.013]`, c0 tex 20 / c1 tex 21). The shader returns `zeroinitializer` for
both declared outputs (`attachment_clear_frag_lph.ll:9-11, 38-39`). Its pipeline
descriptor, as logged by `X9 GRAPH BLEND`
(`NativeBridgeMetalFrontend.mm:1818`), has `c0 mask0` and `c1 mask15` on every
occurrence. `MTLColorWriteMask` 0 is `None`, so the draw **does not write colour
attachment 0** — the panel-sized target it is bound with — and writes zeros to colour
attachment 1, whose pass state is `DontCare`/`DontCare`. `CAPTURE`, with the enum
meaning `SOURCE CONFIRMED`.

Consequence: no captured draw or pass is a full-surface zeroing of the panel-sized
colour 0 attachment. Earlier text in this document describing "the drawable's clear as
a draw" is corrected by this subsection. Which attachment is later presented is not
instrumented, so the intent of the asymmetric write mask is `UNKNOWN`.

`CAPTURE`. No per-pixel evidence that the swipe repainted the previously corrupted
region exists in this log.

## 7. Candidate mechanisms, ranked

1. **IOSurface `LOAD` on an unpainted first frame** — `SOURCE CONFIRMED` mechanism,
   `LIKELY` relevance. Every captured panel-sized pass is `Load`+`Store`, and no captured
   draw or pass zeroes the panel-sized colour 0 target (§2.2, §5, §6.3). Pixels the
   frame's draws do not cover show whatever the surface already held. A later
   recomposition would heal them. This is the simplest explanation for "transient, heals
   after interaction", and it does not require any tile-level anomaly.
2. **`DontCare` rewrite over viewport∩scissor tiles** — `SOURCE CONFIRMED` mechanism,
   relevance `UNKNOWN`. It can zero whole 32×32 tiles rather than only primitive-covered
   pixels. Not exercised by any captured pass. Would produce rectilinear, tile-aligned
   regions, and only inside the draw's scissor.
3. **Framebuffer fetch of un-preloaded tile memory** — `SOURCE CONFIRMED` mechanism,
   relevance `UNKNOWN`. If a fetched attachment is `DONT_CARE`-loaded and the pass
   stores, tiles with no preload return undefined tile memory to the first primitive.
   The effect is primitive-order dependent and can look like a diagonal seam.
4. **Recycled internal image read by `LOAD`** — `SOURCE CONFIRMED` mechanism, `LIKELY`
   limited to internal attachments. Always reads zero, never prior-frame pixels.
5. **`DontCare` store discarding content** — effectively **not a mechanism** on PanVK:
   the runtime emits `STORE` because the driver fact is absent (§2.1).
6. **HSR/FPK dropping a triangle** — **not supported**: the X9 patches only *remove*
   culling relative to upstream (`04-panvk-hsr-cull-all-rts.patch:13-19, 133-140`;
   `07-panvk-fpk-write-masks.patch:29-31, 117-125`). They cannot turn a drawn triangle
   into a missing one.

## 8. Unavailable instrumentation

- No captured draw at the folder/App Library/Control Center transition; the graph
  capture caps at six draws per shader pair and starts at the first blur draw.
- `X9 GRAPH PASS` gives attachment identity and load/store, but not the pass's scissor,
  framebuffer-fetch use, tile preload decision, fence/dependency, or cache operations.
- `X9 GRAPH BLEND` gives the pipeline's write mask, but not which attachment is
  display-bound; `X9 GRAPH PASS` records texture IDs, not IOSurface IDs, so a
  panel-sized pass cannot be tied to the presented surface from the log alone.
- `X9 FRAME STATE` gives viewport/scissor per draw, but only under frame capture, and
  multi-viewport/scissor arrays are not traced
  (`NativeMetalAPISupplement.inc:138-147`).
- No CA damage/update region is emitted or consumed anywhere in the stack (§
  `damage-tracking-audit.md`).
- No pixel dump of the presented surface at the transition; the only pixel evidence is
  the CC screenshot, which is opaque RGBA (alpha 255 everywhere) and therefore cannot
  show buffer transparency. The CC comparison is in
  `control-center-vs-folder-rendering.md`.

## 9. Conclusion

The load/store mapping is conservative on the store side and faithful on the load
side except for the measured `DontCare` rewrite. No captured pass or draw zeroes the
panel-sized colour 0 target: the passes load and store it, and the `attachment_clear`
pipeline bound inside one of them has write mask 0 on that attachment (§6.3). The one
mechanism consistent with *all* observed behaviour and needing no unverified driver
behaviour is therefore a `LOAD` of a panel-sized surface whose contents were never
painted by the frame: pixels outside the frame's covered geometry inherit the surface's
prior bytes, and a later recomposition overwrites them.

That is a hypothesis, not an established cause. It cannot be confirmed from existing
logs because no draw was captured at the transition and no attachment was traced to
the presented surface. The experiments in `first-frame-experiment-plan.md` are designed
to separate it from the tile-level and framebuffer-fetch mechanisms.
