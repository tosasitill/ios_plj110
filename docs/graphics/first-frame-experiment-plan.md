# First-frame experiment plan — four discriminators, design only

- Task 10 / experimental proposals. **Nothing in this document is implemented.** All four
  experiments are designs; the audit itself stayed read-only.
- Audit date: 2026-11-02. Branch `feat/G1Ultra`, HEAD `646b7cd` (runtime/Mesa sources
  unchanged from `681f64e`; the working tree also holds a rev 232 manifest and a
  frontend histogram edit that are outside this audit).
- Source trees cited: `mesa-source` = `.local/x9/work/x9-metal-render/mesa-source`,
  `metal2vulkan-runtime` = `.local/x9/work/x9-metal-render/metal2vulkan-pan205-source/crates/metal2vulkan-runtime`,
  `x9` = `x9/` in this repository.
- Every experiment must be gated by a boot argument or a build cfg so that a device can run
  A/B on one image, and every observation must be stated as *predicted*, not measured.
- Standing device rules apply: the user flashes and tests; no experiment here authorises a
  flash, a partition write, or a production-default change.

## Decision order

The four experiments are not independent alternatives; they are discriminators, and the
cheapest decisive one should run first.

| # | Experiment | Hypothesis it can kill | Cost | Expected result |
|---|---|---|---|---|
| 3 | No recycling | recycled internal image supplies stale content | build cfg only | negative (recycle already excludes surfaces and zeroes reuse) |
| 1 | Clear internal intermediates | a `LOAD` of an internal target with no prior content | ~30 lines + cfg | negative if the artifact is on the presented surface |
| 2 | Clear presented surface on acquire | carried-over bytes in the presented surface | ~40 lines + cfg | positive under the leading hypothesis |
| 4 | Display-side scoped dependency | GPU/display ordering, not contents | kernel patch + cfg | negative (artifact is static, submit is synchronous) |

---

## Experiment 1 — force-clear only intermediate render targets

### Hypothesis tested

The transient triangles are pixels of an **internal** colour attachment (`m2v_*` runtime
image, not an IOSurface) that a pass loads (`LOAD`) before anything has written that
subresource in this submission. On a fresh or recycled image the runtime guarantees zeros
(`texture/first_move.rs`, `recycle.rs`), so this can only produce transparent/black regions,
not stale UI pixels — which is exactly the observed "transparent" description.

### Exact code location

- Mapping point: `metal2vulkan-runtime/src/render_target.rs:374-382` (`load_op`) and
  `:473-489` (`load_at` / `store_at`). The first instance of an encoder keeps the client's
  `LOAD`; this is where a test-only rewrite belongs, so it is visible in the same place as
  the production mapping.
- Planning point for a pre-pass clear: `metal2vulkan-runtime/src/render.rs:4690-4738`
  (the `clears` / `left_out` planning that already turns a `Clear` load into either a
  whole-subresource clear or a lone render pass).

### Design

1. In `load_at`, under `cfg(m2v_clear_internal_first_use)` (or an env read at startup),
   rewrite a first-instance `LoadAction::Load` to `LoadAction::Clear` **only when** the
   target is runtime-owned (no IOSurface import) and the image's last writer is not in the
   current submission. The second condition needs a per-image "written in this submission"
   bit that the runtime does not have today; for a first cut, apply the rewrite to every
   runtime-owned image and accept that legitimate accumulations reset.
2. Clear value: the pass's existing clear value (zero/pattern), not a new colour, so the
   experiment changes *whether* content is loaded, not what a cleared pixel looks like.
3. Log one line per rewritten attachment (trace ID, size, previous load op) so the A/B is
   attributable.

### Expected observation if the hypothesis is correct

The triangular regions stop showing transparent inherited content and instead show the
clear value uniformly, **and** the rest of the frame is unchanged. If the artifact is
instead the per-quad shading seam seen in Control Center
(`control-center-vs-folder-rendering.md` §3), this experiment changes nothing.

### Side effects

- Blur chains that deliberately accumulate across passes on the same internal texture will
  restart from zero on the first pass; visible as darker/boxed blur, not as a crash.
- One extra clear or clear-render-pass per rewritten attachment; measurable as draw-count
  and submit-time increase.
- Memory traffic increases; on a tiler this is tile clears, not full-surface stores.

### Rollback

