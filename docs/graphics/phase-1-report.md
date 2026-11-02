# Phase 1 Report — Metal Runtime / metal2vulkan Boundary Split

- Date: 2026-11-02
- Branch / HEAD: `feat/G1Ultra` / `5b3caad` (working tree carries the Phase 1 change)
- Task: Task 3 (P1). Split the Metal runtime from the native/Vulkan plumbing at the
  discovery boundary. No PanVK GPU command generation and no XNU Mali backend change.
- Tags: `VERIFIED`, `SOURCE CONFIRMED`, `BUILD VERIFIED`, `DEVICE VERIFIED`,
  `NOT TESTED`, `INFERENCE`.

Only the compile/link below is `BUILD VERIFIED`. Nothing here is `DEVICE VERIFIED`; no
image was built, flashed or booted.

---

## 1. Scope

**In scope**
- The one place where the Metal runtime still reached across its layer boundary: device
  discovery in `X9GeneralMetalFactory.mm` imported IOKit and the native render transport,
  probed the `SweetX9Render` service itself, and hard-coded the installed PanVK loader
  path.
- Moving those questions behind a bridge-owned boundary so the Metal layer holds only
  answers (`nmb_backend_probe`, `nmb_backend_loader_path`, `nmb_backend_open`).
- Build wiring for the new translation unit.

**Out of scope (per Task 3)**
- PanVK submission, `pan_kmod_ops`, CSF registers, page tables, cache maintenance.
- XNU Mali backend, display, HSR/DCD/FPK, framebuffer fetch behavior.
- metal2vulkan translation and route policy.
- ABI changes in any layer.
- Any deployment step: PanVK rebuild, plugin packaging, kernel allowlist regeneration,
  kernel build, flashing.

**Method**
Source-diff against `5b3caad`, object-level symbol audit of the produced bundle, and one
full compile/link run of `build-general-metal.py`. Runtime behavior is argued from code
equivalence, not measured.

## 2. Existing Implementation

