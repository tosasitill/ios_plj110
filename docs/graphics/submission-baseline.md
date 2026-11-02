# Submission Baseline and Optimization Boundary (Task 6)

Status date: 2026-11-02. Revision under construction: 230 (`feat/G1Ultra`, HEAD
`5b3caad`). No device run, no kernel, PanVK, ABI, cache-plan or fence change.

## 1. Why this is step 1 only

Spec §6 orders the work as baseline → lock split → submission queue → resource
set → cache precision → fence wait → before/after comparison, with one main
variable changed at a time. Two facts constrain this increment:

1. The submission is synchronous by contract. `render_submit()`
   (`SweetX9RenderEngine.inc:269`) performs cache maintenance, address-space
   install, doorbell, fence polling and suspend/detach inside one call, and the
   PanVK backend requires `completed_id == submission_id`
   (`X9PanVKMemory.c:318`). There is no completion object to queue behind until
   the ABI v2 design from Task 2 is implemented.
2. Every remaining step changes kernel synchronization. Without a device
   baseline, a "faster" claim would violate the spec's prohibition on
   presenting unverified results.

Therefore Task 6 in this tree implements **observability only**. Steps 2–6 are
designed below and marked `NOT IMPLEMENTED`.

## 2. Current implementation (`SOURCE CONFIRMED`)

### 2.1 Kernel submit path

`render_submit()` already timestamps eight phases and logs them every 2 s of
accumulated jobs:

| Log field | Meaning |
|---|---|
| `owner_us` | time inside `RenderOwner` before the first phase checkpoint |
| `pre_cache_us` | pre-submit cache maintenance |
| `storage_us` | stream packet construction and private storage sync |
| `start_us` | address-space install, user-IO prepare, group start, doorbell |
| `wait_us` | completion polling (GPU execution plus fence refresh) |
| `retire_us` | group suspend, flush, address-space detach |
| `post_cache_us` | post-CIVAC for GPU-write resources |
| `max_job_us` | slowest whole call in the interval |

Cache byte counts are logged separately (`pre_bytes_avg`, `post_bytes_avg`,
`private_*_skip_avg`, `fallback_avg`, `import_*`, `hole_*`, `wc_*`). All values
are averages plus a maximum; no percentile existed.

### 2.2 Userspace path

`NativeBridgeMetalFrontend.mm` already kept `Timed` counters (count, total,
slowest) for pipeline/library/function creation, `commit → completion`
(`gTimeEncodeToCommit`), `waitUntilScheduled`, `waitUntilCompleted`, and a
32-entry commit ring with per-command-buffer `ctc` and `scheduled` latency.

Missing relative to §6.7:

- P50/P95/P99 of commit-to-completion.
- Submit-call duration (`nmb_command_buffer_commit`).
- Queue latency (`commit → scheduled`) as its own series.
- In-flight depth (queue depth proxy).
- Timeout/reset counters. These live only in the kernel (`uncertain`, no reset,
  no retry) and are not exported through the ABI; they remain `NOT
  IMPLEMENTED` here rather than being synthesized.

## 3. What was added (`BUILD VERIFIED`)

`x9/metal/General/NativeBridgeMetalFrontend.mm`:

| Location | Change |
|---|---|
| `:105-121` | `Timed` now keeps a 32-bucket `floor(log2(µs))` histogram; `timedPercentile()` returns the **upper bound** of the bucket containing the rank. |
| `:120` | New series `gTimeSubmit`, `gTimeQueue`; new `gInFlightMax`. |
| `:1162` | `didSchedule` records commit → scheduled latency in `gTimeQueue`. |
| `:1238-1250` | `commit` records in-flight depth after insertion and times `nmb_command_buffer_commit` in `gTimeSubmit` (failed commits included). |
| `:2045-2053` | Telemetry rows: `G ctc … p50/p95/p99`, `T submit`, `T queue`, `Q inflight`. |
| `:106-138` | Rev 231 opt-in reader: exact boot token `x9_telemetry=1` or its 3-byte alias `x9t`; every 120 completions logs the table on a utility queue as `X9 METAL <row>`. Off by default. |
| `:1290-1293` | `completed()` triggers the reader after its own bookkeeping, outside the scoped commit lock. |

