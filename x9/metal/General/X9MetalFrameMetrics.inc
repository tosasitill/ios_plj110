// SPDX-License-Identifier: MIT
// DRAFT -- not compiled in the preparation environment (no iOS SDK / plugin
// build chain there). Written against the rev284 frontend sources read for
// this draft; expect one compile-fix iteration on the build machine.
//
// X9MetalFrameMetrics.inc -- frame-rate / stutter / retention metrics for
// the X9 Metal frontend (M4 step a; docs/graphics/gpu-optimization-headroom.md
// SS4 P3, SS1 "NOT MEASURED").
//
// What exists today and this file builds on:
//  - native_bridge_metal_telemetry() already streams a table of CPU-side
//    stage histograms (pipeline/library/function/ctc/submit/queue/waits)
//    plus a 32-entry per-commit ring, every 120 completions, opt-in via
//    the x9_telemetry=1 / x9t boot arg. There is no rate, no stutter and
//    no dropped-record accounting anywhere (SS1: "no FPS/stutter metric,
//    no dropped-event count").
//  - The kernel has per-job GPU timestamps (X9 RENDER GPUTS, x9gt=1) and
//    7-phase means (X9 RENDER timing), but the completion record
//    (X9RenderCompletion, X9RenderABI.h) carries no timestamps, so a
//    GPU-timestamp-to-stage join needs an ABI extension; that is M4
//    step b, not this file.
//
// What this file adds, all derived from the frontend's own completion
// stream (completed() in NativeBridgeMetalFrontend.mm):
//  - a windowed completion rate ("fps"), one window per telemetry
//    emission. HONEST SCOPE: a completion is a command buffer, not a
//    presented frame; presents happen in the render server / kernel
//    flip path, which this process cannot see. The completion stream
//    is the only in-process frame proxy -- rev231's "9 frames/s" was
//    derived from job counts the same way. A present-side FPS belongs
//    to the kernel display path (step b) and must not be faked here.
//  - stutter: inter-completion gaps over 25 / 50 / 100 ms (1.5x / 3x /
//    6x the 60 Hz frame budget), plus the longest gap. Gaps are only
//    folded into the cumulative counters when their window was active
//    (at least one commit), so an idle desktop or a locked phone does
//    not manufacture "stutter"; idle windows are counted separately.
//    This is the static-desktop half of P3's row separation; the
//    lockscreen half needs a signal this process does not have.
//  - dropped: commit-ring records that fell out of the printed tail
//    (20 of 32) between two emissions and were therefore never
//    reported -- P3's "event retention window and dropped count",
//    scoped to the one event stream that exists today.
//
// Counters are exported in the same extern "C" volatile style as M3's
// x9_prime_* so a host reader can poll them without the telemetry
// boot arg. All state is atomics; the completion path does no locking,
// no allocation and no logging.

extern "C" {
volatile uint32_t x9_frame_windows, x9_frame_active_windows,
    x9_frame_idle_windows, x9_frame_fps_x100, x9_frame_gap_max_ms;
volatile uint64_t x9_frame_gap25, x9_frame_gap50, x9_frame_gap100,
    x9_frame_dropped, x9_frame_completions;
}

#include <atomic>
#include <cstdio>

namespace x9frame {

static std::atomic<uint64_t> gLastCompletionUs{0};
static std::atomic<uint64_t> gPendingGap25{0}, gPendingGap50{0},
    gPendingGap100{0}, gPendingGapMaxMs{0};
static std::atomic<uint64_t> gCompletions{0};
// Snapshot-side state; only the telemetry emitter touches these.
static uint64_t sLastSnapshotUs = 0, sLastCompletions = 0,
    sLastCommits = 0;
static bool sHaveSnapshot = false;

// Called from completed() with the frontend's nowUs()
// (CLOCK_UPTIME_RAW microseconds). One atomic load, one store, and
// atomics only on the rare gap path.
static inline void noteCompletion(uint64_t now) {
  gCompletions++;
  const uint64_t prev = gLastCompletionUs.exchange(now);
  if (!prev || now <= prev) return;
  const uint64_t gapMs = (now - prev) / 1000;
  if (gapMs <= 25) return;
  gPendingGap25++;
  if (gapMs > 50) gPendingGap50++;
  if (gapMs > 100) gPendingGap100++;
  uint64_t seen = gPendingGapMaxMs.load();
  while (gapMs > seen &&
         !gPendingGapMaxMs.compare_exchange_weak(seen, gapMs)) {
  }
}

} // namespace x9frame

// Called once per telemetry emission from native_bridge_metal_telemetry
// with nowUs() and the current gCommits value. Formats the single "F"
// row (kept under the table's 90-character row limit) and rotates the
// window. Returns false for the very first call, which only establishes
// the baseline. Not thread-safe by design: the emitter is a single
// utility-queue block per emission, as with the rest of the table.
static bool x9_frame_metrics_snapshot(char *out, size_t size,
                                      uint64_t now, uint64_t commits) {
  using namespace x9frame;
  const uint64_t completions = gCompletions.load();
  if (!sHaveSnapshot) {
    sHaveSnapshot = true;
    sLastSnapshotUs = now;
    sLastCompletions = completions;
    sLastCommits = commits;
    // Discard gaps that predate the baseline.
    gPendingGap25.exchange(0);
    gPendingGap50.exchange(0);
    gPendingGap100.exchange(0);
    gPendingGapMaxMs.exchange(0);
    return false;
  }
  const uint64_t windowUs = now - sLastSnapshotUs;
  const uint64_t doneDelta = completions - sLastCompletions;
  const uint64_t commitDelta = commits - sLastCommits;
  sLastSnapshotUs = now;
  sLastCompletions = completions;
  sLastCommits = commits;

  // Retention: the table prints the newest 20 ring records; anything
  // older that arrived since the previous emission was never reported.
  if (commitDelta > 20) x9_frame_dropped += commitDelta - 20;

  const uint64_t p25 = gPendingGap25.exchange(0);
  const uint64_t p50 = gPendingGap50.exchange(0);
  const uint64_t p100 = gPendingGap100.exchange(0);
  const uint64_t pMax = gPendingGapMaxMs.exchange(0);

  x9_frame_windows++;
  const bool active = commitDelta > 0;
  uint32_t fpsX100 = 0;
  if (active) {
    x9_frame_active_windows++;
    if (windowUs)
      fpsX100 = (uint32_t)(doneDelta * 100000000ull / windowUs);
    x9_frame_fps_x100 = fpsX100;
    x9_frame_gap25 += p25;
    x9_frame_gap50 += p50;
    x9_frame_gap100 += p100;
    if (pMax > x9_frame_gap_max_ms)
      x9_frame_gap_max_ms = (uint32_t)pMax;
  } else {
    x9_frame_idle_windows++;
  }
  x9_frame_completions = completions;

  const int n = snprintf(
      out, size,
      "F fps=%u.%02u win=%llums %s gap25=%llu gap50=%llu gap100=%llu "
      "max=%llums drop=%llu",
      fpsX100 / 100, fpsX100 % 100,
      (unsigned long long)(windowUs / 1000), active ? "act" : "idle",
      (unsigned long long)x9_frame_gap25,
      (unsigned long long)x9_frame_gap50,
      (unsigned long long)x9_frame_gap100,
      (unsigned long long)x9_frame_gap_max_ms,
      (unsigned long long)x9_frame_dropped);
  return n > 0 && (size_t)n < size;
}
