# Control Center vs folder/App Library rendering — comparative audit

- Task 10 / investigation E. Read-only source audit; no production code was modified.
- Audit date: 2026-11-02. Branch `feat/G1Ultra`, HEAD `646b7cd` (runtime/Mesa sources
  unchanged from `681f64e`; the working tree also holds a rev 232 manifest and a
  frontend histogram edit that are outside this audit).
- Evidence tags: `SOURCE CONFIRMED` = read at the cited line; `CAPTURE` = read from a
  local runtime log; `SCREENSHOT` = measured from
  `.local/analysis-blur-photo/control-center.png`; `LIKELY` = implied, not measured;
  `UNKNOWN` = needs instrumentation that does not exist.
- Capture logs: rev 211 `.local/analysis-blur211/runtime.raw.log` (folder opened at
  `[48.200]`), rev 212 `.local/analysis-blur212/runtime-latest.raw.log` (swipe touch
  down `[151.390]`, up `[151.604]`). **There is no Control Center runtime log.** The
  Control Center comparison below is a screenshot analysis only; nothing about its
  pass structure is captured.

## 1. What each surface can be compared on

| Field | Folder / App Library | Control Center |
|---|---|---|
| Runtime log | rev 211 + rev 212, `X9 GRAPH` + `X9 FRAME` | **not available** |
| Render-target identity per draw | partial: texture trace ID, format, size, mip/slice; no base address | not available |
| Attachment load/store | captured for the 90 / 142 draws before the per-pair cap | not available |
| Attachment buffer identity across frames | trace ID + kernel `source id=` (2/3); no address | not available |
| Scissor / viewport | only under frame capture (`X9 FRAME STATE`); absent from both logs | not available |
| Damage / present region | none exists in the stack (`damage-tracking-audit.md` §2) | none exists |
| Framebuffer fetch use | no trace field; inferred only from pass shape | not available |
| Tile preload / store decision | no trace field; `SOURCE CONFIRMED` code path only | not available |
| GPU fence / dependency | kernel submit/complete rows, no per-draw fence | not available |
| Cache operations | kernel-level only (`SweetX9RenderEngine.inc:269-481`) | not available |
| Per-pixel result | `X9 FRAME` 1024-point sampler only; no dump | one screenshot, opaque RGBA |

The rest of this document therefore compares **captured pass data for the folder path**
against **measured pixel data for Control Center**, and is explicit where the two cannot
be placed on the same axis.

## 2. Folder / App Library: what the captures show

### 2.1 All drawable-sized colour-0 passes are `Load` + `Store`

Counting the passes whose `requested` size is within 6 px of the panel
(`1256x2760`, `1258x2762`, `1262x2766`):

| Run | drawable-sized passes | times | colour-0 actions |
|---|---|---|---|
| rev 211 (folder) | 17 | 34.726–35.257 (10×), 42.914–45.408 (7×) | 14× `load1/store1`; 3× `load2/store1` (ids 75, 76, 81 — all at 42.9–44.4 s, not at the folder open at 48.2 s) |
| rev 212 (swipe) | 43 | 33.741–35.875 (30×), 40.013–40.016 (6×), 157.652–162.335 (7×) | 37× `load1/store1`; 6× `load2/store1` (ids 134/135/138/139/140/141, all blur intermediates at 157.6–162.3 s) |

`CAPTURE`. The colour-0 attachment of a drawable-sized pass is **never** given a
`Clear` load action in either run. What the capture does contain is a draw pipeline
named `attachment_clear` (vertex `path_blit_vert_lph`, fragment
`attachment_clear_frag_lph`, 4-vertex triangle strip), recorded 6× in rev 211 and 8×
in rev 212, always inside a drawable-sized `load1/store1` pass (e.g. rev 212 id=96 at
`[40.013]`, tex 20 / tex 21).

### 2.2 The `attachment_clear` pipeline writes only colour attachment 1

`X9 GRAPH BLEND` logs the pipeline descriptor's `MTLRenderPipelineColorAttachmentDescriptor.writeMask`
(`NativeBridgeMetalFrontend.mm:1818`). For every `attachment_clear` draw in both runs:

```
BLEND c0=fmt80/mask0/en0/...      BLEND c1=fmt80/mask15/en0/...
```

`MTLColorWriteMask` 0 is `None`; 15 is all four channels. `CAPTURE`, with field
meaning `SOURCE CONFIRMED` (Metal enum). The fragment shader itself returns
`zeroinitializer` for both outputs (`attachment_clear_frag_lph.ll:9-11`, both outputs
declared at `:38-39`).

Under Metal semantics the captured draw therefore **does not write colour attachment 0**
(the drawable-sized target it is bound with), and writes zeros to colour attachment 1,
whose pass state is `load0/store0` (`DontCare`/`DontCare`). On PanVK the `DontCare`
store still stores (`first-frame-rendering-audit.md` §2.1), so attachment 1 persists;
attachment 0 is unchanged by this draw.

