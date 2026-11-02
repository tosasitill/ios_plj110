# Rev231 Runtime Audit

Read-only analysis of the Rev231 boot log at `/Users/tosasitill/2.txt`
(9.59 s – 95.14 s kernel timestamps, 7,275 lines). No production source,
kernel source, shader, allowlist, or build file was modified for this audit.

Companion reports:

- [rev231-metal-latency.md](rev231-metal-latency.md)
- [rev231-wsi-pitch.md](rev231-wsi-pitch.md)
- [rev231-buffer-lifecycle.md](rev231-buffer-lifecycle.md)
- [rev231-submission-performance.md](rev231-submission-performance.md)

Confidence labels used throughout: **HIGH** (source condition and matching log
evidence), **MEDIUM** (source condition or log evidence, one side inferred),
**LOW** (hypothesis consistent with data but not proven).

## 1. What was running

| Item | Evidence | Confidence |
|---|---|---|
| Plugins | `X9GeneralMetal231` active at 3.276 s, `X9PanVK231` loaded at 8.418 s ([2.txt:585](/Users/tosasitill/2.txt), [2.txt:1142](/Users/tosasitill/2.txt)) | HIGH |
| Telemetry gate | Only `x9t` was present; the reader in [NativeBridgeMetalFrontend.mm:106-133](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L106) requires `x9_telemetry=1` or exact `x9t` | HIGH |
| Telemetry cadence | every 120 completions, first table at 41.548 s, last at 86.825 s; 1,080 backboardd commits (`S c1080/0` at [2.txt:6845](/Users/tosasitill/2.txt)) | HIGH |
| Telemetry scope | only `backboardd` emitted `X9 METAL` rows (9 snapshots); no SpringBoard/mediaserverd rows | HIGH |
| Policy boot args | no HSR/FPK/index/serialize tokens; all policy lines show `mode=1` defaults (HSR `all-bound-RTs-written`, FPK `all-bound-RTs-really-written`), e.g. [2.txt:1235](/Users/tosasitill/2.txt), [2.txt:1238](/Users/tosasitill/2.txt) | HIGH |
| Kernel work | 31 timing windows, 1,332 submitted jobs across all clients, 81.3 s of reported elapsed windows (the 1,080 frontend commits are backboardd only) | HIGH |

