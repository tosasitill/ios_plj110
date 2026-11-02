# Rev231 Metal Frontend Latency Audit

Scope: `X9 METAL` telemetry in `/Users/tosasitill/2.txt`, correlated with
[NativeBridgeMetalFrontend.mm](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm)
at the current working tree (Rev231 plus two uncommitted changes). The log was
produced by the Rev231 binary, which does **not** contain the histogram fix.

## 1. Source map

| Symbol / row | Location | Meaning |
|---|---|---|
| `nowUs()` | [NativeBridgeMetalFrontend.mm:56](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L56) | `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000`; monotonic microseconds since boot |
| `Timed` | [:143-151](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L143) | count/total/slowest plus 32 log2-µs buckets; `record()` is the working-tree fix |
| `timedPercentile()` | [:152-158](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L152) | returns the bucket's upper bound (`p95<=…`) |
| `CommitRecord` | [:165](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L165) | `committedUs`, `callbackUs`, `ageUs`, `scheduledUs`, ring index, encoded work |
| `didSchedule` | [:1201-1208](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1201) | stores commit→scheduled duration and adds it to `gTimeQueue` |
| `commit` | [:1255-1298](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1255) | commit entry, ref acquire, ring publish, in-flight insert, runtime commit |
| `completed` | [:1321-1336](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1321) | completion callback; computes `took`, records ctc, sets `callbackUs` |
| `waitUntilCompleted` | [:1300-1307](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1300) | caller block, timed by `J completed` |
| `waitUntilScheduled` | [:1308-1317](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1308) | caller block, timed by `J scheduled` |
| Telemetry table | [:2072-2131](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L2072) | rows `S/T/G/Q/J/V/W/K/E` |
| Reader | [:106-133](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L106) | opt-in `x9_telemetry=1`/`x9t`, every 120 completions, utility queue |

## 2. Field dictionary

### K row

`K120 t=73.534s rp8 dr130 kpx7721 cs0 bl0 ctc=16725us`
([2.txt:3642](/Users/tosasitill/2.txt))

- `t` — `committedUs`, absolute monotonic µs/1000, i.e. seconds since boot.
- `rp` — render passes encoded in that command buffer (`encodedWork.passes`).
- `dr` — draws encoded.
- `kpx` — first-colour-attachment pixels in thousands.
- `cs` — compute dispatches.
- `bl` — blits.
- `ctc` — `callbackUs` = completion callback time − commit entry time, µs.

Only the newest 20 ring entries are printed per snapshot (ring size 32,
[:2126-2128](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L2126)).

### E row

`E1080 age=8830us scheduled=21774us` ([2.txt:6914](/Users/tosasitill/2.txt))

- `age` — commit time − command-buffer creation time; includes caller idle.
- `scheduled` — commit time → scheduled callback duration (same interval as
  `gTimeQueue`), **not** an absolute timestamp.

Only the newest 4 entries per snapshot are printed.

### G / T / Q / J

- `G ctc` — histogram of every completion since process start; in Rev231 the
  histogram is empty, so it collapses to `max` (see §4).
- `T submit` — duration inside `nmb_command_buffer_on_completed` +
  `on_scheduled` + `nmb_command_buffer_commit`, i.e. registration and submit
  call only ([:1289-1292](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1289)).
- `T queue` — commit entry → scheduled callback (`gTimeQueue`).
- `Q inflight` — maximum and current command buffers between commit and
  completion.
- `J scheduled` / `J completed` / `J resource` — cumulative durations of
  explicit caller waits; `checks/skipped` count managed-resource fast paths.
- `S` — cumulative passes/draws/full-screen passes/biggest pass (Ki px)/
  dispatches/blits.

## 3. `ctc` start, end, and included stages

**Start:** `self.committedUs=nowUs()` at commit entry
([:1260](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1260)).

Between start and the runtime commit call the frontend does:

1. GPU-visible buffer acquire for every owned buffer/texture
   ([:1264-1269](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1264)).
2. Ring index assignment and `CommitRecord` publication under `gCommitLock`
   ([:1271-1278](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1271)).
3. In-flight insertion (:1280-1285).
4. Callback registration and `nmb_command_buffer_commit`
   ([:1289-1291](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1289)).

**End:** the first statement of the runtime completion callback, before the
telemetry dispatch or `finish` ([:1325-1331](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1325)).
Thus `ctc` includes:

- frontend commit bookkeeping and buffer-ref acquisition;
- the runtime's queue wait for the serialized engine;
- GPU execution up to the completion signal;
- completion-callback dispatch latency.

It **excludes**:

- all Metal encoding before `commit()` (passes, draws, blits, `setBytes`);
- pipeline/library/function creation, which happens earlier and is measured
  separately by `T pipeline/library/function` and `V vkpipe`;
- the caller's own frame pacing before commit; `E age` covers that.

`ctc` is therefore a **commit-to-callback latency**, not shader execution time
and not GPU busy time. Confidence: **HIGH** (source ordering is explicit).

## 4. Measured distribution

