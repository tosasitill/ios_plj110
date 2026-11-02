# Damage Tracking and Partial Rendering Audit

- Task 10 / investigation B. Read-only source audit; no production code was modified.
- Audit date: 2026-11-02. Branch `feat/G1Ultra`, HEAD `646b7cd` (runtime/Mesa sources
  unchanged from `681f64e`; the working tree also holds a rev 232 manifest and a
  frontend histogram edit that are outside this audit).
- Sources: `metal2vulkan-pan205-source` (runtime), X9 Mesa `mesa-source` (PanVK v14 +
  panfrost), `x9/metal/General` (Metal frontend), `x9/kernel` (framebuffer sampler).
- Evidence tags: `SOURCE CONFIRMED` = read at the cited line; `CAPTURE` = read from a
  local runtime log; `INFERENCE` = follows from source but not measured; `UNKNOWN` =
  needs instrumentation that does not exist.

## 1. Question

The suspected behaviour is that only part of a surface is redrawn after a transition,
so a region painted with wrong contents survives until something overwrites it. This
document establishes, from source and from the two saved captures, what "partial"
could mean at each layer, and whether the previously corrupted region is redrawn after
interaction.

Short answer: **no layer in the audited stack has a damage region.** There is no CA
update region, no `VkPresentRegionKHR` consumer, no dirty-tile mask and no
damage-driven pass split. The only region controls are per-draw viewport/scissor, and
the only incremental rendering in PanVK is the tiler-OOM exception handler. The saved
captures therefore cannot show that a corrupted region was redrawn — they contain no
draw at the transition moments.

## 2. Is there a damage/update region anywhere?

| Layer | Damage region? | Evidence |
|---|---|---|
| CoreAnimation / backboardd | Not exposed to this stack | No `presentRegion`, `damage`, `updateRegion` or `setNeedsDisplayInRect` handling in `x9/metal/General`; the frontend records surfaces (`_surfaces`) but never a region (`NativeBridgeMetalFrontend.mm:1142-1184`) `SOURCE CONFIRMED` |
| X9 Metal frontend → runtime | None | `setScissorRect:` forwards one rectangle; there is no region API in the MTL surface (`NativeBridgeMetalFrontend.mm:900-909`) `SOURCE CONFIRMED` |
| metal2vulkan runtime | None | The Vulkan render area is the smallest attachment extent (`render_pass.rs:1392-1427`, `render.rs:5025-5029`); no region enters `PassLayout` `SOURCE CONFIRMED` |
| Mesa WSI (Metal backend) | Accepted, then **discarded** | `wsi_metal_swapchain_queue_present(..., const VkPresentRegionKHR *damage)` never reads `damage` (`wsi_common_metal.c:688-723`); generic WSI only forwards it (`wsi_common.c:2901-2907`) `SOURCE CONFIRMED` |
| PanVK | None | No software dirty-tile mask; incremental rendering exists only for tiler OOM (§5) `SOURCE CONFIRMED` |
| X9 kernel display | None | The panel copy samples a 32×32 grid of points and composites whole layers (`SweetFramebuffer.cpp:1154-1168, 1847-1860`) `SOURCE CONFIRMED` |

Two supporting facts:

- `VK_KHR_incremental_present` is advertised by PanVK
  (`panvk_vX_physical_device.c:125`), but that only means `wsi_common_queue_present`
  may receive a region. The X9 path has no consumer, and the Metal WSI backend
  ignores the pointer. Advertising the extension therefore does not create partial
  presentation. `SOURCE CONFIRMED`.
- The runtime has a debug-only pixel window (`tracePixelSelect`,
  `X9RenderGraphCapture.inc:16`). It selects what is logged, never what is rendered.

## 3. What per-draw region control exists

### 3.1 Viewport and scissor

Metal's scissor is clipped to the viewport's pixels; the runtime converts the pair to
one Vulkan scissor per viewport (`render_state.rs:39-43, 919-953`). Scissors past the
render area are clipped to it. `SOURCE CONFIRMED`.

Consequences:

- A scissor can only make a draw cover **less** than the pass. It can never make a pass
  skip a tile, skip a load, or skip a store. The pass's render area stays the full
  attachment.