Percentiles are deliberately reported as `<=`: the value is the upper edge of
the containing power-of-two bucket, not an exact sample. The slowest exact
sample remains available from the `max` field.

No Metal API result, ordering, retention, cache operation, mapping, fence or
ABI is changed. Overhead is a few atomics per commit/didSchedule plus one
`clock_gettime_nsec_np` around `nmb_command_buffer_commit`.

## 4. How to collect a baseline (`GAP FOUND ON DEVICE`)

Correction after the first rev 230 boot log (2026-11-02): the table was not
streamed anywhere. `native_bridge_metal_telemetry` and
`native_bridge_metal_status` are exported by the bundle, but neither the kernel
overlay nor any userspace tool in this repository called them, and the frozen
XNU tree contains no reference either. `X9 RENDER timing` is a different,
kernel-side series emitted by `render_submit`; it is real and was captured, but
it does not contain the `G`/`T`/`Q` rows.

Rev 231 adds the reader in the plugin: exact boot token `x9_telemetry=1` (short
alias `x9t`, because the payload has only ~5 spare boot-arg bytes), one table
every 120 completed command buffers, logged on a utility queue. With the token
absent the reader never runs. Userspace baseline state remains `NOT COLLECTED`
until a rev 231 boot with the token is captured.

1. Boot the candidate with `x9t`, leave the desktop idle for 60 s.
2. Open/close Control Center and one app.
3. Capture the `X9 RENDER timing`, `X9 RENDER cache`, and `Q timing` rows.
4. Use at least one continuous 5-minute session; compare P95/P99, not means.

The expected rows are:

```text
X9 SUBMIT  G ctc n=… p50<=…us p95<=…us p99<=…us max=…ms
X9 SUBMIT  T submit n=… p95<=…us max=…ms
X9 SUBMIT  T queue n=… p95<=…us max=…ms
X9 GPU     Q inflight max=… now=…
X9 CACHE   X9 RENDER cache: … pre_bytes_avg=… post_bytes_avg=…
```

## 5. Ordered plan for steps 2–6 (`NOT IMPLEMENTED`)

| Step | Current blocker | Entry criterion | Change site |
|---|---|---|---|
| 2 Lock split | one `metal_service_lock` covers owner, cache, submit and wait; display only TryLocks it | baseline P95 + lock-hold accounting | `SweetX9CSF.cpp:657`, `SweetX9RenderEngine.inc` |
| 3 Submission queue | submit is synchronous; PanVK rejects a job unless it already completed | ABI v2 `SubmitAsync/FenceWait/Query` (Task 2 design, not implemented) | `SweetX9RenderEngine.inc:269`, `X9PanVKMemory.c:318` |
| 4 Resource set | kernel maintains all mapped resources, not the current stream's set | per-submission read/write set from PanVK | `SweetX9RenderEngine.inc:344-480`, `X9PanVKSubmitPlan.c` |
| 5 Cache precision | current plan is the exact union of all GPU mappings; correct but broad | resource-set tracking plus device validation of every transition | `X9RenderCachePlan.inc`, `X9RenderMemory.cpp` |
| 6 Fence wait | kernel polls a private fence page with bounded deadlines; no completion interrupt path | confirm CSF interrupt availability for this hardware | `SweetX9RenderEngine.inc:447-467` |

Each row is a separate change with its own revert. None may be combined with
another until the previous one has device data.

## 6. Non-goals in this increment

- No change to CIVAC/CLEAN/INVALIDATE placement or range.
- No change to `flush_gpu`, `install_as`, `group_start/halt`, fence value checks
  or the `0xa5` guard.
- No change to the single-command-buffer serialization model.
- No claim that the baseline itself improves performance.