Remove the cfg/env branch; no data or format changes, so a rebuild in the same revision
restores production behaviour. If the experiment is landed temporarily, keep it behind a
default-off boot argument so the rollback is `boot-arg=false` without a rebuild.

---

## Experiment 2 — force a full clear of the presented surface

### Hypothesis tested

The leading hypothesis is that the presented surface is `LOAD`-ed by the compositor with
bytes from an earlier frame or from a never-painted surface, and that only the pixels the
frame's draws cover are overwritten. Clearing the surface **before the application renders**
separates "inherited bytes" from "something produced during rendering".

### Why "force full redraw" is not directly available

The audit found no partial-redraw mechanism to disable anywhere:

- the Metal WSI accepts and ignores `VkPresentRegionKHR`
  (`mesa-source/src/vulkan/wsi/wsi_common_metal.c:688-723`, argument unused);
- the runtime's render area is always the full attachment
  (`metal2vulkan-runtime/src/render.rs:5025-5029`);
- PanVK's incremental rendering is tiler-OOM recovery only
  (`mesa-source/src/panfrost/vulkan/csf/panvk_cmd_buffer.h:68-73`, `panvk_vX_cmd_draw.c:1833-1894`);
- the kernel composites the whole layer and samples the whole surface
  (`x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp:1154-1168, 1843-1906`).

Forcing the *client* (SpringBoard/backboardd) to redraw is outside this stack. The closest
stack-controlled equivalent is to make every frame start from a known surface, which is
what this experiment does.

### Exact code location

- Acquire path: `mesa-source/src/vulkan/wsi/wsi_common_metal.c`
  `wsi_metal_swapchain_acquire_next_image` (the image is selected and waited for there;
  see the present path at `:693-715` for the image/drawable pairing).
- Alternative if the CPU-copy route is active (`wsi->sw`,
  `wsi_common_metal.c:708-714`): the clear can instead be recorded on the acquired
  `image->base` with a one-shot command buffer before the blit in
  `wsi_common_metal_layer.m:328-357` copies it to the drawable. Both routes end at the
  same observable: the app's first `LOAD` reads zeros.

### Design

1. After acquire succeeds (and after the image's semaphore wait), start a one-shot command
   buffer, `vkCmdClearColorImage` the whole subresource to `{0,0,0,1}` (Metal's default
   transparent-black is **not** used here: alpha 1 makes an unpainted region unmistakable
   in a screenshot), submit, and wait on it before returning the image. A second variant
   with `{0,0,0,0}` distinguishes "transparent" from "black".
2. Gate on a boot argument with three states: off, opaque-zero, transparent-zero.
3. Log the image index and the clear variant once per acquire (rate-limited).

### Expected observation if the hypothesis is correct

The folder/App Library triangles change from "background visible through the region" to a
solid black (or fully transparent) triangle **before any interaction**, and the post-swipe
result is unchanged. If the triangles remain pixel-identical, the surface is fully painted
by the frame's draws and the defect is produced during rendering (then Experiment 1 and
the shading analysis in `control-center-vs-folder-rendering.md` take over).

### Side effects

- One extra submit per acquired image; acquire latency increases and the frame pacing in
  `rev231-metal-latency.md` is no longer comparable.
- Any legitimate cross-frame retention on the presented image is destroyed by design; if
  the client relies on it, the whole screen (not only the triangles) changes — that itself
  is informative but must not be mistaken for a fix.
- `vkCmdClearColorImage` on a tiled image is a full-surface store; on this device it is
  extra bandwidth and may evict tiles.

### Rollback

Boot argument to `off` (no rebuild), or remove the acquire hook. The clear writes only the
acquired image and never the producer's internal targets, so no persistent state changes.

---

## Experiment 3 — disable cross-frame render-target reuse

### Hypothesis tested

A runtime image can be handed to a new pass while an earlier GPU operation still reads it,
or a recycled image supplies the previous owner's contents. The audit already argues both
are excluded: submission is synchronous and completion implies cache visibility, and
recycling clears ownership and excludes IOSurfaces
(`cross-frame-attachment-audit.md` §3–§5). This experiment is the direct empirical check.

### Exact code location

- The switch already exists: `metal2vulkan-runtime/src/recycle.rs:76` —
  `if cfg!(m2v_no_recycle) { 0 }` makes the image and buffer bounds zero.
- The cfg is registered in `metal2vulkan-runtime/Cargo.toml:83`.

### Design