This is recorded as a capture fact. Its intent is `UNKNOWN`: the capture does not say
which of the two attachments is later composited or presented, and no CC/folder
composite was captured after the transition. It does establish that **no captured draw
is a full-surface zeroing of colour 0**; the earlier claim that "the drawable's clear is
a draw" is too strong and is corrected in `first-frame-rendering-audit.md` §6.2.

### 2.3 The one place a `Clear` is used is the blur chain

At the swipe, the blur tree re-enters with `Clear` and then switches to `Load` on the
same targets:

- `[157.652]` id=134, `downsample_blur_vert_lph` / `narrow_blur_7_frag_lph`,
  `requested=1262x2766`, c0 tex 395 `load2/store1`;
- `[157.653]` id=135 (tex 393, `1258x2762`, `load2/store1`);
- `[160.718]` id=138 (tex 393, `single_pass_blur_3_lph`, `load2/store1`);
- `[161.228/161.229]` ids 139/140 (tex 464/462, `narrow_blur_11_frag_lph`,
  `load2/store1`);
- `[162.334/162.335]` id=141 (tex 504, `load2/store1`) then id=142 (tex 462,
  **`load1/store1`**) sampling tex 504.

`CAPTURE`. So the captured healing-side work is: the *blur intermediates* are cleared
once when the blur is recomputed, later passes load them, and the composite of the
blurred result is not captured (per-pair cap of six draws,
`X9RenderGraphCapture.inc:21-25`). Whether the composite redraws the previously
corrupted pixels cannot be shown from this log.

### 2.4 Attachment lifecycle on the presented surface

`X9 FRAME` gives only the kernel's 32×32 lattice sample of the surface, not per-pixel
coverage. In rev 211 both samples around the folder open (`[43.790]` seq 120 and
`[51.141]` seq 240) report `nonblack=1024 alpha=1024`. In rev 212 the sample at
`[157.556]` (seq 120, surface id=3) is also `nonblack=1024 alpha=1024`; at
`[163.339]` seq 212 reports `nonblack=0 alpha=0` on mask `80000007` (a fully
transparent frame, after the captured interval). `CAPTURE`. The lattice is too coarse
to see a triangular region of tens of pixels, and it cannot show whether the region was
redrawn.

## 3. Control Center: measured from the screenshot only

Source image: `.local/analysis-blur-photo/control-center.png`, 812×1068 RGBA. Alpha is
255 for every pixel, so the screenshot is an opaque composite; it cannot show a
transparent buffer, only the composed result.

### 3.1 One diagonal per rounded card

Each rounded card is divided by a single straight edge running bottom-left → top-right,
i.e. along constant `x + y`. Measured by comparing mean RGB in the two half-planes
inside each card, excluding a ±5 px band on the assumed diagonal and near-white icon
pixels (`SCREENSHOT`):

| Card | `x+y` | upper-left mean RGB | lower-right mean RGB | L1 difference |
|---|---|---|---|---|
| Connectivity | 455 | 142.1, 130.9, 107.7 | 136.7, 117.0, 127.7 | 39.3 |
| Music | 845 | 107.6, 109.7, 97.6 | 114.3, 114.4, 108.1 | 22.0 |
| Lock | 635 | 97.6, 110.8, 105.7 | 100.1, 112.6, 109.1 | 7.6 |
| Notifications | 830 | 81.9, 84.6, 85.3 | 77.8, 79.7, 56.2 | 38.1 |
| Alarm | 1035 | 114.7, 121.5, 96.6 | 116.4, 125.7, 98.5 | 7.9 |
| Stopwatch | 1165 | 107.6, 116.9, 90.7 | 118.9, 125.1, 100.1 | 28.9 |
| Calculator | 1415 | 65.5, 74.5, 60.2 | 78.7, 84.8, 64.8 | 28.1 |
| Camera | 1580 | 124.8, 128.2, 108.4 | 123.7, 130.5, 108.6 | 3.6 |

The difference is not a constant overlay and does not have a constant sign: on
Connectivity the upper-left is redder and the lower-right more blue; on Music,
Stopwatch and Calculator the lower-right is brighter; on Camera it is nearly invisible.
That is the signature of **two halves of one quad being shaded differently**, not of a
stale region and not of a half-plane overlay applied globally.

The background also shows long diagonal bands with the same orientation crossing the
wallpaper and the card containers (`SCREENSHOT`, qualitative; the wallpaper has no
separate capture). Whether these are the same artifact on larger quads cannot be
decided from this image.

### 3.2 The orientation is the two-triangle strip diagonal

`path_blit_vert_lph` builds a 4-vertex triangle strip from an axis-aligned rect
(`.local/analysis-blur211/path_blit_vert_lph.ll:11-51`): vertices are
`(min.x,min.y)`, `(max.x,min.y)`, `(min.x,max.y)`, `(max.x,max.y)`. Strip triangles are
`TL, TR, BL` and `TR, BR, BL`; their shared edge is `BL → TR`, which is exactly
constant `x + y`. The measured seam orientation therefore matches the quad's own
triangulation, one diagonal per quad, at the quad's own offset (`CAPTURE` of the shader
IR + `SCREENSHOT` geometry).

