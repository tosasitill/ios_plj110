# PanVK ↔ XNU Backend Boundary — Task 4 Audit

- Date: 2026-11-02
- Branch / HEAD: `feat/G1Ultra` / `5b3caad` (working tree carries Task 3 + Task 4)
- Task: Task 4 (P2). Audit the existing native integration, make the PanVK ↔ XNU
  boundary explicit and enforced, without changing GPU command generation, cache
  maintenance, fences, CSF programming or display behaviour.
- Tags: `SOURCE CONFIRMED`, `BUILD VERIFIED`, `NOT TESTED`, `INFERENCE`,
  `NOT IMPLEMENTED`, `NEEDS AUDIT`.

This document is an audit of what exists, plus the build-time check that keeps it
true. It is not a redesign and makes no runtime claim. Nothing here is
`DEVICE VERIFIED`.

---

## 1. Rules and method

Task 4 requires:

1. PanVK must not depend on XNU-private structures it does not need
   (Task Mapping, `IOMemoryDescriptor`, Display Service, CSF private functions
   from the Metal runtime).
2. CSF, GPU VM, BO, Fence and Cache must each have one explicitly responsible
   backend.
3. CPU VA, kernel VA, GPU VA, IOVA and physical address must not be treated as
   interchangeable.
4. Fence identity/state/wait/timeout/release/completion must be one lifecycle,
   with no fixed sleeps.
5. Cache maintenance must be scoped to the resources actually touched.

Method: include and symbol scan of every PanVK and Metal source
(`check-native-boundary.py`), direct reading of the userspace backend
(`x9/metal/General/panvk/*`) and the kernel backend
(`x9/kernel/src/iokit/Kernel/arm/*`), and two full build runs (Metal bundle and
PanVK dylib). No runtime behaviour was changed, so no measurement is reported.

Terminology used below:

- **Native backend** — the XNU Mali backend behind `SweetX9Render` plus the
  `X9RenderTransport` call boundary and the X9 `pan_kmod_ops`.
- **PanVK core** — Mesa `src/panfrost/vulkan` excluding the X9 integration files.
- **X9 integration** — `x9/metal/General/panvk/` and `X9RenderTransport.c`.

## 2. Prohibited-dependency matrix

Evidence is a direct `#include`/`#import` or a named symbol scan. A comment that
mentions "IOKit" is not a dependency; one such comment exists in
`panvk_physical_device.c:2` and is not counted.

| Layer | IOKit | IOSurface | Display svc | XNU VM / libkern | Native transport header | Names `x9_render_*` |
|---|---|---|---|---|---|---|
| PanVK core (Mesa) | none | none | none | none | none | none |
| X9 integration (`panvk/*.c`) | none | none | none | none | yes, by design | yes, by design |
| `X9RenderTransport.c` | yes (`mach/mach.h` + IOKit) | none | none | no | provides it | yes |
| Metal factory / frontend | none | none | none | none | none | none |
| `bridge/NativeBackendProbe.mm` | yes | none | none | no | yes | yes |
| metal2vulkan (`crates/`) | none | public IOSurface API only | none | none | none | none |

`SOURCE CONFIRMED`, and mechanically re-checked by the new build step (§7):

- `native boundary clean (panvk): 109 sources`
- `native boundary clean (metal): 25 sources`

The PanVK core only sees the generic `pan_kmod` interface
(`kmod/pan_kmod.h`, `pan_kmod_backend.h`, `drm-uapi/panthor_drm.h`). The X9
integration is the only place that includes `../X9RenderABI.h` and
`../X9RenderTransport.h` (`X9PanVKMemory.h:3-4`, `X9PanVKMemory.c:4`). Those two
headers are fixed-width POD contracts; neither includes XNU headers. PanVK does
not include `mach/*`, does not call an ioctl and does not hold an
`IOMemoryDescriptor`.

