# Rev231 Submission and Cache Performance

Scope: correlate `X9 RENDER timing` and cache rows with PanVK explicit cache
requests and the Metal frontend `ctc/queue` measurements. Log is
`/Users/tosasitill/2.txt`; sources are the working tree.

## 1. Measurement map

| Measurement | Clock / endpoint | Source |
|---|---|---|
| frontend `T submit` | registration + `nmb_command_buffer_commit` only | [NativeBridgeMetalFrontend.mm:1289-1292](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1289) |
| frontend `T queue` | commit entry → scheduled callback | [NativeBridgeMetalFrontend.mm:1201-1208](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1201) |
| frontend `ctc` | commit entry → completion callback | [NativeBridgeMetalFrontend.mm:1327-1331](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1327) |
| kernel `owner` | submit entry → render-owner lock held | [SweetX9RenderEngine.inc:270-277](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L270) |
| kernel `pre_cache` | render-owner lock held → CPU cache maintenance before GPU | [SweetX9RenderEngine.inc:277-363](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L277) |
| kernel `storage` | stream setup, publish, private storage clean | [SweetX9RenderEngine.inc:363-398](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L363) |
| kernel `start` | GPU flush/AS install/group start | [SweetX9RenderEngine.inc:398-424](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L398) |
| kernel `wait` | completion-fence poll loop | [SweetX9RenderEngine.inc:424-452](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L424) |
| kernel `retire` | group suspend, final flush, AS detach | [SweetX9RenderEngine.inc:452-459](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L452) |
| kernel `post_cache` | GPU quiescent → post cache maintenance | [SweetX9RenderEngine.inc:459-479](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L459) |

All kernel phases are means over a 2 s window (`elapsed>=2000000000`), printed
as `total[i]/count/1000` microseconds
([SweetX9RenderEngine.inc:497-511](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L497)).
They are **not shader execution times**; the emitter says so explicitly.

## 2. Kernel phases

31 windows, 1,332 jobs, 81.3 s of reported elapsed time:

| Phase | Mean µs | Min µs | Max µs | Share of summed mean |
|---|---|---|---|---|
| owner | 1.5 | 1 | 15 | 0.0% |
| pre_cache | 3,021 | 129 | 4,837 | 25.0% |
| storage | 338 | 79 | 362 | 2.8% |
| start | 536 | 262 | 569 | 4.4% |
| wait | 6,156 | 346 | 15,953 | 51.2% |
| retire | 354 | 190 | 446 | 2.9% |
| post_cache | 1,605 | 18 | 2,641 | 13.3% |
| **sum** | **12,011** | | | **100%** |

`max_job` has mean 20,792 µs and maximum 36,686 µs, but it is the maximum
whole-job time in each window, not a mean; it should not be compared directly
to the phase means.

Late-window behaviour shows the same ranking with a growing wait:

| Time | jobs | pre | wait | post | max_job |
|---|---|---|---|---|---|
| 86.494 | 76 | 4,295 | 3,944 | 2,320 | 33,372 |
| 88.531 | 50 | 4,683 | 11,607 | 2,585 | 32,932 |
| 90.549 | 40 | 4,797 | 14,589 | 2,606 | 29,760 |

([2.txt:6826](/Users/tosasitill/2.txt), [2.txt:7070](/Users/tosasitill/2.txt),
[2.txt:7231](/Users/tosasitill/2.txt)).

At the end of the run, `wait` is 62.6% of the measured service time
(14,589 / 23,299 µs) and `pre_cache + post_cache` is 31.8%. Earlier in the run
the split is more balanced (3,944 wait vs 4,295+2,320 cache at 86.494).

## 3. Cache scope and bytes

Job-weighted means over all 1,332 jobs (each window's average weighted by its
job count):

- `pre_bytes_avg` = **59.2 MB/job**, max window average 83.4 MB/job.
- `post_bytes_avg` = **33.5 MB/job**.
- Late windows are higher: 78.0 MB pre / 47.4 MB post at 90.550 s.

Scope rows at 90.550 s:

```
cache-scope:  jobs=40 private_pre_skip_avg=0 private_post_skip_avg=0
              initial_clean_avg=0 full_fallback_avg=77992755
cache-reach:  pre_skip_avg=0 post_skip_avg=0
              import_pre_avg=77992755 import_pre_full_avg=77992755
              import_post_avg=47371059 import_post_full_avg=47371059
cache-coherent: active=0
cache-manifest: pre_spans=1217 post_spans=1105 holes_saved=0/0
cache-plan:   builds=38 reuses=2
cache-wc:     pre_skip_avg=362615193 post_skip_avg=362369433
```

([2.txt:7232-7238](/Users/tosasitill/2.txt)). Reading:

- Every pre-cache byte is attributed to imported mappings; private allocations
  are all skipped. `initial_clean_avg=0` means no submission qualified for the
  full-BO initial-clean shortcut.
- `full_fallback_avg == pre_bytes_avg` means the fallback is the whole reach;
  there is no partial dirty tracking.
- 1,217 pre spans and 1,105 post spans with zero holes: the kernel is cleaning
  the exact union of mapped ranges, not the whole resource blindly. This is a
  precise implementation, but of *reach*, not of *dirtiness*.