### 3.3 The seam is not tile-aligned

PanVK's tile grid is 32×32 at 1–2 samples (32×16 at 4 samples) and axis-aligned
(`clear_reach.rs:21-22`). A constant-`x+y` diagonal crosses a moving set of tiles and
coincides with a tile boundary only at isolated points. None of the measured seam
offsets is an axis-aligned rectangle edge. The CC artifact is therefore **not** produced
by the `DontCare`-load tile rewrite, by a tile preload boundary, or by a partial-tile
store.

### 3.4 Persistent vs transient

The CC seam is deterministic per card and present in a still screenshot taken after the
interface settled. A stale-attachment or reuse race would vary between frames and
between cards with the same geometry; here every card has the artifact at its own
constant offset and no card is clean. That points to something reproduced by every
frame of that draw — a property of how the quad is shaded or of its two triangles — not
to cross-frame residue (`cross-frame-attachment-audit.md` §5 ranks reuse as a moving
tear, not a fixed seam).

The folder/App Library artifact is reported to disappear after interaction. The
captures show that the interaction recomputes the blur tree with `Clear` on the
intermediates and re-composites (§2.3), which is the only healing path the logs contain;
they do not show the composite. So "the swipe redraws the corrupted region" remains
`LIKELY`, not proven.

## 4. Side-by-side conclusions

| Question | Folder / App Library | Control Center | Evidence |
|---|---|---|---|
| Artifact shape | triangular regions | one diagonal per card, same −45° orientation | reported vs `SCREENSHOT` |
| Aligned to 32×32 tiles | unknown; not necessary | **no** | `SCREENSHOT` geometry |
| Aligned to a quad diagonal | plausible | **yes**, matches `path_blit_vert` strip | shader IR + `SCREENSHOT` |
| Persists after interaction | no (recovers) | **yes** | reported; no capture of either composite |
| Explained by cross-frame reuse | no (synchronous submit; display in-use guard) | no (deterministic) | `cross-frame-attachment-audit.md` |
| Explained by `DontCare` tile rewrite | possible but not captured | no (not tile-aligned) | `first-frame-rendering-audit.md` §2.3 |
| Colour-0 target ever cleared | no captured `Clear`; `attachment_clear` writes mask 0 to c0 | unknown (no capture) | `CAPTURE` §2.1–2.2 |
| Shader/half-quad shading difference | unknown (no per-draw pixels) | **consistent with all measurements** | `SCREENSHOT` §3.1 |
| Framebuffer fetch as a cause | possible only for `DontCare`+store passes; none captured | unknown | `first-frame-rendering-audit.md` §4 |

### 4.1 What can be said with confidence

1. The CC artifact is a per-quad, per-triangle shading discontinuity. It is not
   tile-aligned, not a clear-load boundary, and not a cross-frame reuse tear.
   `SCREENSHOT` + shader IR.
2. The folder/App Library path never clears the drawable-sized colour target with a
   `Clear` load action in either capture; the `attachment_clear` helper captured on that
   target has a zero write mask on colour 0, so it does not zero it either. `CAPTURE`.
3. The only `Clear` actions in the captured healing path are on full-panel blur
   intermediates, and the composite that consumes them was not captured. `CAPTURE`.
4. Therefore the difference between "recovers after a swipe" and "stays" is consistent
   with **which layer is recomposited on interaction**, not with two different
   attachment mechanisms: the folder backdrop is recomputed and re-composited; the CC
   cards are recomposited every frame with the same per-quad discontinuity. `LIKELY`.

### 4.2 What cannot be said

- That the swipe redraws the exact corrupted pixels (no per-pixel trace, cap of six
  draws per shader pair, no scissor trace).
- Which colour attachment is displayed in the `attachment_clear` pass (no mapping from
  trace texture to presented surface for that pass).
- Whether the CC halves differ because of interpolation, because the two triangles are
  separate draws, because of a per-vertex attribute, or because of framebuffer-fetch
  ordering. Each is consistent with the measurements; none is proven.

## 5. Instrumentation needed to close the comparison

| Missing datum | Where it would have to be emitted |
|---|---|
| per-draw footprint on the presented surface | vertex positions and scissor in `X9 FRAME STATE`, or a small pixel window at the transition (`X9RenderGraphCapture.inc:21`) |
| which attachment is display-bound | map trace texture ID → IOSurface ID for the pass's c0 when it is presented |
| per-triangle shading inputs | provoking vertex / varying values for `path_blit_vert_lph`, not traced today |
| CC pass structure | an `X9 GRAPH` capture with Control Center open, after raising the per-pair cap or selecting a pixel window |
| redraw proof | a pre/post pixel window over the corrupted region across the swipe |

Until then the comparison supports a per-quad shading explanation for Control Center
and a carried-over-contents explanation for the first folder frame, but it does not
unify them.