The kernel side owns all XNU objects. `SweetX9RenderService.cpp:226` creates the
`IOBufferMemoryDescriptor`, `:228` creates the import descriptor, and
`X9RenderPageTable.cpp:19,76` translates it to physical segments for the page
tables. None of those types cross the ABI: the user/kernel boundary is
`X9RenderABI.h` only, whose records are fixed-width and `static_assert`-checked.

## 3. Ownership map

| Resource | Single owner | Kernel-side evidence | Userspace-side responsibility |
|---|---|---|---|
| CSF group / queue / command stream / doorbell | XNU backend | `X9RenderCreateGroup` → `render_engine->group_create` (`SweetX9RenderService.cpp:331-343`); group/stream input writes, clean and doorbell in `X9CSFRenderQueue.cpp:29-34,135-167` | packages the 96-byte `X9RenderSubmission` and CALL streams (`X9PanVKSubmitPlan.c`); never touches a register |
| Submission / completion | XNU backend | `render_submit` polls the completion record; `SweetX9RenderEngine.inc:481-483` records it in the 64-entry history | `x9_pan_kmod_submit` rejects any completion that is not already complete and matching (`X9PanVKMemory.c:327-337`) |
| GPU VM / page tables | XNU backend | `X9RenderMap`/`X9RenderUnmap` → `render_engine->map/unmap`; PTEs built in `X9RenderPageTable.cpp`, dirty-table clean at `:120-133` | keeps a `mappings[4096]` mirror for unmap bookkeeping only (`X9PanVKMemory.c:23,195-238`); builds no PTE, IOVA or AS |
| BO allocation / import / release | XNU backend | `X9RenderAllocate`/`ImportHost` (`SweetX9RenderService.cpp:194-275`), `X9RenderRelease` (`:277`) | `allocate`, `free_bo`, `import_host` wrappers; `import_host` retries once read-only on `EACCES` (`X9PanVKMemory.c:384-403`) |
| Fence identity / completion | XNU backend for GPU completion; XNU backend + userspace sync provider for the Vulkan timeline | completion record + history (`SweetX9RenderEngine.inc:5,20,481-483,593-598`) | `X9PanVKSync.c` fence DAG/timeline; a signal becomes complete only after the native call validates (§6) |
| Cache maintenance | XNU backend | plan built in `X9RenderCachePlan.inc:32-85`, executed in `X9RenderMemory.cpp:57-164` | emits exact BO-relative ranges only (`X9PanVKMemory.c:109-139`) |

The split is therefore: **XNU owns hardware and XNU objects; userspace owns
Vulkan bookkeeping**. Nothing userspace holds is authoritative for hardware
state. The `mappings[]` mirror is a bookkeeping cache, and the kernel re-validates
every unmap (`SweetX9RenderService.cpp:296-305`) rather than trusting it.

## 4. Address spaces

The five spaces are kept distinct:

| Space | Range / type | Who assigns | Where it appears |
|---|---|---|---|
| CPU VA | caller's own address space, 16 KiB-aligned for imports | client | `X9RenderAllocation.host_address`, `withAddressRange` (`SweetX9RenderService.cpp:228`) |
| Kernel VA | kernel-only `IOBufferMemoryDescriptor` bytes | XNU | `getBytesNoCopy`/`getPhysicalSegment` (`X9RenderPageTable.cpp:19,76`); never returned |
| GPU VA | `X9_RENDER_MIN_VA` (1 GiB) … `X9_RENDER_MAX_VA` (16 GiB), 4 KiB-aligned | kernel, requested by userspace | `X9RenderMapping.gpu_address` (kernel range-checked at `SweetX9RenderService.cpp:291-293`) |
| IOVA / PA | machine physical pages backing a BO | XNU | only inside page-table installation and cache maintenance; no ABI field exposes it |
| Host VA at import | the client VA above, pinned for the context lifetime | client, pinned by kernel | import BO only; never confused with GPU VA |