Rev231 is commit `646b7cd` ("add opt-in telemetry reader and pin rev 231
allowlist"). Its diff touched only `NOTES.md`, two existing reports, the
allowlist, and +40 lines in the Metal frontend that add the reader. It touched
no kernel, PanVK, HSR/FPK, shader, or display code.

The working tree currently contains two **uncommitted** changes that are *not*
in the Rev231 binary:

1. `NativeBridgeMetalFrontend.mm`: `Timed::record()` histogram fix (the
   Rev231 binary still uses `Timed::add` with only `count`/`total`).
2. `x9/kernel/generated/x9_render_external_manifest.h`: pinned to 232.

All frontend line numbers below are from the working tree. Line numbers for
the Rev231 binary behaviour are called out explicitly.

## 2. Confirmed problems

### C1. The Rev231 `G ctc` percentiles are meaningless

`G ctc n=1080 p50<=48541us p95<=48541us p99<=48541us max=48ms`
([2.txt:6857](/Users/tosasitill/2.txt)). P50 = P95 = P99 = max is the signature
of an empty histogram: `timedPercentile()` falls back to `slowest` when all
buckets are zero ([NativeBridgeMetalFrontend.mm:152-158](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L152)).
Rev231's `Timed::add` only incremented `count`/`total` (`git show 646b7cd`),
while `completed()` sampled commit-to-callback manually. The fix is in the
working tree but not device-verified. Confidence: **HIGH**.

### C2. `ctc` is not GPU execution time

`ctc` starts at `self.committedUs=nowUs()` inside `commit()`
([NativeBridgeMetalFrontend.mm:1260](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1260))
and ends in the runtime completion callback
([NativeBridgeMetalFrontend.mm:1327-1331](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1327)).
Between those two points the code acquires buffer references, publishes the
ring entry, inserts into the in-flight set, registers callbacks, commits to the
serialized engine, waits in that engine's queue, executes on the GPU, and
dispatches the callback. Encoding is *before* `commit()` and is not included.
The fastest sampled value is 6,384 µs while `T submit` p95 is 511 µs
([2.txt:6860](/Users/tosasitill/2.txt)), so at least ~5.9 ms elapses after the
submission call returns. Calling `ctc` "GPU time" is unsupported. Confidence:
**HIGH**.

### C3. Presentation still copies; there is no swapchain

`AppleCLCD::directPresent()` validates the surface, then calls `flipFrom()`
(copy into the kernel flip buffer) or `scanFrom()`
([SweetFramebuffer.cpp:1523-1531](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L1523)).
The name is historical; the panel is not scanned out from the client
IOSurface. There is no `VkSwapchain`/`VK_KHR_swapchain` object in the tree;
presentation is an IOSurface ID consumed by the kernel. Confidence: **HIGH**.

### C4. WSI pitch refusals are real and reproducible on 16-bpp SpringBoard surfaces

274 `WSI pitch not properly aligned` errors: 265 from backboardd, 9 from
SpringBoard; first at 15.910 s, last at 87.600 s. The proven case is
`SpringBoard create id 63 78x120 ... bpr 160` at 33.230 s followed immediately
by two errors at 33.230/33.245 s, then `id 65` with the same geometry at
33.288 s followed by errors at 33.289/33.296 s
([2.txt:2636](/Users/tosasitill/2.txt), [2.txt:2733](/Users/tosasitill/2.txt)).
160 is not a multiple of the 64-byte linear row alignment required by PanVK
v14 ([pan_mod.c:965-971](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/mesa-render-source/src/panfrost/lib/pan_mod.c#L965),
[pan_layout.h:172-238](/Users/tosasitill/ios_plj110/.local/x9/work/x9-metal-render/mesa-render-source/src/panfrost/lib/pan_layout.h#L172)).
Confidence: **HIGH** for the 78x120/160 case; **MEDIUM** for the remaining 265
backboardd errors, whose surfaces are not logged.

### C5. Kernel submission still blocks on the GPU fence

`render_submit()` times `times[4]` before the completion loop and `times[5]`
after it ([SweetX9RenderEngine.inc:424-452](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc#L424)),
and the loop polls the private fence pages until `submission_id` and the user-IO
words match. The `wait` phase is therefore GPU execution plus fence-cache
acquisition and polling, not a pure fence wait. Its 31-window mean is 6,156 µs
and its final window is 14,589 µs ([2.txt:7231](/Users/tosasitill/2.txt)).
Confidence: **HIGH** for what the code does; the "GPU execution" portion is
**MEDIUM** because fence polling is included.

### C6. `X9 RENDER LIVE` exposes every resource as concurrently mapped

For pid 57, `resources` equals `mappings` in every one of 28 samples
(1,219/1,219 at 90.325 s, [2.txt:7221](/Users/tosasitill/2.txt)). `mappings` is
`context->mapping_count`, incremented only by `X9RenderMap`
([SweetX9RenderService.cpp:307-320](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L307)),
and `X9RenderRelease` refuses while `mapping_exists(r)` is true
([SweetX9RenderService.cpp:281](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L281)).
Releases do occur (`window release=26/46MiB` at 90.325 s), so the equality is
not proof that all resources are unreleasable. It is proof that the current
counters cannot distinguish a healthy mapped working set from retention.
Confidence: **HIGH** for the counter equality; **LOW** for any leak
interpretation.

## 3. Suspected problems that need more instrumentation

### S1. Backboardd's 265 pitch refusals have no surface attribution

`X9 SURFACE layout` and `X9 TEXTURE routes` log only imports 1–8 and every
128th import ([NativeBridgeMetalFrontend.mm:1699-1714](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1699)).
Only the 1256x2760/bpr 5120 surface is visible. That pitch is 64-byte aligned,
so it cannot explain the errors; any other client surface is invisible.
Confidence: **MEDIUM**.

### S2. Imported read-only buffers dominate pre-cache scope

Late windows report `import_pre_avg=77,992,755` against
`pre_bytes_avg=77,992,755`, and `initial_clean_avg=0`
([2.txt:7232-7234](/Users/tosasitill/2.txt)). Every submission cleans the full
mapped import union, even though those pages enter through a read-only grant
([X9PanVKMemory.c:384-418](/Users/tosasitill/ios_plj110/x9/metal/General/panvk/X9PanVKMemory.c#L384)).
Whether the clean is required depends on speculative CPU lines and aliasing,
which the log cannot prove. It is a candidate cost, not a defect. Confidence:
**MEDIUM**.

### S3. `old30s` grows while total MiB stays bounded

pid 57 goes from 1 resource/0 MiB at 9.590 s to 1,219/410 MiB at 90.325 s;
`old30s` rises from 0 to 680 resources/257 MiB
([2.txt:1205](/Users/tosasitill/2.txt), [2.txt:7221](/Users/tosasitill/2.txt)).
There are zero `why=budget` reports and zero disabled/exposed clients. This is
consistent with a long-lived pool crossing the 30 s age threshold, but the
sampling window is only ~90 s and there is no resource-age histogram or
release-attempt counter. Confidence: **LOW** for a leak; **HIGH** that the
current telemetry cannot answer it.

### S4. The serialized engine queue dominates commit-to-callback

`T queue p95<=32767us`, `T submit p95<=511us`, `Q inflight max=2`,
`J scheduled wait n=1080 total=17373ms` (mean 16.09 ms)
([2.txt:6860-6869](/Users/tosasitill/2.txt)). The queue latency distribution
is coarse (bucket upper bounds) and there is no queue-depth histogram, so the
queue-vs-GPU split inside `ctc` cannot be measured. Confidence: **MEDIUM**.

## 4. Issues not supported by the evidence

- **"Rev231 regressed the Control Center platter."** The log contains zero
  `ControlCenter`, `Control Center`, or `platter` strings; the only SpringBoard
  GPU evidence is the texture-route snapshot at 33.279 s. Rev231 changed no
  rendering path. The platter must be recorded as NOT TESTED in this run, not
  fixed or regressed. Confidence: **HIGH** that the claim is unsupported.
- **"There is a buffer/memory leak."** Resources plateau at 1,219 and total
  bytes fall from 442 to 410 MiB between 88.272 s and 90.325 s; releases
  continue in every window. No budget refusal, no uncertain submission, no
  disabled context. Confidence: **HIGH** that a leak is unproven.
- **"Cache maintenance is unnecessary and can be skipped."** No coherency
  contract or GPU-snoop evidence exists in the log; `cache-coherent active=0`
  and the opt-in path was never enabled. Removing CIVAC/CVAC would be an
  unverified behaviour change. Confidence: **HIGH** that the recommendation
  would be unsupported.
- **"`ctc` is shader/GPU execution time."** See C2. Confidence: **HIGH**.
- **"The first pitch error is caused by the 1256x2760 surface."** That surface
  has bpr 5120 (64-byte aligned) and the first error follows a 196,608-byte
  host import, not the 1256 allocation. The pairing is temporal only.
  Confidence: **HIGH** that the 1256 claim is wrong; **LOW** that the 196,608
  import is the culprit.

## 5. Platter regression check against Rev225–230

The Control Center platter lost-triangle bug is documented in
[NOTES.md:219-347](/Users/tosasitill/ios_plj110/NOTES.md#L219) as present since
Rev225 and unresolved. This run added no CC marker to the log (0 matches for
`ControlCenter`, `Control Center`, `platter`), so the platter itself was **not
exercised observably**. The check below is therefore a code/telemetry-policy
comparison, not a rendering verdict.

| Dimension | Rev225–230 state | Rev231 change | Rev231 log evidence | Verdict |
|---|---|---|---|---|
| Shader variants | Rev228 A/B found no Kraid-vs-BIR codegen difference; Rev229 replay varied vertices, not shaders | none (diff touches no shader source or variant table) | `S` shows normal passes; no shader-variant row exists | unchanged |
| Framebuffer fetch | FPK path active; Rev221/224/225 isolated FPK draws | none | `FPK policy mode=1` at 9.732/31.111 s; totals 65,536 draws, 762 kills at 81.364 s ([2.txt:6337](/Users/tosasitill/2.txt)) | unchanged |
| HSR / DCD | Rev218 all-RTs cull fix; Rev224 zero-area cull kept as no-op | none | `HSR policy mode=1`; totals 65,536 draws, can_cull 3,927, blocked 16,205 ([2.txt:6338](/Users/tosasitill/2.txt)) | unchanged |
| FPK write masks | Rev221 honours colour write masks | none | `rt_mask`, `written`, `effective` rows unchanged in shape ([2.txt:5218](/Users/tosasitill/2.txt)) | unchanged |
| Vertex/index handling | Rev220 widens tight index ranges; Rev229 replay | none | `X9 PANVK IB WIDEN` active; `IDX KIND` rows show `isize=2` ([2.txt:1677](/Users/tosasitill/2.txt)) | unchanged |
| Render-target layout | no boundary task changed RT layout | none | no RT-layout source or log row changed | unchanged |
| Texture import routes | Rev230 added Buffer Manager metadata registration only | none (Rev231 adds only the telemetry reader) | routes `direct/pitched/shadow` counters present, same policy; 274 pitch errors | unchanged mechanism, new visibility |

`git show 646b7cd` (Rev231) modifies only `NOTES.md`, two reports, the
allowlist, and the +40-line reader. `git show 681f64e` (Rev230) changes the
display backend, Buffer Manager, and frontend metadata/ref bookkeeping, but no
shader, HSR/FPK, MRT, vertex/index, or render-target-layout code. The Rev231
log shows the default policy modes with no A/B tokens.

Conclusion: there is no source or capture evidence that Rev231 changed platter
rendering, so neither a regression nor a fix can be claimed. The platter remains
NOT TESTED in this run. Confidence: **HIGH** for the code comparison;
**LOW** for any statement about the physical platter.

## 6. Three highest-priority implementation tasks

### T1. Land and verify the ctc histogram, then add stage timestamps

Finish the already-written `Timed::record()` fix, then add an encode-end
timestamp and expose queue-to-scheduled and scheduled-to-complete latencies as
separate histograms. Do not add GPU timestamps until the driver can supply
them; do not label any value "GPU time".

Minimal validation: one idle ~60 s run on Rev232 with `x9t`; `G ctc` must show
`p50 < p95 < p99 < max`, `K`-row samples must fall inside the G quantiles, and
`T queue + (callback - scheduled)` must bracket `ctc` per sampled commit.

### T2. Attribute and fix the 64-byte pitch gap

Log one line per imported surface (IOSurface ID, width, height, bpp, bpr,
plane offset, allocation, route) instead of the 8/128 sampling, and make the
presentation path choose a 64-byte-multiple row pitch or the shadow route
explicitly for 16-bpp surfaces. Do not change the PanVK alignment rule.

Minimal validation: the 78x120/bpr 160 surface must report a named route
(`direct`, `pitched`, or `shadow`) with no `WSI pitch not properly aligned`
within the same import; after the change the 274-error count must be 0 for
surfaces whose bpr is a multiple of 64, and the shadow count must not grow
silently.

### T3. Make the kernel wait and lifecycle counters measurable per job

Emit a per-window histogram (not just a mean) for `wait`, `pre_cache`, and
`post_cache`, plus counters for release attempts, release refusals by reason,
and resource age buckets. Keep the synchronous fence loop unchanged until the
histograms exist.

Minimal validation: after 60 s idle plus one Control Center open, the log must
show wait/pre/post histograms and release-refusal counts; `resources`,
`mappings`, and `old30s` must be explainable from the new counters, with no
behaviour change to submission or cache maintenance.

## 7. Report index

| Report | Scope | Key numbers |
|---|---|---|
| [rev231-metal-latency.md](rev231-metal-latency.md) | `X9 METAL` K/E/G/T/Q/J, ctc definition, percentiles, bias | ctc p50 15.60 ms, p95 27.62 ms, p99 43.63 ms |
| [rev231-wsi-pitch.md](rev231-wsi-pitch.md) | Metal texture → IOSurface → PanVK image → kernel present | 274 pitch errors, 64-byte rule |
| [rev231-buffer-lifecycle.md](rev231-buffer-lifecycle.md) | `X9 RENDER LIVE`, refs, release policy | 1,219 resources/410 MiB, no budget refusal |
| [rev231-submission-performance.md](rev231-submission-performance.md) | kernel phases, cache scope, PanVK ranges | wait 6.16 ms mean, pre 3.02 ms, post 1.61 ms |

No production code was changed. Analysis stops here for review.
