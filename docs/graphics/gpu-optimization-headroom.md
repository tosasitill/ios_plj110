# GPU Optimization Headroom — X9 (MT6993 / Arm Avalon) vs UniXNU Adreno740 playbook

Read-only evaluation. No production code was changed by this report.

Inputs:
1. https://unixnu.com/zh/porting/gpu — "GPU 与 Metal 适配" (Adreno740 / SM8550 port). Its
   optimization rules are quoted below as (a)…(g).
2. Our stack: `x9/kernel/src/iokit/Kernel/arm/` (abbrev **K**), `x9/metal/General/` (**M**).
3. The vendor Android GPU driver for the *same silicon*:
   `/Users/tosasitill/HelioDarwin/oppo-source/modules/vendor/mediatek/kernel_modules/gpu/`
   (**V**). MT6993 builds `gpu_mali/mali_avalon/a16w` = Arm DDK **r54p1-11eac0**, CSF front-end
   (`V/a16w/config/cflags.bzl:77`, `V/a16w/config/fragment/mt6993.fragment:5`
   `CONFIG_MALI_CSF_SUPPORT=y`, `:6,11` real HW + large pages). Ideas only — that tree is the
   Android side and is not linkable into XNU.

Verification level of every claim is labelled: **DEVICE** = from a log the device produced,
**CODE** = read from the working tree, **VENDOR** = read from the Android driver, **INFER** = my
reasoning.

## 1. Measured envelope (what we are optimizing against)

**DEVICE**, rev231 log, 1,332 jobs / 31 windows ([rev231-submission-performance.md](/Users/tosasitill/ios_plj110/docs/graphics/rev231-submission-performance.md) §2):

| kernel phase | mean µs | share | what it contains (CODE) |
|---|---|---|---|
| owner | 1.5 | 0.0% | render-owner lock |
| pre_cache | 3,021 | 25.0% | CPU cache maintenance before GPU |
| storage | 338 | 2.8% | stream setup, publish, 64 KiB ring clean, 64 GPU cache cmds/stream |
| start | 536 | 4.4% | `flush_gpu()` + `install_as()` + group start |
| wait | 6,156 | 51.2% | completion-fence poll loop |
| retire | 354 | 2.9% | group halt + `flush_gpu()` + `remove_as()` |
| post_cache | 1,605 | 13.3% | cache maintenance after GPU |
| **sum** | **12,011** | | |