Before this change the discovery factory (`X9GeneralMetalFactory.mm`, `5b3caad`:

- imported `<IOKit/IOKitLib.h>`, `X9RenderTransport.h` and `X9MetalPolicy.h`;
- matched `SweetX9Render` with `IOServiceGetMatchingService` and read `X9RenderABI` /
  `X9RenderReady` itself;
- called `x9_render_open` / `x9_render_close` itself to prove the connection works and to
  learn `shader_present`;
- resolved the accelerator port's registry ID with
  `IORegistryEntryGetRegistryEntryID(port, ...)`;
- named the loader path directly in `native_bridge_metal_open(X9_PANVK_BUNDLE_PATH, ...)`.

Everything downstream of that was already layered correctly: the frontend imports no IOKit
and no transport header, metal2vulkan has no reference to `x9_render*`, `SweetX9Render` or
a `/dev` node (grep of `crates/` for `x9_render|SweetX9|IOServiceGetMatching` returns
nothing), and CSF access lives only in the X9 PanVK backend. `SOURCE CONFIRMED`.

So the remaining cross-layer leak was narrow and in discovery only. That is what Phase 1
closes; no submission, memory or display path is touched.

## 3. Changes

Four files; two new, two edited. No ABI header, no manifest, no patch.

| File | Change |
|---|---|
| `x9/metal/General/bridge/NativeBackendProbe.h` (new) | Boundary declarations: `struct NmbBackendProbe { uint64_t shader_present; }`, `nmb_backend_probe`, `nmb_backend_loader_path`, `nmb_backend_open`. |
| `x9/metal/General/bridge/NativeBackendProbe.mm` (new) | The moved code: IOKit match, ABI/ready check, `x9_render_open`/`close`, `shader_present` answer, registry-ID resolution from the accelerator port, loader path, `native_bridge_metal_open`. |
| `x9/metal/General/X9GeneralMetalFactory.mm` (edited) | Drops `<IOKit/IOKitLib.h>`, `X9RenderTransport.h`, `X9MetalPolicy.h` and the `X9_PANVK_BUNDLE_PATH` fallback; calls `nmb_backend_probe` / `nmb_backend_loader_path` / `nmb_backend_open`. |
| `x9/build/scripts/build-general-metal.py` (edited) | Compiles `bridge/NativeBackendProbe.mm` with the same flags (including `-DX9_PANVK_BUNDLE_PATH=...`) and links the object into the bundle. |

Factory after the change (`X9GeneralMetalFactory.mm:34-45`):

```objc
 struct NmbBackendProbe probe={0};
 if(!nmb_backend_probe(&probe))return nil;
 native_bridge_metal_set_shader_core_mask(probe.shader_present);
 ...
 id device=nmb_backend_open(cache,port,missing_selector);
```

The boot-arg gate (`x9_metal_render=1`), the cache directory choice, the console logging
and the `device=nil` refusal path are unchanged.

## 4. Architecture

Before:

```
X9GeneralMetalFactory  ── IOKit ─────────────┐
      │                 ── x9_render_open ──┤   (Metal layer knew both)
      │ X9_PANVK_BUNDLE_PATH                │
      └──────────────── native_bridge_metal_open
```

After:

```
X9GeneralMetalFactory        only: nmb_backend_probe / loader_path / open
      │
bridge/NativeBackendProbe.mm  ── IOKit ──── SweetX9Render probe
      │                       ── X9RenderTransport ── x9_render_open/close
      │                       ── X9_PANVK_BUNDLE_PATH
      └──────────────── native_bridge_metal_open
```

Beneath the probe nothing moved: transport → 12-selector native ABI → kernel render
service → CSF, and PanVK over `pan_kmod_ops`, are the same code as at HEAD. The probe
therefore answers the Metal layer without exporting any XNU object or mapping to it.

## 5. ABI Impact

**None, all layers.** `SOURCE CONFIRMED` (diff touches no header under `x9/kernel/`, no
`X9RenderABI.h`, no `pan_kmod_ops`, no `nmb_*` signature used by metal2vulkan).

- External Metal ABI unchanged: `NSPrincipalClass` stays `X9GeneralMetalFactory`,
  `initWithAcceleratorPort:` keeps its signature, the bundle still exports
  `native_bridge_metal_open` and the `nmb_*` façade.
- New exported C symbols are internal to the bundle: `nmb_backend_probe`
  (`NativeBackendProbe.mm:14`), `nmb_backend_loader_path` (`:37`), `nmb_backend_open`
  (`:39`). They are not called from outside the bundle and are not in any allowlist.
- The kernel-side allowlist header is untouched. See §8 for the revision consequence.

## 6. Memory Impact

**None.** The probe allocates no buffer, mapping or page table; it opens a connection,
reads caps, closes it, exactly as the factory did. The device object, cache path and
resource ownership are unchanged (`SOURCE CONFIRMED`).

## 7. Synchronization Impact

**None.** Discovery runs before any device object exists and takes no lock that
submission or display uses. `nmb_backend_probe`'s `x9_render_open`/`close` pair is the same
short-lived second client the factory created before this change. `SOURCE CONFIRMED`.

## 8. Build Result

`BUILD VERIFIED` for the userspace bundle only:

```
python3 x9/build/scripts/build-general-metal.py --revision 230
→ Generic Metal runtime and iOS16 bundle compiled/linked/signed;
  no execution or deployment; requires native rendering KMD+PanVK loader
```

- Compiled and linked: `X9GeneralMetalFactory.o`, `NativeBackendProbe.o`,
  `X9RenderTransport.o`, `X9GeneralMetalFrontend.o`,
  `libx9_general_metal_bridge.a` → `X9GeneralMetal230.bundle/X9GeneralMetal`, stripped and
  ad-hoc signed with `--pagesize 16384`.
- Two warnings, both pre-existing and unrelated: `X9FrameCapture.inc:147,148`
  (`NBBuffer *` passed where `id<MTLBuffer>` is expected).
- Link audit of the produced bundle (`nm`):
  - `_nmb_backend_probe`, `_nmb_backend_loader_path`, `_nmb_backend_open`,
    `_native_bridge_metal_open` present.
  - `X9GeneralMetalFactory.o` undefined IOKit symbols: 0; undefined `x9_render_*`: 0.
  - `NativeBackendProbe.o` is the only object with IOKit (3) and transport (1) references.
  So the Metal layer now has no compile- or link-level path to the native service or the
  Mali backend.
- **Not built:** PanVK, kernel, boot image. **Not run:** nothing was installed or booted.
- **Packaging consequence, deliberate:** the built bundle is revision 230, while
  `x9/kernel/generated/x9_render_external_manifest.h` still pins `X9GeneralMetal229` /
  `X9PanVK229`. Making this bundle deployable needs `X9PanVK230.bundle` (no 230 PanVK
  bundle exists locally), then the `plugins` packaging step that rewrites the allowlist,
  then a kernel build that accepts it. Those steps also imply a new flash candidate, which
  is the user's call, so they were not run.

## 9. Runtime Result

`NOT TESTED`. No device run, no flash, no reboot, no kernel or PanVK rebuild. The claim in
§11 is a code-equivalence argument, not a measurement.

## 10. Performance

No effect on any submission, translation, cache or display path. Discovery gains one
non-inlined call and reads the same registry properties; the connection open/close pair is
unchanged in count and order. `INFERENCE` from the diff.

## 11. Regression

Behavior-equivalence check against `5b3caad`, in order:

| Step | Before | After |
|---|---|---|
| boot-arg gate | `x9_metal_render=1` in `kern.bootargs` | same |
| service present + ABI 1 + ready | IOKit match/read in factory | same code, in probe |
| connection opens | `x9_render_open`/`close` | same calls, in probe |
| shader cores > 0 | factory refuses | probe refuses (factory returns nil) |
| shader mask | `native_bridge_metal_set_shader_core_mask(caps.shader_present)` | same value from probe |
| accelerator port → registry ID | `IORegistryEntryGetRegistryEntryID(port)` in factory | same call in `nmb_backend_open` |
| device open | `native_bridge_metal_open(loader, cache, registry, missing)` | same call, same arguments, from probe |
| logs | `begin … loader=`; `factory … registry=%llu` | `begin … loader=` (same string); `factory … port=%u` |

Observable deltas, both logging-only:

1. When the accelerator port cannot be resolved to a registry ID, the `begin` line is now
   printed and then `factory … device=nil`; before, the factory returned silently with no
   log. Return value, refusal condition and side effects are identical.
2. The second log prints `port=%u` instead of `registry=%llu`. Anything that greps the
   old token would need the new one; nothing in-tree parses it. `SOURCE CONFIRMED`.

Acceptance conditions from the specification (§3.4):

| Condition | Status |
|---|---|
| Metal Runtime does not operate Mali CSF | Holds. Was already true; now also true at link level — the factory/frontend objects have no IOKit or transport undefined symbols. `BUILD VERIFIED` |
| metal2vulkan does not operate XNU private mappings | Holds unchanged. No `x9_render*`, `SweetX9Render`, registry or `/dev` access in `crates/`; IOSurface use goes through public IOSurface/Metal entry points. `SOURCE CONFIRMED` |
| Metal object release not dependent on implicit GPU idle | Holds unchanged. metal2vulkan holds resources "from each encoder's end until the command buffer completes" (`used_resources.rs:22`; `commit()` `command_buffer/mod.rs:2424`); release is tied to completion, not to a global-idle assumption. `SOURCE CONFIRMED` |
| Pipeline cache and Function Constant semantics preserved | Untouched by this change: no pipeline, shader-cache or specialization code was edited. `SOURCE CONFIRMED` |
| Framebuffer Fetch path preserved | Untouched: no render-pass, subpass, tile-load or attachment code was edited. `SOURCE CONFIRMED` |
| Existing Metal call chain does not regress | Compile/link verified with the same flags and the same argument sequence; runtime `NOT TESTED` |
| No external Metal ABI change | Holds. `SOURCE CONFIRMED` |

## 12. Rollback

Revert the four Phase 1 files and rebuild:

```
git checkout -- x9/metal/General/X9GeneralMetalFactory.mm x9/build/scripts/build-general-metal.py
rm x9/metal/General/bridge/NativeBackendProbe.h x9/metal/General/bridge/NativeBackendProbe.mm
python3 x9/build/scripts/build-general-metal.py --revision <rev>
```

No kernel, PanVK, manifest or on-device state changes, so rollback is compile-only.

## 13. Remaining Issues

1. **Control Center platter triangles are still unresolved** (missing, split and flickering
   triangles). This report does not claim otherwise; no rendering path was touched, and no
   attempt was made to attribute it to this refactor.
2. **Task 2 design is not implemented.** Submission is still synchronous end to end with
   `metal_service_lock` held across the completion poll; the async v2 ABI in
   `submission-decoupling-design.md` remains a proposal.
3. **The boundary is now a rule, not yet a check.** Nothing in CI prevents a future
   factory edit from re-importing IOKit or the transport. A build-time check (for example,
   asserting the factory/frontend objects have no `IOServiceGetMatching*` /
   `IORegistryEntry*` / `x9_render_*` undefined symbols) would enforce it; not added here.
4. **Legacy non-bundle Metal files still import IOKit**: `x9/metal/X9Metal.m` and
   `x9/metal/x9-metal-probe.m`. They are not compiled into `X9GeneralMetal<N>.bundle`
   (`build-general-metal.py` compiles only `General/*.mm` and `General/bridge/*.mm`), so
   they do not affect the shipped path; left untouched.
5. **Deployment is pending and needs a decision**: PanVK 230 build, plugin packaging,
   allowlist regeneration and a kernel build, followed by the user flashing the candidate.
6. **Discovery still opens a second render connection** (`x9_render_open`/`close`) before
   the real one. Pre-existing; unchanged; a possible later simplification now that the
   call sits in one file.
