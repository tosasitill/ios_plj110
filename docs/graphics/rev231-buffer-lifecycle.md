# Rev231 Buffer Lifecycle and Retention Audit

Scope: `X9 RENDER LIVE` accounting, buffer-manager refs, GPU/display lifetimes,
and the Rev231 log. Sources are the working tree; log is
`/Users/tosasitill/2.txt`.

## 1. Accounting source

The report is emitted by `live_report()` in
[SweetX9RenderService.cpp:146-165](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L146).
Every 2 s, or on a forced event, it walks `context->resources` and prints:

```
X9 RENDER LIVE: pid=57 why=periodic resources=1219 MiB=410 wc=1005/336MiB
imported=214/74MiB big8M=23 old30s=680/257MiB window alloc=26/14MiB
release=26/46MiB mappings=1219 budget=1024MiB
```

Fields:

| Field | Definition | Source |
|---|---|---|
| `resources` | entries with non-null `r.memory` | [:152-153](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L152) |
| `MiB` | sum of `r.bytes`, the descriptor/requested bytes | [:153](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L153) |
| `wc` | write-combined resources and bytes (no CPU cache maintenance) | [:154-155](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L154) |
| `imported` | resources created through `X9RenderImportHost` | [:154-155](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L154) |
| `big8M` | resources ≥ 8 MiB | [:157](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L157) |
| `old30s` | resources whose `created` stamp is ≥ 30 s old | [:156](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L156) |
| `window alloc/release` | counters since the previous report; reset after printing | [:164-167](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L164) |
| `mappings` | `context->mapping_count`, active client GPU mappings | [:163](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L163) |
| `budget` | `client_budget()`, 1,024 MiB in this log | [:163](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L163) |

`created` is set for **every** successful allocate/import, including imports
([:269](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L269)).
`MiB` is not physical footprint; separate `X9 RENDER FOOTPRINT` lines report
task-ledger bytes and native allocation bytes independently.

## 2. Lifetime paths

### 2.1 Kernel resource

- Created by `X9RenderAllocate` or `X9RenderImportHost`; stored in a context
  slot with `memory`, `handle`, `flags`, imported/exposed bits, and `created`.
- Released by `X9RenderRelease` only when the whole context is not `exposed`
  and no client mapping references the resource
  ([SweetX9RenderService.cpp:277-287](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L277)).
  On success it drops `cache_mapping`, completes the memory, releases it, and
  clears the slot.
- `mapping_exists()` scans the active mapping table:
  [SweetX9RenderService.cpp:170-173](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L170).