The Rev231 `G` row is unusable: at 86.823 s it reports
`n=1080 p50<=48541us p95<=48541us p99<=48541us max=48ms`
([2.txt:6857](/Users/tosasitill/2.txt)). The cause is the missing histogram:
`completed()` increments `count`/`total` directly and `timedPercentile()` has
no buckets, so it returns `slowest`. `git show 646b7cd` confirms Rev231's
`Timed` had no `record()`/buckets. The working-tree fix adds them; it is not
in the tested binary. Confidence: **HIGH**.

To recover a usable distribution, the 180 raw `K` rows (20 newest × 9
snapshots, 41.548–86.825 s) were sorted directly, bypassing the broken
histogram:

| n | min | p50 | p90 | p95 | p99 | max | mean |
|---|---|---|---|---|---|---|---|
| 180 | 6,384 µs | 15,602 µs | 22,816 µs | 27,621 µs | 43,629 µs | 47,101 µs | 15,978 µs |

`rp` mean 8.8, `dr` mean 88.4, `kpx` mean 8,940 (thousand px), `cs` 0.0,
`bl` 0.1. Every sampled command buffer is graphics-only.

`E` (36 samples): age p50 5,826 µs, p95 17,320 µs, max 35,158 µs; scheduled
p50 14,704 µs, p95 37,922 µs, max 40,253 µs.

## 5. Per-stage interpretation

- `T submit` p95 ≤ 511 µs, max 3 ms at 86.823 s
  ([2.txt:6860](/Users/tosasitill/2.txt)). Submission bookkeeping is not the
  bottleneck.
- `T queue` p95 ≤ 32,767 µs, max 48 ms. Because percentiles are bucket upper
  bounds, the true p95 lies in (16.384 ms, 32.767 ms].
- `ctc` p50 15.602 ms is almost identical to the `J scheduled` mean of
  16.09 ms (`total=17373ms / 1080`), and `Q inflight max=2`. A single
  serialized engine slot is the dominant term, not encoding and not the submit
  call.
- The minimum `ctc` of 6.384 ms is already 12.5× the p95 submit cost, so even
  the best case spends milliseconds after submission returns. This is direct
  evidence against reading `ctc` as CPU submission cost.
- The kernel-side `wait` phase (GPU fence poll) averages 6.16 ms and rises to
  14.59 ms in the last window
  ([2.txt:7231](/Users/tosasitill/2.txt)); it is the closest available proxy
  for GPU execution, but includes fence-cache acquisition and polling.

The exact queue-vs-GPU split per commit cannot be computed: `T queue` and
`ctc` are separate distributions, not paired samples. A paired sample would
require storing `scheduledUs` and `callbackUs` in the same K row; `E` stores
`scheduled` only for the newest four commits.

## 6. Sampling bias

1. **Single process.** Only backboardd emitted tables (1,080 commits); no
   SpringBoard or mediaserverd frontend samples exist.
2. **Recency-weighted K sample.** 20 of 32 ring entries per 120-completion
   snapshot → 180/1,080 commits (16.7%), biased to the newest commits at each
   snapshot. It spans the session but is not a uniform sample.
3. **E sample.** Only four newest commits per snapshot → 36 samples, dominated
   by the current frame's command buffers.
4. **G row.** Broken in Rev231 (all percentiles = max).
5. **No frame identity.** Rows carry a commit index, not a display frame or
   surface ID; commit and present cannot be correlated in the log.
6. **Logging is outside the interval.** Telemetry is emitted from a utility
   queue after the callback records its timestamp, so NSLog I/O does not
   inflate `ctc`; it can still delay the next snapshot.

## 7. Missing telemetry

- Encode start/end timestamps (to separate encoding from commit).
- Paired `scheduledUs`/`callbackUs` per commit (to split queue wait from GPU).
- A queue-depth histogram (only max/current in-flight are reported).
- Driver/firmware GPU timestamps or fence signal time (none are surfaced).
- Present/flip timing in the kernel, keyed by the same commit or surface ID.
- Per-process aggregation; SpringBoard's own commits are absent.
- Pipeline/function compile events correlated with the first draw that needed
  them (the aggregate `V` row cannot attribute a stall to a frame).
- Histogram buckets finer than powers of two for 10–50 ms; the current
  `p95<=` bound can be 2× wide.

## 8. Conclusions

| # | Conclusion | Confidence |
|---|---|---|
| L1 | `ctc` = commit entry → completion callback; includes queue wait, GPU execution, callback dispatch; excludes encoding | HIGH |
| L2 | Rev231's `G ctc` percentiles are invalid (empty histogram); raw K rows give p50 15.60 ms / p95 27.62 ms / p99 43.63 ms | HIGH |
| L3 | Submission bookkeeping is cheap (`T submit` p95 0.51 ms); serialized queue/GPU latency dominates | HIGH |
| L4 | One serialized slot (`Q max=2`) and 16 ms mean caller waits indicate frame-level serialization | MEDIUM |
| L5 | The Rev231 log cannot split queue wait from GPU execution; new paired telemetry is required | HIGH |

No production code was modified.