- Therefore a scissor-limited draw leaves every texel it does not cover at whatever
  the load action put there — which is the client's own contract, not a runtime defect.

PanVK packs the scissor into the draw descriptor and, in the CSF path, reads
**`scissors[0]` only** (`panvk_vX_cmd_draw.c:870-906`, `XXX: Switch scissor_array_enable
to true`). A Metal draw that legitimately uses per-viewport scissors beyond index 0
would therefore be clipped by the first rectangle only. That over-draws relative to
Metal; it does not under-draw. `SOURCE CONFIRMED`.

### 3.2 Render area

The Metal render pass descriptor has no render-area property; its area is the
attachment size. The runtime reproduces that: `area()` takes the minimum extent across
attachments, clamped by the descriptor's target size (`render_pass.rs:1392-1427`), and
every pass records it in full (`render.rs:5025-5029, 5239`; `render/pass_steps.rs:217,
374`). `SOURCE CONFIRMED`.

`PassReach::of` distinguishes three cases — no tile reached, every tile reached, or the
tiles the draws' viewport∩scissor reach (`clear_reach.rs:152-180`). That distinction
exists **only** to bound the `DontCare`→`Clear` rewrite; it does not change the render
area. `SOURCE CONFIRMED`.

## 4. Partial rendering in the runtime

The only mechanism that renders into a sub-rectangle of an attachment is the
`DontCare` rewrite: when a color attachment is loaded `DontCare`, is not memoryless and
is stored, the runtime turns the load into `Clear` and the clear reaches only the whole
tiles the draws' scissors reach (`render.rs:4289-4326`, `clear_reach.rs:204-268`).

Reachability in the two captures: **zero passes** use `DontCare` load together with
`Store`. Every captured attachment line is `load0/store0`, `load1/store1` or
`load2/store1` (§6). The rewrite was not exercised by any captured draw. `CAPTURE`.

Everything else is full-attachment:

- Clears ahead of a pass, shadow copies and imageblock work are planned per pass and
  per attachment, not per damage rect (`render.rs:4690-4738`).
- Instance splitting repeats the full render area; the only difference between
  instances is load/store (§`first-frame-rendering-audit.md` §2.2).

## 5. PanVK: tile load/store and incremental rendering

- The pass always has a render area; tiles outside it are never in the polygon list.
  Tiles inside it are loaded according to the load op (`.preload`), stored according to
  the store op (`.discard = !store`), and clean-tile write-back is enabled only for a
  `CLEAR` load or AFBC (`pan_fb.c:326-333, 396-404, 421-434`). `SOURCE CONFIRMED`.
- PanVK's "incremental rendering" is **not** damage-based. It is the exception handler
  registered on the fragment subqueue for a tiler-heap OOM
  (`panvk_vX_gpu_queue.c:470-478`). It re-issues the fragment run for the chunks that
  are still in the heap, then renders the remaining tiles under a second FBD whose
  loads are `spill.load` (`pan_fb_load_iview` = `LOAD_IMAGE`, `pan_fb.h:272-279`;
  `panvk_vX_exception_handler.c:235-330`). Each tile is still rendered once and each
  tile still follows the pass's load/store contract. `SOURCE CONFIRMED`.
- There is no software dirty-tile mask and no consumer of one. `SOURCE CONFIRMED`.

## 6. Captured evidence

Method: parse `X9 GRAPH PASS` from the two logs. `MTLLoadAction` `0`=DontCare,
`1`=Load, `2`=Clear; `MTLStoreAction` `0`=DontCare, `1`=Store
(`NativeBridgeMetalFrontend.mm:1171-1176`). The capture caps at six draws per
vertex/fragment pair unless a pixel window is selected
(`X9RenderGraphCapture.inc:25`), and starts at the first blur draw (`:21`).

### 6.1 Folder run, rev 211 (`.local/analysis-blur211/runtime.raw.log`)

- 90 draws, `34.686–52.596 s`. Attachment combinations across all color attachments:
  `load0/store0` ×90, `load1/store1` ×37, `load2/store1` ×53, `load0/store1` ×0.
