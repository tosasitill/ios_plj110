# Phase 2 Report — PanVK / XNU Backend Boundary

- Date: 2026-11-02
- Branch / HEAD: `feat/G1Ultra` / `5b3caad` (working tree carries Phase 1 + Phase 2)
- Task: Task 4 (P2). Split PanVK from the XNU backend by making the existing
  integration boundary explicit and build-enforced. No GPU command generation,
  cache, fence, CSF or display change.
- Tags: `SOURCE CONFIRMED`, `BUILD VERIFIED`, `NOT TESTED`, `INFERENCE`,
  `NOT IMPLEMENTED`, `NEEDS AUDIT`.

Only the two userspace builds below are `BUILD VERIFIED`. Nothing is
`DEVICE VERIFIED`; no image was built, flashed or booted.

---

## 1. Scope

**In scope**
- Audit the existing native integration: every include and native symbol in
  Mesa PanVK, the X9 integration (`x9/metal/General/panvk/`), the transport and
  the Metal layer.
- Confirm that CSF, GPU VM, BO, Fence and Cache each have one responsible owner.
- Turn that boundary into a build-time check so a later edit cannot silently
  reintroduce a direct XNU/IOKit/display dependency.

**Out of scope (deliberately)**
- Any change to submission, cache maintenance, fences, CSF registers, page
  tables, HSR/DCD/FPK or display.
- Any `pan_kmod_ops` semantic change, any ABI change, any kernel or PanVK source
  change.
- Rewriting the integration (the spec forbids unverified semantic changes and
  large refactors).
- Deployment: PanVK packaging, allowlist regeneration, kernel build, flash.

**Method**
Source scan of 109 PanVK sources and 25 Metal sources, direct reading of the
userspace backend and the kernel backend, and two full builds. Runtime behaviour
is argued from code, not measured.

The detailed matrix, ownership map, address-space table, cache and fence
analysis, and ranked findings live in `docs/graphics/panvk-backend-boundary.md`.
This report states the change, its verification and its limits.

## 2. Existing Implementation

Audit result: the integration was already structurally clean. `SOURCE CONFIRMED`.

- **PanVK core** (`mesa-render-source/src/panfrost/vulkan`, 109 sources incl.
  headers/`.inc`) include no IOKit, IOSurface, display service, `mach/*`,
  `libkern` or X9 transport header. The only textual hits are comments
  (`panvk_physical_device.c:2,1630`, `panvk_instance.c:160`,
  `panvk_device_memory.c:382,416,454`) and the conditional framework link in
  `meson.build:146`.
- **X9 integration** (`x9/metal/General/panvk/`, 1243 lines) includes
  `kmod/pan_kmod.h`, `drm-uapi/panthor_drm.h`, the POD `X9RenderABI.h` and
  `X9RenderTransport.h`; it contains no XNU object type and no register access.
- **Metal factory / frontend** name no IOKit and no `x9_render_*` after Phase 1;
  `bridge/NativeBackendProbe.mm` is the only discovery seam.
- **Kernel backend** owns CSF queue/group/doorbell, page tables, BOs, cache plan
  and completion/history.

Ownership, in one line each:

| Resource | Owner |
|---|---|
| CSF group, queue, command stream, doorbell | XNU backend (`X9CSFRenderQueue.cpp`) |
| GPU VM, PTE construction, AS install | XNU backend (`X9RenderPageTable.cpp`) |
| BO allocation/import/release | XNU backend (`SweetX9RenderService.cpp:194-277`) |
| Fence completion | XNU backend records; userspace sync provider mirrors into Vulkan |
| Cache maintenance | XNU backend plan/execute; userspace supplies exact ranges |

The remaining defect was not ownership but *enforceability*: nothing failed the
build if a future edit added `#include <IOKit/...>` to a PanVK file or called
`x9_render_*` from the Metal layer.

## 3. Changes

Three files; one new, two edited. No runtime source, header, patch, manifest or
ABI changed.

| File | Change |
|---|---|
| `x9/build/scripts/check-native-boundary.py` (new) | Source-level boundary checker with `--panvk` and `--metal` policies. Fails on forbidden direct includes and on native symbols outside the allowed integration files. |
| `x9/build/scripts/build-panvk-native-ios.py` (edited, `:26`) | Runs `--panvk` after third-party patches, before budget prep and meson. |
| `x9/build/scripts/build-general-metal.py` (edited, `:40`) | Runs `--metal` before the Rust bridge is compiled. |

The check reads sources only; it builds, loads and executes nothing.

## 4. Architecture

```
Mesa PanVK core ── pan_kmod_ops ──► X9 integration (panvk/*.c)
                                        │  X9RenderTransport.h
                                        ▼
                                  X9RenderTransport.c ──► 12-selector ABI
                                        │
                                        ▼
                          kernel SweetX9Render → CSF / VM / BO / Cache / Fence
```

What the check enforces:

```
panvk core ── ✗ IOKit / IOSurface / display / mach_vm / libkern / transport
panvk core ── ✓ pan_kmod, panthor_drm uapi
X9 integration ── ✓ transport + x9_render_*
Metal factory/frontend ── ✗ IOKit / transport / x9_render_*
bridge/NativeBackendProbe.mm ── ✓ IOKit + transport (single seam)
```

## 5. ABI Impact

**None.** No `X9RenderABI.h`, no `pan_kmod_ops` signature, no ioctl number, no
allowlist header was touched. `SOURCE CONFIRMED` by diff.

## 6. Memory Impact

**None.** The check allocates nothing at runtime and the builds add only a
pre-process scan. Userspace mirror size (`4096`), BO ownership and cache ranges
are unchanged. `SOURCE CONFIRMED`.