- **DEVICE** 9 frames/s at 23% GPU utilisation, ~10 ms/job ([K/sweet_x9_gpu_dependencies.c:220](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/sweet_x9_gpu_dependencies.c#L220)).
  INFER: we are overhead-bound, not shader-bound.
- **DEVICE** 59.2 MB pre + 33.5 MB post cache-maintenance bytes per job; 74,496 cache-range calls,
  54,681 B average range vs 62,937 B average BO; WC skipping saves ~362 MB/job; rev246 measured
  ~82 MB cleaned + ~47 MB invalidated per job across all imports.
- **DEVICE** frontend `ctc` (commit→callback) p50 15,602 µs, p95 27,621 µs, p99 43,629 µs,
  max 47,101 µs; `T queue` p95 ≤32,767 µs, max 48 ms; in-flight `Q max=2`
  ([rev231-runtime-audit.md](/Users/tosasitill/ios_plj110/docs/graphics/rev231-runtime-audit.md), [rev231-metal-latency.md](/Users/tosasitill/ios_plj110/docs/graphics/rev231-metal-latency.md)).
- **DEVICE** table ceilings: 1,024-entry mapping table full at 65 s (~900 BOs)
  ([K/X9RenderKernel.h:8-13](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/X9RenderKernel.h#L8));
  512 MiB budget reached in <1 min with no recovery
  ([K/SweetX9RenderService.cpp:126-128](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L126)).
- **NOT MEASURED**: there is no FPS/stutter metric, no GPU-timestamp→stage join, no dropped-event
  count anywhere in `docs/graphics`. `first-frame-experiment-plan.md` is design-only, every entry
  still `predicted`.

## 2. Rule-by-rule against the UniXNU playbook

| # | UniXNU rule (Adreno740) | X9 today | Verdict |
|---|---|---|---|
| a | BOs = pageable **write-combined** descriptors; page attributes set once per batch, restored on free | WC is the default (`kIOMapWriteCombineCache`, [K/SweetX9RenderService.cpp:139-143](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L139), `:226-230`) ✔. But every fresh WC BO is scrubbed with `dc civac` over its **whole** length at alloc ([K/SweetX9RenderService.cpp:28-42](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L28), call `:254`), attributes are never restored on free (`X9RenderRelease` `:302-312`), and a per-page re-assert walk exists off-by-default (`x9_wc_check`, [K/X9RenderMemory.cpp:14-37](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/X9RenderMemory.cpp#L14)) | **partly met**; residual cost is alloc-time scrub + no reuse |
| b | own memory mapped with a **batch exclusive** SMMU call (no alias check); imports batch-mapped per cache category | No SMMU at all — GPU MMU translates with SMMU in bypass ([K/SweetX9CSF.cpp:234-236](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9CSF.cpp#L234)). PanVK accepts **IMMEDIATE only**, one `X9RenderMap` IOKit round-trip per op, 4 KiB pages only ([M/panvk/X9PanVKMemory.c:206-207](/Users/tosasitill/ios_plj110/x9/metal/General/panvk/X9PanVKMemory.c#L206), `:274-286`) | **not applicable as written**; the equivalent need (batched, alias-check-free binding) is unmet |
| c | unmap: 1 TLBI per page ≤512 pages, ASID-wide above | No per-page or per-ASID TLBI anywhere. The only invalidation is whole-AS `AS+0x18 UPDATE` bracketed by `idle_as()` polls ([K/SweetX9CSF.cpp:237-246](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9CSF.cpp#L237), `:350-377`) | **absent**, replaced by something far coarser |
| d | **no TLBI on the submit path**; mapping fills invalid leaves only; unmap invalidates its own pages; upper levels untouched | Leaf-fill-is-invalid-only ✔ ([K/X9RenderPageTable.cpp:93-104](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/X9RenderPageTable.cpp#L93)); but `prune()` rewrites upper-level entries (`:50-64`, `:113-114`), and submit does `flush_gpu()`+`install_as()` … `flush_gpu()`+`remove_as()` — 2 full L2+LSC flushes and 4 AS-idle waits **per job** ([K/SweetX9RenderEngine.inc:400](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L400), `:460`) | **violated**; this is the structural difference |
| e | object lookup = handle-indexed, bounded by memory | Handles are monotonic but lookup is a linear scan; 5 unconditional `for(auto &r:c->resources)` O(4096) walks per submit ([K/SweetX9RenderService.cpp:167-183](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L167); [K/SweetX9RenderEngine.inc:339](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L339), `:465`; [K/X9RenderCachePlan.inc:63](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/X9RenderCachePlan.inc#L63)), plus O(count≤1152) PA scan **per level per leaf** ([K/X9RenderPageTable.cpp:39](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/X9RenderPageTable.cpp#L39)) | **absent** |
| f | 2 rings by priority; FIFO across priority; preemption saves tile memory; **interactive QoS** on scheduler + completion threads | 1 immutable 16 KiB ring per stream, 3 streams/group, ≤8 groups; `priority` is validated 0..1 and only written into the CSG request word; no QoS/priority call exists in these files; submit is synchronous submit-**and**-wait under one global lock ([K/SweetX9RenderEngine.inc:25-29](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L25), `:425-451`; [M/panvk/X9PanVKMemory.c:165-184](/Users/tosasitill/ios_plj110/x9/metal/General/panvk/X9PanVKMemory.c#L165) `wait_bo` waits for the last submission of the whole connection) | **absent** |
| g | idle own-image backing pooled as **spares**, aging at 10 s, reclaimed on failure/pressure; spare before new block | No client-BO pool anywhere. `X9RenderRelease` frees immediately ([K/SweetX9RenderService.cpp:302-312](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L302)); PanVK `bo_free` frees immediately ([M/panvk/X9PanVKMemory.c:80-89](/Users/tosasitill/ios_plj110/x9/metal/General/panvk/X9PanVKMemory.c#L80)); `X9BufferManager` is metadata-only by design ([M/X9BufferManager.h:2-5](/Users/tosasitill/ios_plj110/x9/metal/General/X9BufferManager.h#L2)); desktop pool is fixed 2 buffers, disabled after 3 presented / 8 attempts / one >100 ms ([K/SweetX9DesktopWorker.inc:26-31](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9DesktopWorker.inc#L26), `:122-128`) | **absent** |
| 4 | pipeline cache **pre-warm**: `__DATA,__primelist` of every `.metallib`, background thread 90 s after device open, busy=8 CBufs/250 ms, resumable `prime-*` progress, build-id = runtime build + driver `LC_UUID`, optional pipeline pack | No priming of any kind. `MTLCompilerService`-side compile is **synchronous and joined by the caller** ([M/NativeMetalCompiler.inc:6-18](/Users/tosasitill/ios_plj110/x9/metal/General/NativeMetalCompiler.inc#L6)); persistent caches do exist (shader cache keyed on LC_UUID+request bytes, `:19-45`,`:130-137`; Vulkan cache at `/private/var/mobile/Library/Caches/X9GeneralMetal`, [M/X9GeneralMetalFactory.mm:38](/Users/tosasitill/ios_plj110/x9/metal/General/X9GeneralMetalFactory.mm#L38)) but only *after* first use; `VkPipeline` is created at draw time ([M/NativeBridgeMetalFrontend.mm:2225-2235](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L2225)) | **absent** — biggest qualitative gap for stutter/first frame |
| e′ | 8-bit BGRA fb: sysmem+nocb default, **lazy transient attachments kept in tile memory**, memoryless stays in tile, MSAA forced to 1× | BGRA8 ✔, but tile shaders are refused outright (`supportsTileShaders`=no, `tileWidth/Height`=0, [M/NativeBridgeMetalFrontend.mm:942-949](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L942)) while `supportsMemorylessRenderTargets=1` is advertised ([M/bridge/NativeMetalObjects.rs:472-473](/Users/tosasitill/ios_plj110/x9/metal/General/bridge/NativeMetalObjects.rs#L472)); `VK_IMAGE_CREATE_TRANSIENT`/`MEMORY_LAZY` never appear in `M/panvk`; captured drawable-sized passes are `load1/store1` (43 in rev212) and `DontCare` still stores ([first-frame-rendering-audit.md](/Users/tosasitill/ios_plj110/docs/graphics/first-frame-rendering-audit.md) §2.1, [control-center-vs-folder-rendering.md](/Users/tosasitill/ios_plj110/docs/graphics/control-center-vs-folder-rendering.md) §2.1-2.2); MSAA not forced | **absent** |
| f′ | frame diagnostics: 8 named stages, CPU uptime + calibrated GPU timestamps, per-interaction A/B, static-desktop and lockscreen rows separate | Stages exist as bucketed logs (age/queue/submit/ctc/inflight/cache/WSI, [M/NativeBridgeMetalFrontend.mm:143-163](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L143)) and 8 kernel phase means, but GPU counter sets are **not joined** to the stage chain ([M/NativeMetalCounters.inc:1-50](/Users/tosasitill/ios_plj110/x9/metal/General/NativeMetalCounters.inc#L1)) and no interaction-keyed comparison exists | **partial**; the missing half is what the other six rules would need as a gate |

## 3. What the vendor driver for the same GPU does (copyable, cited)

1. **Never invalidates on submit.** Invalidation is per-AS, **VA-ranged**, and happens on
   map/unmap/fault paths only: `AS_COMMAND_FLUSH_PT|FLUSH_MEM` + `mmu_hw_do_lock_no_wait` +
   `wait_ready` (**V**/mmu/mali_kbase_mmu_hw_direct.c:543-608), with `op_param.flush_skip_levels`
   skipping untouched PGD levels (`mmu/mali_kbase_mmu.c:1919`) and, for `arch_major>11` (Avalon),
   **TLB-invalidate-only** instead of a full L2+LSC flush (`mali_kbase_mmu.c:357-360`,
   `1935-1938`). This is the hardware-supported answer to playbook rule (d) on *our* GPU.
2. **Hardware coherency instead of software maintenance.** Coherency comes from the DT
   (`of_dma_is_coherent()` → `COHERENCY_ACE_LITE`, **V**/mali_kbase_core_linux.c:4818-4859);
   MT6993 uniquely enables `CONFIG_MALI_MTK_COHERENT_DMA_BUF=y` (**V**/config/fragment/mt6993.fragment:101)
   and attaches coherent dma-bufs to a separate `mali_dma_coherent` device
   (`mali_kbase_mem_linux.c:1617-1708`). CPU-side maintenance is only `dma_sync_*` for page tables
   when non-coherent (`mali_kbase_cache_policy.c:53-64`); pool pages are synced **once at
   allocation** (`mali_kbase_mem_pool.c:222,298,674`). INFER: the ~93 MB/job of CIVAC/IVAC we pay
   is what the vendor does not pay on this SoC.
3. **Pools with an RT refill thread.** 4 KiB + 2 MiB pools per context/device, 64 MiB caps
   (`mali_kbase_mem.h:992-1006`), plus MTK **mGMM**: rank-aware pools, `RANK_POOL_LIMIT=256MB`,
   `REFILL_TARGET=32MB`, `PREFILL_TARGET=64MB`, boot prefill, and a `SCHED_FIFO` prio-2
   `mgm_refill_worker` that speculatively allocates/splits order-10 blocks
   (**V**/../base/arm/memory_group_manager/memory_group_manager.c:43-46, 666-738, 964-995, 1450-1478).
   Reclaim is shrinker-driven above the refill target only.
4. **2 MiB GPU pages.** `CONFIG_LARGE_PAGE_SUPPORT=y` → `LARGE_PAGE_AUTO`, and
   `KBASE_HW_FEATURE_LARGE_PAGE_ALLOC` is enabled only for the Avalon tDRx family
   (`mali_kbase_hwconfig_features.h:156-186`, `mali_kbase_hw.c:88-97`).
5. **RT threads for the hot kernel paths.** `mali-gpuq-kthread` + `mali-kcpuq-kthread` promoted to
   `SCHED_FIFO` prio 2 (**V**/csf/mali_kbase_csf_scheduler.c:8983-8996, 9010-9013); page-fault/JIT/OOM
   workqueues converted to FIFO kthreads (`:9158-9198`, fragment:105); Linux process priority is
   re-mapped onto GPU group priority (`mali_kbase_csf.c:4347-4360`); HW supports 4 group priorities
   and per-queue priority ≤15 (`mali_base_csf_kernel.h:163-169`, `mali_kbase_csf.c:549,622`).
6. **Aggressively shortened suspend timeout.** `CSF_CSG_SUSPEND_TIMEOUT_CYCLES` = **150M cycles**
   vs upstream 3100M, under `CONFIG_MALI_MTK_TIMEOUT_OPT=y`
   (**V**/mali_kbase_config_defaults.h:202-218, fragment:98). INFER: directly targets the latency of
   the halt/suspend step we execute on every job.
7. **Reuse-window anti-thrash + tiler-heap reclaim.** JIT regions used within
   `jit_reclaim_timeout_ms` (default **1000 ms**) are skipped by reclaim
   (`mali_kbase_mem_linux.c:829-839`, `mali_kbase_mem.h:276`); tiler-heap reclaim runs at
   `DEFAULT_SEEKS+2`, batch 512, off-slot timeout 30 s/2048 pages, and MTK adds
   **page-fault-triggered forced reclaim** (`csf/mali_kbase_csf_tiler_heap_reclaim.c:29-35,416-424`).
8. **PBHA/SLC per-buffer cache-partition hints** written into PTEs via PDMA 8-bit PBHA
   (`mmu/mali_kbase_mmu.c:2486-2501`, `mali_kbase_mem_linux.c:475`, fragment:92).
9. **DVFS delegated, not local.** A "dummy" devfreq governor applies only thermal OPP caps
   (`platform/mtk_platform_common/mtk_platform_devfreq_governor.c:63-108`); real DVFS goes to GED
   with 3D/TA/COMP utilisation split and a 26 MHz perf-counter hint
   (`mtk_platform_dvfs.c:116-138, 379-392`), async DVFS (`backend/gpu/mali_kbase_pm_metrics.c:190-211`),
   runtime-PM autosuspend 100 ms default clamped to 10-25 ms + adaptive power policy
   (`platform/mt6993/mali_kbase_config_mt6993.c:51-56, 414-490`), bandwidth voting
   (`CONFIG_MALI_MTK_GPU_BM_2=y`, `mali_kbase_core_linux.c:5760-5764`).

## 4. Ranked optimization space

Ranked by measured share of the 12 ms/job service time, then by risk.

**P0-A — Decouple submission from completion (targets `wait` 51%).**
Today: `externalMethod` holds the context lock for the whole call
([K/SweetX9RenderService.cpp:463-472](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L463)),
the engine re-takes the process-wide `metal_service_lock`, then polls fences in a `do{}while` for up
to 10 s with `poll_pause()` and `sleep_ms()` granularity ≥1 ms
([K/SweetX9CSF.cpp:201-224](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9CSF.cpp#L201)), and re-issues a
32 KiB GPU cache acquisition **on every spinning poll**
([K/SweetX9RenderEngine.inc:431-439](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L431)).
Playbook (f) uses two rings + interactive QoS; the vendor uses FIFO-2 kthreads and interrupts.
For us the minimum viable change is: allow ≥2 jobs in flight per connection, event/IRQ-driven fence
acquisition instead of polling, and a second priority class for display-critical work.
Expected: −3…5 ms/job. **Risk: high** (fence ordering, wedged-DMA quarantine) and the spec forbids
unverified fence/queue changes → single-variable A/B, one boot-arg gate.

**P0-B — Turn cache maintenance off *by proof*, not by assumption (targets 38%).**
`pre_cache`+`post_cache` = 4,626 µs/job on 59.2+33.5 MB. `x9_gpu_coherent=1` already skips CIVAC for
exposed buffers but is documented as an unverified assumption
([K/SweetX9RenderEngine.inc:335-342](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L335),
`:466-468`, log `:522-523`; `V` shows the SoC is coherent: `of_dma_is_coherent` +
`CONFIG_MALI_MTK_COHERENT_DMA_BUF=y` for mt6993 only). Action: run a real snoop/coherency probe
(L2/SMMU config read-back + write-then-read race across CPU/GPU on WC, cached and imported IOSurface
memory), then either (i) widen the coherent path with evidence and delete the reach-set walks, or
(ii) keep maintenance and remove the opt-in. Today's state is simultaneously the top perf item and
the top correctness risk (`refactor-issues.md` CACHE-3). Expected: −2…4 ms/job.

**P0-C — Stop tearing down the address space per job (targets `start`+`retire` 7.3% + unlocks everything else).**
The constraint is structural: `map()`/`unmap()` refuse while `vm->attached`
([K/X9RenderPageTable.cpp:96-97](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/X9RenderPageTable.cpp#L96),
`104-105`), and `attached` is only false because submit detaches the AS
([K/SweetX9RenderEngine.inc:400](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L400),
`:460`; [K/SweetX9CSF.cpp:350-377](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9CSF.cpp#L350)). So mapping
cannot overlap execution, and each job pays 2× `FLUSH_CACHES 0x23304` L2+LSC plus 4 `idle_as()`
polls. The same hardware supports ranged, non-teardown invalidation (**V** item 1). Replace
install/detach with: keep AS installed per connection; on PT change issue `AS UPDATE` with
VA-range FLUSH_PT/FLUSH_MEM (invalidate-only on Avalon); make leaves editable while attached by
only writing invalid leaves and invalidating on unmap (we already satisfy the first half).
Expected: −0.9 ms/job directly, plus the enabler for P0-A/P1-A. **Risk: high** — wrong invalidation
= silent corruption; needs a page-table/TLB correctness test before flashing.

**P1-A — BO pool with aging (playbook (g), vendor item 3).**
Delete the per-alloc full-buffer `dc civac` scrub by keeping a pool of already-scrubbed WC blocks;
reuse before allocating; age out (~10 s) and reclaim under pressure. Also relieves the two ceilings
that currently break long runs (1,024 mappings at 65 s, 512 MiB budget in <1 min). Expected: −alloc
path per frame, +stability over 60 s+. **Risk: medium** (lifetime/still-mapped BO poisons the device,
[M/panvk/X9PanVKMemory.c:80-89](/Users/tosasitill/ios_plj110/x9/metal/General/panvk/X9PanVKMemory.c#L80)).

**P1-B — Mapping economics: batch bind, O(1) handles, 2 MiB pages.**
One IOKit call per 4 KiB map op today; `bind_vm` already receives a batch from Mesa and loops it
serially ([M/panvk/X9PanVKMemory.c:205-247](/Users/tosasitill/ios_plj110/x9/metal/General/panvk/X9PanVKMemory.c#L205)); leaf
lookup is O(3×1152) PA scans; five O(4096) resource walks per submit (row (e)); each PT table eats
16 KiB but indexes/cleans only its first 4 KiB
([K/X9RenderPageTable.cpp:17-27](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/X9RenderPageTable.cpp#L17),
`:125`), so the 1,152-table budget is 4× smaller than it looks. Vendor enables 2 MiB blocks for this
GPU family (item 4). Expected: removes map/unmap round-trips and the linear scans from the frame
path; raises VA capacity. **Risk: low-medium**, mostly ABI work.

**P1-C — Pipeline pre-warm + pack (playbook §4).**
Zero priming exists; compile is synchronous at first use and joined by the caller, and
`VkPipeline` is built at draw time. Port the mechanism, not the Adreno details: embed a
`.metallib` list in a plugin section, start a bounded background thread after the render server has
been up ~90 s, gate on desktop idleness (playbook: busy = >8 command buffers in 250 ms; 120 Hz idle
desktop ≈6), persist resumable `prime-*` progress, key entries on
runtime-build + Mesa `LC_UUID` so a rebuild does not silently reuse stale pipelines, and optionally
ship an offline pack. Expected: removes first-open stutter (folder / Control Center / lock screen)
rather than raising throughput. **Risk: low** (compile-only), but it competes for the boot-arg line,
which is at 607/608 bytes.

**P2-A — Tile/on-chip memory: transient + memoryless + honour `DontCare`.**
We advertise memoryless support, refuse tile shaders, and present evidence that full-size passes
round-trip system memory on load *and* store (`load1/store1`, 43 captured passes in rev212). Playbook
(e) is the same idea on GMEM; vendor item 7 shows how they manage the tiler heap under pressure.
Steps: stop storing where the consumer is `DontCare`, mark drawable-sized scratch attachments
transient/lazy so PanVK keeps them in the tiler heap, and reconsider `supportsMemorylessRenderTargets`
until it is actually implemented. Expected: removes whole-screen system-memory round-trips in blur /
platter passes. **Risk: medium** — spec §3 blocks unverified framebuffer/HSR changes, and the CC
platter bug is still open, so this must be measured against that bug, not around it.

**P2-B — Damage-aware present + direct scanout.**
`VkPresentRegionKHR` is accepted and ignored, render area is always full attachment, incremental
rendering is only tiler-OOM recovery ([damage-tracking-audit.md](/Users/tosasitill/ios_plj110/docs/graphics/damage-tracking-audit.md)
§2/§5); present still copies into the kernel flip buffer and composition is CPU
([display-backend.md](/Users/tosasitill/ios_plj110/docs/graphics/display-backend.md) §1.4). This is the remaining Task 7
work (client-surface direct scanout, hold enrollment, separate display fence, three-buffer queue).
Expected: removes one full-screen copy (1256×2760×4 ≈ 13.9 MB) per presented frame.

**P2-C — Priority/QoS + governor + suspend timeouts.**
No thread-priority code exists in the render paths (row (f)); the governor samples busy-ns every
50 ms with a 900 MHz floor ([K/sweet_x9_gpu_dependencies.c:165-235](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/sweet_x9_gpu_dependencies.c#L165));
`group_halt` waits up to 1 s while the vendor cut the equivalent suspend timeout to 150M cycles.
Adopt: raise the render/completion worker priority (playbook's interactive QoS), shorten our CSG
suspend/halt waits with a measured timeout, and feed DVFS from real per-engine utilisation.
Expected: tail latency (the 48 ms `T queue` max), not mean throughput.

**P3 — Metrics as the gate for all of the above.**
Add what the playbook's §"定位慢帧的耗时" specifies and we lack: the full stage chain with GPU
timestamps calibrated onto `CLOCK_UPTIME_RAW` and joined to the CPU stages, per-interaction A/B
pairs, separate static-desktop and lockscreen-transition rows, cache-build-id + warmup progress,
event retention window and dropped count, and an explicit FPS/stutter counter. Until then no P0/P1
claim can be called verified, and the queue-vs-GPU split inside `wait` stays unknown
(`refactor-issues.md` names it unexplained).

## 5. Deliberately not copied

- Adreno-only machinery: GMEM/`sheng-fb-bgra`, Turnip debug overrides, the two `turnip-*.patch`
  files, AIR→SPIR-V→NIR→IR3, `sheng-gpu-preempt=2` ring semantics.
- SMMU/PFU batch-exclusive mapping and the 512-page TLBI threshold: we run the SMMU in bypass and
  the Mali AS has no per-page TLBI primitive in our driver; the equivalent is ranged
  `FLUSH_PT/FLUSH_MEM` (P0-C).
- The vendor `kbase` sources themselves — Android-side, license and ABI incompatible with XNU; we
  copy mechanisms only.

## 6. Bottom line

The playbook's mapping micro-rules are only partly transferable: we already satisfy (a)'s WC
allocation and (d)'s invalid-leaf-only fill, and (b)/(c) have no counterpart because we bypass the
SMMU. The transferable, evidence-backed headroom is elsewhere and quantified against us: the
synchronous submit-and-wait model (`wait` 51%), software cache maintenance over ~93 MB/job
(38%) which the vendor avoids by using the SoC's hardware coherency, and per-job address-space
teardown (7% plus the reason mapping cannot overlap execution). Those three are ~90% of measured
kernel service time. Tile-memory/transient attachments, damage-aware present and pipeline pre-warm
form the second tier, aimed at stutter, first frame and copies rather than throughput — and the CC
platter defect is still unattributed, so it must be treated as open regardless of any refactor.
Nothing in this report was run on the device; every number above is either a rev231/rev244/rev246
device log row or a code reading, and the ordering assumes we first land the metrics in §4 P3.