1. Build the runtime with `RUSTFLAGS="--cfg m2v_no_recycle"` (no source change).
2. Run the same folder-open, App-Library and Control-Center sequence with the existing
   capture options.
3. Compare artifact presence, plus the `images_evicted` / `buffers_parked` counters from
   `recycle.rs:52-73` to confirm the cfg took effect (both must read zero).

### Expected observation

Negative: the artifacts are unchanged. A positive result (artifacts disappear) would mean
a recycled image carries content despite the zeroing guarantee, which would contradict
`texture/first_move.rs` and `recycle.rs:23-39` and would be the more important finding.

### Side effects

- Every frame allocates fresh images and buffers: higher peak memory, slower startup and
  steady state, possible allocation failure under memory pressure.
- Addresses change every frame, so any address-keyed capture correlation is invalid.
- Performance numbers are not comparable to the baseline.

### Rollback

Rebuild without the cfg. No persistent state.

---

## Experiment 4 — add correctly scoped GPU cache and execution dependencies

### Hypothesis tested

The visible region is read by the display engine, or by the next submission, before the
producing GPU work has made it visible. The audit measured that this is unlikely:
`SweetX9RenderEngine.inc:461-481` waits for the fence, halts the CSG, flushes the GPU and
cleans/invalidates the exposed bytes **before** reporting `completed_id`, so a completed
submission already implies cache visibility; and the artifact is static and geometric, not
a moving tear (`cross-frame-attachment-audit.md` §5).

This experiment is therefore a **negative control**: it should be run to be able to say the
ordering hypothesis was tested rather than assumed away.

### Exact code location

- GPU-side scoped dependency (existing, for reference):
  `metal2vulkan-runtime/src/render/executions.rs:3290-3293` (`Barrier::everything()`
  between instances) and `render.rs:5310-5313`.
- Render-pass self-dependency (FBF): `metal2vulkan-runtime/src/render_target.rs:1785-1815`
  — no `VK_SUBPASS_EXTERNAL` arm today.
- Display-side ordering point: `x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc:461-481`
  (post-completion work) and `SweetFramebuffer.cpp:1180-1200` (swap / vsync hold).

### Design

1. **GPU→GPU**: add a `VK_SUBPASS_EXTERNAL` dependency (src stage `COLOR_ATTACHMENT_OUTPUT` /
   `FRAGMENT_SHADER`, dst stage `COLOR_ATTACHMENT_OUTPUT`) to render passes whose input
   attachment reads a colour attachment written by an earlier submission. Keep the existing
   self-dependency. This is the only missing dependency shape in the current pass
   description.
2. **GPU→display**: before the swap, issue one display transaction/flush for the surface
   range after the fence wait, gated by a boot argument. Do not change the existing cache
   operations; add an explicit barrier only.
3. Run as two independent A/Bs (4a GPU→GPU, 4b GPU→display) so a negative result is
   attributable.

### Expected observation

Negative for both: pixels and seam positions unchanged. A positive 4b would show the
artifact disappearing only on the first frame after a swap and reappearing on a later
frame, i.e. a race signature, which the current static CC seam does not have.

### Side effects

- 4a: one more dependency per pass; negligible cost, but it changes submission ordering
  and therefore the latency series.
- 4b: a per-frame display barrier can lower the achieved refresh rate and change the
  vsync hold in `SweetFramebuffer.cpp:1039-1081`; it is also the riskiest experiment for
  display stability, so it must be behind a default-off boot argument and must not touch
  cache maintenance.
- Any change here is explicitly outside the "no unverified changes to Cache Maintenance,
  GPU Fence, HSR, DCD, Framebuffer Fetch" rule unless the user approves it as an
  experiment.

### Rollback

4a: remove the dependency. 4b: boot argument off; removing the barrier restores the
previous swap path exactly, because it adds no state.

---

## What none of these experiments can establish

- Whether the swipe redraws the *same* pixels that were corrupted: every experiment above
  observes the frame after the transition, not the transition's per-pixel history. That
  needs the instrumentation listed in `control-center-vs-folder-rendering.md` §5.
- The per-triangle shading difference measured in Control Center: it survives every
  attachment experiment if it is a shader/interpolation property. It needs a capture with
  provoking-vertex/varying values, which the current trace does not emit.
- Root cause. Each experiment only removes one mechanism; a positive result identifies a
  mechanism, not necessarily the only one.