- Drawable-sized (≥1200×2700) color-0 passes: 18.
  - 10 at `34.726–35.257 s`, `requested=1256x2760`, `load1/store1`.
  - 7 at `42.914–45.408 s`, `1258x2762`/`1262x2766`, mixed `load1` and `load2`.
- Folder event `[48.200]`; first blur draw `[48.933]` id=85, `316x692`,
  `load2/store1` on `tex=588` with `c1 load0/store0`.
- **No drawable-sized pass was captured after `45.408 s`.** The cap stopped the
  drawable shader pairs during startup.

### 6.2 Swipe run, rev 212 (`.local/analysis-blur212/runtime-latest.raw.log`)

- 142 draws, `33.686–162.335 s`. Combinations: `load0/store0` ×142, `load1/store1` ×68,
  `load2/store1` ×74, `load0/store1` ×0.
- Swipe touch down `[151.390]`, up `[151.604]`. Every captured draw between the swipe
  and `151.861` is the folder's blur subtree:

  | id | time | vs/fs | requested | c0 |
  |---|---|---|---|---|
  | 102 | 151.703 | `downsample_blur_vert_lph` / `narrow_blur_15_frag_lph` | 163×351 | tex 96, `load2/store1` |
  | 103 | 151.707 | same | 316×692 | tex 92, `load2/store1` |
  | 107 | 151.803 | `narrow_blur_23_frag_lph` | 316×692 | tex 92, `load1/store1` |
  | 109 | 151.861 | same | 316×692 | tex 92, `load1/store1` |

  The blur target is cleared once (`load2`) and then **loaded** for the repeats. It is
  never re-cleared on the later passes.
- Drawable-sized color-0 passes: 43. 36 are `1256x2760`/`Load`+`Store` at
  `33.741–40.016 s` — startup and the first folder composition. The remaining 7 are
  `1258x2762`/`1262x2766` at `157.652–162.335 s`, 6 s after the swipe, and 6 of those 7
  are `Clear`+`Store`; the last (`162.335`, tex 462) is `Load`+`Store`.
- No drawable-sized pass exists in the capture between `40.016 s` and `157.652 s`.

`CAPTURE`. Two consequences:

1. The swipe's own draws do not include a full-panel pass. The capture cannot show a
   full redraw of the corrupted region at the moment of interaction.
2. What the swipe *does* show is **partial re-rendering without clearing**: after the
   first `Clear`, the same blur target is re-entered with `Load`. Whatever the first
   pass left outside its scissor survives the repeats.

### 6.3 The `attachment_clear` draw does not zero colour 0

At `[40.013]` rev 212, id=96 is `vs=path_blit_vert_lph fs=attachment_clear_frag_lph`,
`type=4` (triangle strip), `count=4`, on the 1256×2760 target `tex=20`,
`load1/store1`. The fragment function returns zeros for both color outputs
(`.local/analysis-blur211/attachment_clear_frag_lph.ll:9-11`), and the vertex function
builds the quad from `vertex_id & 1` and `vertex_id & 2` over an `AxisAlignedRect`
(`path_blit_vert_lph.ll:11-51`). `CAPTURE` + `SOURCE CONFIRMED`.

The pipeline state for this draw, however, is `c0 mask0` / `c1 mask15`
(`X9 GRAPH BLEND`, same id). `MTLColorWriteMask` 0 is `None`, so **the draw does not
write colour attachment 0**; it writes zeros only to colour attachment 1, whose pass
state is `DontCare`/`DontCare`. This holds for all six rev 211 and eight rev 212
occurrences. `CAPTURE`, enum meaning `SOURCE CONFIRMED`.

Consequences, stated at the level the evidence supports:

1. No captured pass or draw demonstrates a full-surface zeroing of the panel-sized
   colour 0 target. The earlier reading of this section — "the compositor clears the
   presented surface by drawing a two-triangle quad" — is **not supported** by the
   pipeline state and is withdrawn.
2. Which attachment is presented is not instrumented (`X9 GRAPH PASS` logs texture
   IDs, not IOSurface IDs), so the asymmetric write mask is `UNKNOWN` in intent.