## 7. Synchronization Impact

**None.** No fence, wait, timeout or lock code was edited. The audit confirms the
existing lifecycle: timeline completion only after a validated synchronous
native completion; waits use cond-var deadlines with no fixed sleep; kernel
`render_wait` is a non-blocking 64-entry history lookup. `SOURCE CONFIRMED`.

## 8. Build Result

`BUILD VERIFIED` for userspace only:

```
python3 x9/build/scripts/build-general-metal.py --revision 230
  native boundary clean (metal): 25 sources
  Generic Metal runtime and iOS16 bundle compiled/linked/signed; ...

python3 x9/build/scripts/build-panvk-native-ios.py
  Native PanVK dynamic library linked; no device execution or rendering validation
```

- `python3 -m py_compile` passes for `build-general-metal.py`,
  `build-panvk-native-ios.py` and `check-native-boundary.py`.
- Both checks pass on the current tree: `--panvk` 109 sources, `--metal` 25
  sources.
- Negative test: a temporary `panvk/__boundary_selftest.c` with
  `#include <IOKit/IOKitLib.h>` made `--panvk` exit 1; the file was removed and
  the check returned clean.
- Metal build emits only the two pre-existing `X9FrameCapture.inc:147,148`
  warnings.
- **Not built:** kernel, boot image. **Not run:** nothing installed or booted.

## 9. Runtime Result

`NOT TESTED`. No device run, flash or reboot. This task changes no executed code,
so there is no new behaviour to observe.

## 10. Performance

No effect. The only additions run at build time and scan text. Submission, cache
and display paths are byte-identical. `INFERENCE` from the diff.

## 11. Regression

| Area | Before | After |
|---|---|---|
| PanVK GPU command generation | — | unchanged (no source edit) |
| Cache maintenance | exact BO ranges + kernel span plan | unchanged |
| Fence/completion | synchronous validated completion | unchanged |
| CSF / VM / BO ownership | kernel | unchanged |
| Metal discovery | bridge probe | unchanged |
| Build | always proceeds | fails early if a boundary rule is violated (intended) |

Acceptance conditions from the specification (§4.2):

| Condition | Status |
|---|---|
| PanVK does not directly manage XNU Task Mapping | Holds: no `mach_vm`/`vm_map`, no `IOMemoryDescriptor`, no kernel VA. `SOURCE CONFIRMED` + build check |
| PanVK does not operate `IOMemoryDescriptor` | Holds: all descriptors are kernel-side. `SOURCE CONFIRMED` |
| PanVK does not depend on a Display Service | Holds: no display include or symbol. `SOURCE CONFIRMED` |
| Metal Runtime does not call XNU CSF private functions | Holds at source and link level after Phase 1. `BUILD VERIFIED` |
| CSF has one owner | XNU backend. `SOURCE CONFIRMED` |
| GPU VM has one owner | XNU backend; userspace mirror is non-authoritative. `SOURCE CONFIRMED` |
| BO has one owner | XNU backend. `SOURCE CONFIRMED` |
| Fence has one lifecycle | Userspace DAG + validated native completion; kernel history. `SOURCE CONFIRMED` |
| Cache is precisely scoped | Exact BO-relative 256-B ranges; kernel span plan. `SOURCE CONFIRMED` |
| Address spaces not conflated | CPU/kernel/GPU/IOVA-PA kept distinct. `SOURCE CONFIRMED` |
| No fixed sleeps | Cond-var deadlines; no `sleep`/`usleep` in the fence path. `SOURCE CONFIRMED` |
| Framebuffer Fetch / HSR / DCD / display preserved | Untouched. |

## 12. Rollback

```
rm x9/build/scripts/check-native-boundary.py
# delete the two invocations:
#   x9/build/scripts/build-panvk-native-ios.py:26
#   x9/build/scripts/build-general-metal.py:40
```

Compile-only rollback; no kernel, PanVK, ABI, manifest or device state involved.

## 13. Remaining Issues

1. **Control Center platter triangles are still unresolved** (missing, split,
   flickering). This phase does not touch, explain or claim to fix them. Per
   spec §3.2D they remain an open rendering-correctness issue.
2. **Patch 00 is path-locked.** `x9/build/third_party/00-mesa-panvk-x9.patch`
   embeds `/Users/tosasitill/HelioDarwin-x9-lk-preserve/...` absolute paths; no
   build script applies it, and the retained tree was manually rewritten. A
   fresh tree cannot be reproduced from the tracked patch. `NEEDS AUDIT`; not
   changed because it is build plumbing, not GPU behaviour.
3. **BO wait is connection-wide**, not per-BO (`X9PanVKMemory.c:165-183`).
   Conservative, but imprecise. Task 6 work.
4. **The boundary check is direct-dependency only.** It does not follow
   transitive includes or inspect object undefined symbols. Object-level
   enforcement in CI is `NOT IMPLEMENTED`.
5. **The ABI header lives under a kernel directory** (`../X9RenderABI.h`) and is
   included by userspace. It is POD, but the location invites kernel-private
   accretion; a shared include home is `NOT IMPLEMENTED`.
6. **Task 2 design is still unimplemented.** Submission remains synchronous with
   `metal_service_lock` held across the completion poll; the async v2 ABI stays a
   proposal.
7. **Deployment is pending and needs a decision.** The 230 Metal bundle is not
   deployable until a PanVK 230 bundle is built, plugins are packaged, the
   allowlist is regenerated and a kernel is built — then the user flashes the
   candidate. None of that was run.
8. **Task 7 (Display / Direct Scanout) has not started.** No GPU direct scanout
   path exists; display remains kernel-side CPU composition.