- Write-combined resources skip ~362 MB per job of cache maintenance, about
  6× the 59 MB actually cleaned. WC is therefore a large win that is already
  active, not a remaining optimization.
- Cache plan reuse is low late (38 builds, 2 reuses) because mapping mutation
  invalidates the plan; shared cache ops are still per submission.

## 4. PanVK explicit ranges

`X9 PANVK CACHE RANGE` is emitted for explicit `x9_pan_kmod_sync_cpu` and
deferred map flushes:

- 74,496 calls by 90.628 s; latest window `range_bytes_avg=54,681`,
  `bo_bytes_avg=62,937`, `merged=17,882`
  ([2.txt:7243](/Users/tosasitill/2.txt)).
- Ranges are BO-relative, rounded outward to cache atoms, clamped to the BO,
  and merged only for the same BO and same operation when
  adjacent/overlapping ([X9PanVKMemory.c:109-120](/Users/tosasitill/ios_plj110/x9/metal/General/panvk/X9PanVKMemory.c#L109),
  [X9PanVKMemory.c:141-163](/Users/tosasitill/ios_plj110/x9/metal/General/panvk/X9PanVKMemory.c#L141)).
- Average range is 87% of average BO size, so merging is not hiding a large
  over-clean at this layer; the kernel's separate post-cache scope is larger
  because it covers all GPU-write mappings.

These counters describe cache operations the PanVK layer requested. They do
not describe GPU work, a complete dirty set, or bytes saved.

## 5. CPU cache maintenance implementation

[`X9RenderMemory.cpp:123-149`](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/X9RenderMemory.cpp#L123)
walks ranges in 64 KiB windows with preemption disabled:

- `dc civac` for clean+invalidate,
- `dc cvac` for clean,
- `dc ivac` for invalidate,
- four instructions per loop iteration, then `dsb sy` per window.

Counters in the log (`batches=344,064`, `runs=2,107,763`) show that CPU
migration safety multiplies the number of cache instruction runs; the kernel
comment accepts this tradeoff. Post-cache is clean-only for GPU-write
resources; the `dc civac` path also handles speculative clean copies before
GPU access.

Removing or weakening this is not justified by the log: coherent mode is
inactive, imported pages are shared aliases, and no dirty tracking exists.
Any change needs a proven coherency contract, not just a latency budget.

## 6. Dominant measured costs and owners

| Rank | Cost | Owner | Size | Evidence |
|---|---|---|---|---|
| 1 | Serialized completion wait / queue | Metal runtime + kernel fence poll | ctc p50 15.6 ms; kernel wait 6.2 ms mean, 14.6 ms late | [2.txt:6857](/Users/tosasitill/2.txt), [2.txt:7231](/Users/tosasitill/2.txt) |
| 2 | Pre-cache CPU maintenance | kernel, over imported mapping union | 3.0 ms mean, 4.8 ms late; 59 MB/job | [2.txt:7231](/Users/tosasitill/2.txt) |
| 3 | Post-cache CPU maintenance | kernel, GPU-write resources | 1.6 ms mean, 2.6 ms late; 33 MB/job | [2.txt:7231](/Users/tosasitill/2.txt) |
| 4 | GPU start / storage / retire | kernel CSG | 0.34–0.54 ms each | [2.txt:7231](/Users/tosasitill/2.txt) |
| 5 | Metal submission bookkeeping | frontend | p95 0.51 ms, max 3 ms | [2.txt:6860](/Users/tosasitill/2.txt) |

The frontend `ctc` includes the queue and GPU; the kernel `wait` is inside the
runtime's submit worker. The two are consistent: a ~15.6 ms median
commit-to-callback with a ~6 ms GPU wait leaves roughly 9 ms of queue and
callback latency, but the log has no paired samples to prove that split.

## 7. Correlations and limits

- `T submit` is cheap, so optimizing registration/commit cannot move `ctc`.
- `T queue` p95 ≤ 32.8 ms and `Q inflight max=2` point at a single serialized
  lane rather than a deep pipeline.
- Cache maintenance is 31–46% of measured kernel service time depending on the
  window; it is significant but not the majority at the end of the run.
- The kernel wait loop is synchronous and runs under the render-owner lock
  ([SweetX9RenderEngine.inc:273-277](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L273),
  [SweetX9RenderEngine.inc:424-451](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L424)).
  Two submissions cannot overlap in the kernel, regardless of the frontend's
  in-flight count. This is the structural reason the queue wait is long.
- The kernel phases are means; a few slow jobs are invisible except in
  `max_job`. Percentiles are required before claiming tail improvements.

## 8. Missing instrumentation

1. Per-phase histograms (p50/p95/p99), not only means.
2. Pre-cache bytes split by imported, private, write-combined, and
   CPU-exposed; post-cache split the same way.
3. Time inside `x9_render_memory_cache` versus plan build/reach computation;
   the current `pre_cache` phase mixes both.
4. Queue depth and wait time histograms in the frontend, paired with the same
   commit index.
5. GPU fence signal timestamp (if the firmware can expose it) so kernel `wait`
   can be split into GPU execution versus cache-acquire/poll latency.
6. Cache plan rebuild cause (which mapping mutation) and the cost of the
   rebuild itself.

No production code was modified.