3. The quad's geometry (two triangles sharing the bottom-left → top-right diagonal)
   still matches the measured Control Center seam orientation
   (`control-center-vs-folder-rendering.md` §3.2). That is a geometry match with the
   card quads generally, not evidence that this particular pipeline produced it.

For the folder first frame the net effect is the same or stronger: the panel-sized
target is `Load`+`Store` and nothing captured zeroes it, so pixels outside covered
geometry inherit whatever the target held.

## 7. Does a swipe cause a full or a partial redraw?

From the captured data: **not determinable, and the captured draws suggest the
opposite for the blur subtree.**

- The runtime has no API that would make a swipe preferable to a full repaint: every
  pass it is given renders its full render area (§3.2).
- What a swipe changes is the *set of draws CoreAnimation issues* and their scissors.
  In the capture, the post-swipe draws are the folder's blur subtree only
  (`163×351`, `316×692`), re-rendered with `Load` after one `Clear`.
- A full-panel member of that work (the 1258×2762 / 1262×2766 passes) appears only
  6 s later. The capture gives no causal link to the gesture; it may be a later
  animation or a lock/home transition.

The observation "the artifact heals on scroll" is therefore consistent with two
different histories, and the existing logs cannot separate them:

1. the next full-surface composition covers the region (a later draw, not the swipe),
   or
2. the swipe's own re-blur with `Load` writes over the region (partial re-render, no
   clear).

## 8. Can the corrupted region be shown to be redrawn? No, not with existing telemetry

Missing instrumentation, each with the reason it cannot be substituted:

- **No per-draw pixel footprint.** `X9 FRAME DRAW` records primitive type and count,
  not the pixels covered (`X9FrameCapture.inc:344-367`), and it did not run in either
  capture (no `X9 FRAME DRAW` rows). Vertex positions exist only as logged hex under
  frame capture.
- **No surface diff across frames.** The kernel sampler reads 1024 points on a 32×32
  lattice (`SweetFramebuffer.cpp:1160-1166`). A triangular region of tens of thousands
  of pixels can fall entirely between samples; `nonblack=1024 alpha=1024` does not
  mean the frame was correct.
- **Probe gating.** `X9 FRAME destination` is emitted only while `x9_probe` is set,
  which is enabled by a black/non-black transition (`SweetFramebuffer.cpp:1876-1889`).
  Neither capture has destination samples at the folder or swipe event.
- **No damage rects** exist to compare against (§2).
- **No tile write mask.** PanVK does not expose which tiles a pass dirtied; the
  clean-tile bit is internal to the FBD (`pan_fb.c:421-434`).

An experiment that answers this question needs, at minimum: the draw's screen-space
footprint (a CPU-side vertex transform is enough) plus a full-surface readback of the
presented surface before and after the interaction, compared per pixel.

## 9. Conclusions

| Statement | Status |
|---|---|
| No damage region is produced or consumed anywhere in the stack | `SOURCE CONFIRMED` |
| The Metal WSI backend receives and ignores `VkPresentRegionKHR` | `SOURCE CONFIRMED` |
| Pass render area is always the full attachment; scissor only shrinks draws | `SOURCE CONFIRMED` |
| `DontCare`+`Store` is the only sub-rectangle path; unused by captured draws | `SOURCE CONFIRMED` / `CAPTURE` |
| PanVK incremental rendering is tiler-OOM recovery, not damage | `SOURCE CONFIRMED` |
| Post-swipe blur passes re-use the prior target with `Load`, not `Clear` | `CAPTURE` |
| The swipe's captured draws contain no full-panel pass | `CAPTURE` |
| The previously corrupted region is redrawn after interaction | `UNKNOWN` — not observable with the current probes |
| The panel-sized colour-0 target is never zeroed by a captured pass or draw | `CAPTURE` |
| The `attachment_clear` helper does not write colour 0 (`writeMask=0`) | `CAPTURE` + `SOURCE CONFIRMED` |
| A missing/partial triangle in any quad leaves a triangular stale region | `INFERENCE`; not tied to the captured clear draw |