Userspace never computes an IOVA or PA. `bind_vm` passes a requested GPU VA and
the kernel checks range, 4 KiB alignment, overlap and the PA of every page
(`SweetX9RenderService.cpp:291-315`). The userspace `mappings[]` entries store
`gpu_address` only as an opaque token for the matching `Unmap`.

## 5. Cache maintenance

Actual behaviour, not the older "whole-range CIVAC" premise:

- Userspace rounds each request to 256-byte atoms inside the BO and refuses to
  wrap (`X9PanVKMemory.c:109-119`; atom = `X9_RENDER_CACHE_ATOM`).
- Deferred map syncs are merged only for the **same BO and same operation**
  (`X9PanVKMemory.c:143-162`), so an invalidate cannot be reordered past a clean
  or across a possible alias.
- A submit flushes pending map syncs before taking the device lock
  (`X9PanVKMemory.c:321-322`), which is required because the cache call takes the
  native lock itself.
- The kernel re-validates the 256-byte alignment and BO bounds
  (`SweetX9RenderService.cpp:323-329`) and executes a span plan built per
  resource, split into clean-before and invalidate-after phases with holes
  excluded and sorted/merged (`X9RenderCachePlan.inc:32-85`).
- Write-combined, coherent and never-exposed BOs are skipped by plan rules;
  imported host memory is never cleaned.

So maintenance is scoped to the resources in the submission's plan, not to the
whole address space and not to a fixed range. The remaining imprecision is the
plan's reachability scope (which resources count as part of a submission), which
is the same limitation already recorded for Task 6.

`SOURCE CONFIRMED`. No cache code was modified in Task 4.

## 6. Fence and completion semantics

- A userspace fence is immutable except `pending → complete`
  (`X9PanVKSync.c:11`).
- Waits use a condition variable with the caller's deadline and no fixed sleep
  (`X9PanVKSync.c:162-195`). The deadline is passed through from Vulkan; an
  infinite wait maps to `pthread_cond_wait`, a timed wait to
  `pthread_cond_timedwait_relative_np`.
- Output payloads are allocated before any object is mutated, so a reservation
  failure cannot publish a partial batch (`X9PanVKSync.c:269-278`).
- The timeline is signalled only after `x9_pan_kmod_submit` returns success,
  which itself requires `completed_id == submission_id` and a clean completion
  record (`X9PanVKMemory.c:327-337`). A failed or mismatched submit marks the
  provider failed (`X9PanVKSync.c:281-283`) rather than signalling.
- Kernel `render_wait` is a non-blocking lookup in the 64-entry completion
  history (`SweetX9RenderEngine.inc:593-598`); it never sleeps. The synchronous
  completion already happened inside `render_submit`.
- BO wait is coarse: it waits for the last submission of the whole connection,
  not for the last use of that BO (`X9PanVKMemory.c:165-183`). This is
  conservative (never returns early), but it is not a per-BO fence. `NEEDS WORK`
  for Task 6.
- There is no external sync fd import/export; both directions return `ENOTSUP`.
  Vulkan semaphores/fences are the only cross-queue mechanism.

`SOURCE CONFIRMED`. No fence or submission code was modified in Task 4.

## 7. Enforcement

`x9/build/scripts/check-native-boundary.py` scans sources only and fails the
build on a violation. Two policies:

- `--panvk`: roots are Mesa `src/panfrost/vulkan` and `x9/metal/General/panvk`.
  IOKit, IOSurface, display-service and XNU-VM/libkern includes are forbidden
  everywhere; the transport header and `x9_render_*` names are allowed only in
  `x9/metal/General/panvk/`.
- `--metal`: root is `x9/metal/General`. IOKit and the transport are allowed only
  in `X9RenderTransport.c` and `bridge/NativeBackendProbe.mm`; `panvk/`, the
  standalone `probe/` tool and the uncompiled `X9RenderResources.*` helper are
  excluded because they are not part of the shipped bundle.

Wiring:

- `build-panvk-native-ios.py:26` runs `--panvk` after third-party patches are
  applied and before the budget prep / meson configure.
