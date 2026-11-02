# Phase Report — Task 6 Submission Baseline

## 1. Scope

Establish the performance baseline required by spec §6.1–§6.7 before any lock,
queue, resource-set, cache or fence change. This phase deliberately changes
observability only: one main variable per change.

## 2. Existing Implementation

- `render_submit()` (`SweetX9RenderEngine.inc:269`) is synchronous: cache
  maintenance, address-space install, doorbell, bounded fence polling, group
  suspend and detach happen in one call under `metal_service_lock`.
- The kernel already logs eight phase timings (`owner_us`, `pre_cache_us`,
  `storage_us`, `start_us`, `wait_us`, `retire_us`, `post_cache_us`,
  `max_job_us`) plus cache byte averages every 2 s.
- The Metal frontend already kept count/total/slowest for commit-to-completion
  (`gTimeEncodeToCommit`), scheduling waits and completion waits, and a
  32-entry commit ring with per-command-buffer `ctc`/`scheduled` values.
- Missing: percentiles (P50/P95/P99), submit-call duration, queue latency and
  in-flight depth. Timeout/reset counters are kernel-only and not exported.

## 3. Changes

`x9/metal/General/NativeBridgeMetalFrontend.mm`:

| Function / region | Change |
|---|---|
| `struct Timed` `:105` | added a 32-bucket log2-microsecond histogram |
| `timedPercentile()` `:111` | returns the conservative upper bucket edge |
| `gTimeSubmit`, `gTimeQueue`, `gInFlightMax` `:120-121` | new counters |
| `didSchedule` `:1162` | records commit → scheduled latency |
| `commit` `:1238-1250` | tracks in-flight depth; times `nmb_command_buffer_commit` |
| `native_bridge_metal_telemetry` `:2045-2053` | `G ctc` percentiles, `T submit`, `T queue`, `Q inflight` rows |

## 4. Architecture

No module boundary moved. The Metal runtime now emits the `X9 SUBMIT` metric
family; the kernel remains the owner of `X9 GPU` and `X9 CACHE` phases. The
existing telemetry transport is unchanged.

## 5. ABI Impact

None (`VERIFIED` by inspection). No selector, structure, size or capability
changed.

## 6. Memory Impact

None (`VERIFIED` by inspection). Only static atomic counters in the bundle; no
allocation, mapping or lifecycle change. Estimated additional static footprint
is a few kilobytes.

## 7. Synchronization Impact

None (`VERIFIED` by inspection). New counters are C++ atomics on paths that
already execute under the frontend's existing commit synchronization. No lock
added, removed, reordered or held longer across a GPU wait.

## 8. Build Result

`BUILD VERIFIED` (userspace bundle and rev 230 package):
`build.py plugins --revision 230` rebuilt PanVK incrementally, repackaged
`X9PanVK230`, relinked `X9GeneralMetal230` with `native boundary clean (metal):
27 sources` and ad-hoc signed it; only the two pre-existing
`X9FrameCapture.inc:147,148` warnings. `package-plugins.py` rewrote the kernel
allowlist to `X9GeneralMetal230`/`X9PanVK230` (10 files, 51331072 bytes).
Adding the instrumentation did not break the bundle link.

## 9. Runtime Result

`NOT TESTED`. No device run. The new rows have not been observed in a device
log.

## 10. Performance

No measurement exists; no performance claim is made. Sampling adds one
monotonic clock read around `nmb_command_buffer_commit` and a few atomic
increments per commit (`INFERENCE`, not measured).

## 11. Regression

Compile and link only. No test suite was run (compile/package policy since
2026-10-28). Correctness of the metric labels is source-level; the values
remain `NOT TESTED` until a device log is collected.

## 12. Rollback

Revert the six regions in `NativeBridgeMetalFrontend.mm` listed in §3. The
change is isolated to telemetry and has no state that outlives the process.

## 13. Remaining Issues

1. Steps 2–6 (lock split, submission queue, resource set, cache precision,
   fence wait) are `NOT IMPLEMENTED`; they require an ABI v2 async submission
   path and a device baseline.
2. Timeout/reset counters are kernel-only and not visible to userspace.
3. Kernel phase timings still report averages and a maximum, not percentiles.
4. Bundle 230 is not deployable: the kernel allowlist still pins 229.
5. Control Center platter triangles remain `UNRESOLVED`; this phase does not
   touch rendering.
