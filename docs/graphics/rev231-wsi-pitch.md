# Rev231 WSI Pitch Compatibility

Scope: Metal texture descriptor → IOSurface plane → no-copy buffer → PanVK
image layout → kernel presentation. Sources are the working tree plus the
external Mesa/metal2vulkan trees under `.local/x9/work/`; log is
`/Users/tosasitill/2.txt`.

## 1. There is no VkSwapchain in this stack

A search of the tree finds no `VkSwapchain`, `VK_KHR_swapchain`, or
`vkCreateSwapchainKHR` use. Presentation is:

1. CoreAnimation gives the Metal frontend an `IOSurfaceRef` and a texture
   descriptor (`newTextureWithDescriptor:iosurface:plane:` in
   [NativeBridgeMetalFrontend.mm:1657-1714](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1657)).
2. The frontend takes the plane base, `IOSurfaceGetBytesPerRowOfPlane`, the
   allocation size, and imports the pages as a no-copy buffer.
3. `nmb_texture_new_on_buffer(..., iosurface=true)` builds the Metal texture
   over those pages ([NativeMetalObjects.rs:781-792](/Users/tosasitill/ios_plj110/x9/metal/General/bridge/NativeMetalObjects.rs#L781)).
4. The kernel display consumes an IOSurface ID from the frame record and scans
   or copies it ([SweetFramebuffer.cpp:1482-1547](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L1482)).

So Mesa's `WSI pitch` message is **not** about a Vulkan swapchain. It is the
name PanVK uses for an explicit linear row pitch (`wsi_row_pitch_B`). This
distinction matters: the errors in the log are texture-import failures, not
presentation-queue failures. Confidence: **HIGH**.

## 2. Exact pitch conditions

### 2.1 Frontend: no alignment decision

The frontend passes the IOSurface's row pitch through unchanged:

- plane base/row from `IOSurfaceGetBaseAddressOfPlane` /
  `IOSurfaceGetBytesPerRowOfPlane`
  ([NativeBridgeMetalFrontend.mm:1664-1666](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1664));
- offset `start - base`, `bytesPerRow = row`
  ([NativeBridgeMetalFrontend.mm:1676](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1676));
- buffer manager view size `row * descriptor.height`
  ([NativeBridgeMetalFrontend.mm:409-416](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L409)).

There is no `bpr & 63` check and no padding here. The only frontend checks are
nonzero row, plane base inside the allocation, and page-rounding of the import
length.

### 2.2 PanVK: explicit linear layout validation

The runtime creates the image with `VK_EXT_image_drm_format_modifier` and an
explicit plane layout. PanVK copies `pPlaneLayouts[plane].rowPitch` into
`wsi_row_pitch_B` ([panvk_image.c:507-517](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/mesa-render-source/src/panfrost/vulkan/panvk_image.c#L507)),
and later reports the resulting row pitch back to the caller
([panvk_image.c:995-999](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/mesa-render-source/src/panfrost/vulkan/panvk_image.c#L995)).
`pan_mod_linear_init_slice_layout()` receives that explicit pitch as
`layout_constraints->wsi_row_pitch_B`:

- pitch must describe at least the mip width:
  [pan_mod.c:959-963](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/mesa-render-source/src/panfrost/lib/pan_mod.c#L959);
- `row_stride_B & align_mask` → `WSI pitch not properly aligned`:
  [pan_mod.c:965-971](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/mesa-render-source/src/panfrost/lib/pan_mod.c#L965);
- `offset_B & align_mask` → `WSI offset not properly aligned`:
  [pan_mod.c:973-977](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/mesa-render-source/src/panfrost/lib/pan_mod.c#L973).

`align_mask` is `pan_linear_or_tiled_row_align_req(PAN_ARCH, format, plane) - 1`
([pan_mod.c:936-937](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/mesa-render-source/src/panfrost/lib/pan_mod.c#L936)).
For v7+ formats not on the 16-byte/1-byte exception lists, that requirement is
**64 bytes** ([pan_layout.h:228-238](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/mesa-render-source/src/panfrost/lib/pan_layout.h#L228)).

This is a driver constraint, not a kernel display constraint. The kernel's
own validator is separate and looser: 4-byte elements, 16-byte physical
alignment, stride copied verbatim
([X9DisplayBackend.inc:217-251](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/X9DisplayBackend.inc#L217)).
A 16-bpp IOSurface would fail the kernel's `bytes_per_element != 4` check
before pitch is considered; the observed pitch errors therefore occur in the
GPU import, not at present time. Confidence: **HIGH**.

## 3. Routes and their exact conditions

The runtime tries in-place aliasing first:

| Route | Condition | Counter |
|---|---|---|
| direct | Vulkan's linear image reports `offset==0`, `row_pitch==Metal row pitch`, and matching array pitch | `host_textures_in_place` (includes pitched) |
| pitched | in-place image needs an explicit `DRM_FORMAT_MOD_LINEAR` plane at Metal's pitch because the driver's default linear layout differs | `pitched_host_images` |
| shadow | in-place is refused; the runtime allocates its own image and copies in/out | `shadowed_textures` |
| reject_pitch | the shadow path also cannot make an image at that pitch | `host_texture_shadows_refused_pitch` |
| reject_image / reject_memory | shadow refused for image/format or placement | `host_texture_shadows_refused_image` / `_placement` |

Source:

- layout comparison and direct return:
  [in_place_image.rs:298-314](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/metal2vulkan-pan205-source/crates/metal2vulkan-runtime/src/texture/in_place_image.rs#L298);
- pitched fallback and its refusal:
  [in_place_image.rs:331-338](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/metal2vulkan-pan205-source/crates/metal2vulkan-runtime/src/texture/in_place_image.rs#L331);
- explicit-pitch plane at offset 0, `row_pitch`, `array_pitch`, linear
  modifier:
  [in_place_image.rs:405-435](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/metal2vulkan-pan205-source/crates/metal2vulkan-runtime/src/texture/in_place_image.rs#L405);
- counter semantics:
  [work_counts.rs:2196-2214](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/metal2vulkan-pan205-source/crates/metal2vulkan-runtime/src/work_counts.rs#L2196)
  and [work_counts.rs:4544-4546](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/metal2vulkan-pan205-source/crates/metal2vulkan-runtime/src/work_counts.rs#L4544).

Important semantic point: `reject_pitch` counts a **shadow fallback refused**,
not the first in-place decision. A texture counted there has no usable
image/route, which is consistent with the 274 Mesa errors. Confidence: **HIGH**
for the code semantics; **MEDIUM** that every Mesa error maps 1:1 to a
`reject_pitch` increment (multiple images per texture can each log).

## 4. Log evidence

### 4.1 Counts

| Process | `WSI pitch not properly aligned` | Share |
|---|---|---|
| backboardd | 265 | 96.7% |
| SpringBoard | 9 | 3.3% |
| total | 274 | first 15.910 s, last 87.600 s |

No `WSI offset not properly aligned` and no `WSI pitch too small` occurred.

### 4.2 Proven 16-bpp case

```
33.230 SWEET SURF: SpringBoard create id 63 78x120 fmt 0x32433038 bpe 2 bpr 160 planes 0 size 32768
33.230 |SpringBoard| MESA: error: WSI pitch not properly aligned
33.245 |SpringBoard| MESA: error: WSI pitch not properly aligned
33.288 SWEET SURF: SpringBoard create id 65 78x120 ... bpe 2 bpr 160 ...
33.289 |SpringBoard| MESA: error: WSI pitch not properly aligned
33.296 |SpringBoard| MESA: error: WSI pitch not properly aligned
```

([2.txt:2636](/Users/tosasitill/2.txt), [2.txt:2733](/Users/tosasitill/2.txt)).
`160 & 63 = 32`, so the 64-byte rule rejects it. The same geometry recreated
as id 65 fails again, which rules out a one-off. Confidence: **HIGH**.

Adjacent accepted cases:

- `52x60 bpr 128` (128 is 64-aligned), created 32.615, no pitch error
  ([2.txt:2521](/Users/tosasitill/2.txt));
- `84x120 bpr 192` (192 is 64-aligned), created 33.205/33.270, no pitch error
  ([2.txt:2614](/Users/tosasitill/2.txt));
- `1256x2760 bpr 5120` (5120 = 80×64), created 15.809, routed direct/pitched
  ([2.txt:1659](/Users/tosasitill/2.txt), [2.txt:1670](/Users/tosasitill/2.txt)).

A `230x186 bpr 480` SpringBoard surface was created at 32.334
([2.txt:2470](/Users/tosasitill/2.txt)); 480 is also not 64-aligned, and pitch
errors follow later at 33.806–33.891, but no layout line ties them to id 60.
Confidence: **LOW** for that association.

### 4.3 Backboardd route counters

Final sampled snapshot at 86.823 s:

```
direct=15 pitched=5 shadow=15 reject_pitch=14 reject_image=0 reject_memory=1
```

([2.txt:6896](/Users/tosasitill/2.txt)). Because `pitched` is a subset of
`direct`, this means 10 plain in-place, 5 pitched in-place, 15 shadow copies,
and 15 textures with no usable route (14 pitch, 1 placement). The first
snapshot at 15.848 s was `direct=1 pitched=1 shadow=0 reject_pitch=0`, and by
16.317 s it was `direct=2 pitched=2 shadow=1 reject_pitch=1`
([2.txt:1670](/Users/tosasitill/2.txt), [2.txt:1777](/Users/tosasitill/2.txt)).

The first pitch error at 15.910 s follows a 196,608-byte read-only import at
15.884/15.904 s ([2.txt:1679](/Users/tosasitill/2.txt),
[2.txt:1683](/Users/tosasitill/2.txt)). Temporal adjacency is strong but the
surface's row pitch is not logged, so the association is **MEDIUM**, not
proven. What is proven is that the error is not caused by the 1256x2760
surface (its 5120 pitch is aligned and it routes successfully).

## 5. Are swapchain images and imported IOSurface textures confused?

There are no swapchain images, so the classic confusion cannot exist. There is
a different conflation:

- `nmb_texture_import_stats` aggregates **all host-page textures**: no-copy
  Metal buffers, imported IOSurfaces, and heap-buffer-backed textures all
  increment the same six counters
  ([NativeMetalObjects.rs:252-263](/Users/tosasitill/ios_plj110/x9/metal/General/bridge/NativeMetalObjects.rs#L252)).
- The counters are sampled only when a surface layout is logged (imports 1–8
  and every 128th), so they are snapshots of all textures created in that
  process, not a per-present queue
  ([NativeBridgeMetalFrontend.mm:1699-1714](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1699)).

Consequently, a refusal counter cannot be attributed to a presented IOSurface
rather than to an ordinary sampled texture. This is the actual attribution gap
behind the 265 backboardd errors. Confidence: **HIGH**.

## 6. Impact

- In-place routes avoid copies; shadow routes allocate a private image and copy
  in/out (usage-dependent), adding CPU/GPU traffic and memory
  ([texture.rs:2202-2248](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/metal2vulkan-pan205-source/crates/metal2vulkan-runtime/src/texture.rs#L2202)).
- Of 45 sampled backboardd texture decisions, 15 went to shadow and 15 had no
  route (14 pitch, 1 placement); of 10 SpringBoard decisions, 2 went to shadow
  and 2 had no route. The 15 in-place backboardd routes include the 5 pitched
  ones.
- Whether these failures affect the Control Center platter is **not
  established**. The log has no CC markers, and the platter has never been
  shown to use an unaligned-pitch surface. Do not treat this as the platter
  cause. Confidence: **HIGH** that the causal link is unproven.

## 7. Missing instrumentation

1. One line per imported surface with: process, IOSurface ID, plane, width,
   height, bpp, bpr, plane offset, allocation, chosen route, and refusal
   reason. Current 1–8/128 sampling hides every later surface.
2. A route decision log inside `in_place_unbound` / shadow creation naming the
   exact failing check (pitch, offset, width, image, placement), not only
   Mesa's generic error.
3. Separate counters for IOSurface-imported presentation surfaces versus
   ordinary host-page textures.
4. Bits/bytes copied by shadow in/out, per surface, to price the fallback.
5. The kernel present record should log the IOSurface ID and stride it
   actually scans/copies, so GPU-import and display use can be joined.

No production code was modified.