- `build-general-metal.py:40` runs `--metal` before the Rust bridge is compiled.

Both were exercised end to end in this task (§8). A negative self-test (a
temporary `panvk/__boundary_selftest.c` containing `#include <IOKit/IOKitLib.h>`)
made `--panvk` exit 1 as intended and was then removed.

Limitation, stated explicitly: this is a **direct-dependency** check. It does
not follow transitive includes and does not inspect object undefined symbols.
The link-level property recorded in `phase-1-report.md` §8 (factory/frontend
objects have zero IOKit and zero `x9_render_*` undefined symbols) is still
verified by hand, not by this script. `NOT IMPLEMENTED` for CI.

## 8. Build result

`BUILD VERIFIED` for both userspace artefacts; no kernel build, no packaging.

```
python3 x9/build/scripts/build-general-metal.py --revision 230
  native boundary clean (metal): 25 sources
  Generic Metal runtime and iOS16 bundle compiled/linked/signed; ...

python3 x9/build/scripts/build-panvk-native-ios.py
  Native PanVK dynamic library linked; no device execution or rendering validation
```

- The Metal run produced only the two pre-existing `X9FrameCapture.inc:147,148`
  warnings (`NBBuffer *` passed where `id<MTLBuffer>` is expected).
- The PanVK run was a no-op rebuild (`[1/5]` generate + link), because no PanVK
  source changed; the check still ran and passed before meson.
- `python3 -m py_compile` passes for all three scripts.

`NOT TESTED`: no device run, no install, no boot, no flash.

## 9. Findings and gaps

Ordered by risk. Nothing here was fixed in Task 4 unless stated.

1. **Patch 00 is path-locked and not reproducible.** `x9/build/third_party/00-mesa-panvk-x9.patch`
   embeds absolute paths under `/Users/tosasitill/HelioDarwin-x9-lk-preserve/...`
   (lines 9, 399-404, 422). The retained tree was manually rewritten to
   `ios_plj110` and no script in `x9/build` applies patch 00; the build assumes
   the retained tree is already correct. A fresh tree restored by applying patch
   00 would point at a different and possibly stale source. `NEEDS AUDIT` →
   decide between a path-rewriting prepare step and a regenerated patch. Not
   changed here because it is build plumbing, not GPU behaviour.
2. **BO wait is connection-wide** (`X9PanVKMemory.c:165-183`). Correct but
   coarse; per-BO last-use tracking is Task 6 work.
3. **Two mapping tables must stay in step.** Userspace mirrors 4096 mappings; the
   kernel accepts up to `X9_RENDER_KERNEL_MAPPINGS` (4096) and the ABI still
   advertises 1024. The kernel re-validates each unmap, so a divergence fails
   cleanly rather than corrupting state, but the mirror can overflow on a kernel
   that advertises 1024. `SOURCE CONFIRMED`; limitation, not a regression.
4. **The ABI header sits under a kernel directory.** `X9PanVKMemory.h:4`
   includes `../X9RenderABI.h`. This is a POD contract, not an XNU dependency,
   but its location invites accidental kernel-private additions. Recommend a
   shared `include/` home when the ABI is next revised. `NOT IMPLEMENTED`.
5. **Boundary check is source-level only** (§7). Object-level enforcement would
   catch a transitive include or a symbol pulled from another TU.
   `NOT IMPLEMENTED`.
6. **No external sync fd** and one VM / one group remain as recorded in the
   inventory; unchanged.
7. **Display is untouched.** PanVK has no display include and no GPU direct
   scanout path; display remains the kernel-side CPU composition path. Task 7
   still pending.

## 10. Rollback

Task 4 adds one script and two build-step lines; no runtime source changed.

```
rm x9/build/scripts/check-native-boundary.py
# remove the two check-native-boundary.py invocations at
#   x9/build/scripts/build-panvk-native-ios.py:26
#   x9/build/scripts/build-general-metal.py:40
```

Rollback is compile-only; no kernel, PanVK, ABI, manifest or device state is
involved.