- Client `X9RenderMap` increments `mapping_count`; `X9RenderUnmap` removes one
  entry ([SweetX9RenderService.cpp:296-320](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp#L296)).

### 2.2 Metal/buffer-manager

- `X9BufferManager` is a metadata registry; `x9_buffer_unregister` retires an
  entry while `gpu_refs || display_refs` remain and frees it when the last ref
  drops ([X9BufferManager.c:59-66](/Users/tosasitill/ios_plj110/x9/metal/General/X9BufferManager.c#L59),
  [X9BufferManager.c:88-91](/Users/tosasitill/ios_plj110/x9/metal/General/X9BufferManager.c#L88),
  [X9BufferManager.c:115-118](/Users/tosasitill/ios_plj110/x9/metal/General/X9BufferManager.c#L115)).
- The frontend acquires one GPU ref per owned buffer/texture at commit
  ([NativeBridgeMetalFrontend.mm:1263-1268](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1263))
  and releases them exactly once in `finish()`
  ([NativeBridgeMetalFrontend.mm:1223-1231](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1223)).
  If completion is uncertain the command buffer is retained with its surfaces
  rather than pretending release is safe
  ([NativeBridgeMetalFrontend.mm:1235-1239](/Users/tosasitill/ios_plj110/x9/metal/General/NativeBridgeMetalFrontend.mm#L1235)).
- The `X9BufferKindDisplay` / `X9BufferOwnershipDisplay` enum values exist but
  have no callers; no display ref is ever acquired through this registry
  ([X9BufferManager.h:18-25](/Users/tosasitill/ios_plj110/x9/metal/General/X9BufferManager.h#L18)).

### 2.3 Display

`directPresent()` retains the last presented `SweetSurface` and releases the
previous one on the next present
([SweetFramebuffer.cpp:1534-1538](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L1534)).
Presentation itself copies into the kernel flip buffer, so the client surface
is not held for scanout
([SweetFramebuffer.cpp:1523-1531](/Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L1523)).
This is an asymmetry worth noting: display lifetime is a kernel `retain`, not a
Buffer Manager `display_refs` count.

## 3. Log evidence

pid 57 (backboardd), selected samples:

| Time | resources | MiB | wc | imported | big8M | old30s | mappings |
|---|---|---|---|---|---|---|---|
| 9.590 | 1 | 0 | 1/0 | 0/0 | 0 | 0/0 | 0 |
| 15.823 | 61 | 60 | 60/47 | 1/13 | 4 | 0/0 | 61 |
| 32.490 | 67 | 89 | 66/75 | 1/13 | 6 | 0/0 | 67 |
| 66.086 | 810 | 319 | — | — | — | 253/175 | 810 |
| 71.076 | 909 | 341 | — | — | — | 472/232 | 909 |
| 84.206 | 1,031 | 380 | 817/293 | 214/87 | 22 | 629/241 | 1,031 |
| 88.272 | 1,219 | 442 | 1,004/368 | 215/74 | 25 | 672/257 | 1,219 |
| 90.325 | 1,219 | 410 | 1,005/336 | 214/74 | 23 | 680/257 | 1,219 |

([2.txt:1205](/Users/tosasitill/2.txt), [2.txt:1664](/Users/tosasitill/2.txt),
[2.txt:2487](/Users/tosasitill/2.txt), [2.txt:6632](/Users/tosasitill/2.txt),
[2.txt:7063](/Users/tosasitill/2.txt), [2.txt:7221](/Users/tosasitill/2.txt)).

Facts:

- No `why=budget` report and no `X9 RENDER MEM:` refusal occurred.
- Resources plateau at 1,219 in the last two samples; total MiB moves
  442 → 410, so bytes are reclaimed while slots are reused.
- Release activity continues every window (82/123 MiB at 88.272 s, 26/46 MiB
  at 90.325 s), so the release path works.
- pid 125 is stable at ~112 resources/95 MiB at 90.772 s
  ([2.txt:7251](/Users/tosasitill/2.txt)), which shows the growth is not a
  global kernel accounting artefact.
- 184 `X9 PANVK IMPORT: read-only grant` lines pair with 184 first-attempt
  `prepare refused ... kr=2` lines; none is orphaned. The refusal is the
  designed read-only handshake, not failed retention
  ([X9PanVKMemory.c:384-418](/Users/tosasitill/ios_plj110/x9/metal/General/panvk/X9PanVKMemory.c#L384)).

## 4. Retention vs leak

`old30s` is a simple age count, not an unreachable-object count. A resource
older than 30 s can be a cached IOSurface, a mapped pool buffer, or a live
display surface. At 90.325 s, 680 such resources hold 257 MiB, while:

- total is 410 MiB and below the 1,024 MiB budget;
- releases are still occurring;
- only 2 command buffers are ever in flight (`Q inflight max=2`);
- no context is exposed/disabled and no submission is uncertain.

There is therefore **no proof of a leak**. The suspicious observations are:

1. `resources == mappings` in every pid 57 sample. Since release refuses while
   a mapping exists, this is either an eviction/unmap discipline that happens
   between reports or a counter that tracks a persistent kernel mapping. The
   log cannot distinguish the two.
2. `old30s` rises from 0 to 680 resources while the total plateaus. If the pool
   is not bounded by design, this would eventually approach the budget; the
   ~90 s window is too short to see it.
3. There is no release-refusal counter, so a client that repeatedly tries to
   free mapped resources is invisible.

The `MiB` field is also request bytes, not resident pages; imported surfaces
can be shared with other processes, and write-combined resources have
different cache attributes. Calling any of this a "leak" without a
reachability check would be unsupported. Confidence: **HIGH** that a leak is
unproven; **MEDIUM** that the mapping/release relationship needs design review.

## 5. Expected retention

A compositor legitimately holds a surface pool; expected retention cannot be
derived from this log. What can be stated:

- GPU in-flight retention is bounded by 2 command buffers, so `gpu_refs`
  cannot explain 680 old resources.
- Display retention is one surface (`fLastSurf`), so it cannot explain it
  either.
- The remaining 677 old resources are either client pool/cache or an
  unreleased mapping; the log has no per-resource owner, age histogram, or
  unmap count to decide.

## 6. Missing instrumentation

1. Age histogram (0–1 s, 1–5 s, 5–30 s, 30–120 s, >120 s) by resource class
   (allocated, imported, write-combined, IOSurface).
2. Release attempts and refusals split by reason: context exposed, mapping
   exists, invalid handle, other.
3. Unmap count and mapping lifetime histogram; mapping owner (which client
   call or which kernel path).
4. Per-resource display-ref and GPU-ref counts at report time.
5. Whether `old30s` resources are ever released: re-release of a sampled set,
   or a generation counter per class.
6. Imported-surface lifetime paired with the kernel `fLastSurf` retain, so
   display-held surfaces can be separated from cached ones.

No production code was modified.
