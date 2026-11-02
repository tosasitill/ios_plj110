# Task 2 — Submission Decoupling Design

- Date: 2026-11-02
- Based on: Task 1 audit at HEAD `5b3caad` (`architecture-inventory.md`, `call-graph.md`,
  `refactor-issues.md`, `abi-map.md`, `phase-0-report.md`).
- Status: **design only. No code was modified.** Implementation starts only after
  explicit approval.
- Tags: `SOURCE CONFIRMED` / `VERIFIED` for current-state facts; `PROPOSED` for design
  decisions; `UNKNOWN` / `NOT TESTED` where stated.

---

## 1. Design goals and non-goals

### 1.1 Goals (PROPOSED)

1. Remove GPU completion from the Metal API call path: `vkQueueSubmit` enqueues and
   returns; completion is observed through a fence object.
2. Remove `metal_service_lock` from cache maintenance and from completion waiting;
   keep it only for CSF register/AS/doorbell/halt programming.
3. Scope cache maintenance to the submission's resource set, with a conservative
   reachability fallback.
4. Give buffers and VMs explicit in-flight references so async completion cannot race
   release.
5. Keep the display path independent: display observes a fence read-only and never
   takes a GPU submit lock.
6. Preserve Metal/Vulkan observable ordering and the existing synchronous behavior as a
   selectable path.

### 1.2 Non-goals (must not change in Task 2/implementation phase 1)

- No change to CSF firmware, register programming, doorbell addresses, AS layout, fence
  page offset (`+0x8000`), guard bytes, or group start/halt sequencing.
- No change to cache instructions or to the `X9_RENDER_CACHE_ATOM` contract.
- No change to Metal `commit` semantics, command-buffer completion handlers, or
  `vkQueueSubmit` ordering rules.
- No direct scanout, no display hardware change, no IOV A/IOVA unification.
- No new GPU feature exposed through `pan_kmod_ops` beyond submission/fence plumbing.
- No re-port of PanVK or metal2vulkan.

### 1.3 Hard constraints discovered in Task 1

| # | Constraint | Evidence | Tag |
|---|---|---|---|
| C1 | `externalMethod` rejects descriptors and caps structure input at `sizeof(X9RenderSubmission)` (96 B) | `SweetX9RenderService.cpp:438-440` | `SOURCE CONFIRMED` |
| C2 | `externalMethod` holds the per-context lock for the whole selector | `:441-443` | `SOURCE CONFIRMED` |
| C3 | `render_submit` holds `metal_service_lock` across maintenance + submit + poll | `SweetX9RenderEngine.inc:269`; `SweetX9CSF.cpp:657` | `SOURCE CONFIRMED` |
| C4 | Completion is polled; `render_wait` is a 64-entry history lookup | `SweetX9RenderEngine.inc:400-455,593-597` | `SOURCE CONFIRMED` |
| C5 | PanVK has no submit-time user-resource list; only internal `panvk_priv_bo` refs exist | `mesa-render-source/src/panfrost/vulkan`: `panvk_priv_bo_ref` used in mempool/device/descriptor internals only | `SOURCE CONFIRMED` |
| C6 | metal2vulkan does track per-command-buffer resources (`used_resources()`) | `NativeMetalObjects.rs:1672-1684` | `SOURCE CONFIRMED` |
| C7 | Two identical copies of `X9RenderABI.h` exist (kernel and userspace), kept in sync manually | `diff` returns identical; `x9/metal/General/X9RenderABI.h` | `SOURCE CONFIRMED` |
| C8 | Plugin allowlist pins exact bundle SHA/bytes/path; any plugin change needs regeneration | `x9/kernel/generated/x9_render_external_manifest.h` | `SOURCE CONFIRMED` |
| C9 | One connection, one VM, one group in practice | `X9PanVKMemory.c:187-203,261` | `SOURCE CONFIRMED` |
| C10 | Display uses TryLock and defers; lock order `fLock → GPU owner` | `SweetFramebuffer.cpp:1041-1056`; `SweetX9CSF.cpp:664` | `SOURCE CONFIRMED` |

C1 and C2 directly shape the ABI: a resource-set payload cannot be passed as an
`IOExternalMethod` descriptor under the current method, and a blocking fence wait cannot
run under the current per-context lock.

---

## 2. New module boundaries

### 2.1 Target decomposition (PROPOSED)

```
Metal.framework shim                     M1  unchanged
metal2vulkan                             M2  unchanged (optional resource hint later)
─────────────────────────────────────────────────────────────
PanVK + X9 Integration                   M3  Submission Front-End
  builds X9SubmissionDesc, owns CALL buffers, maps Vulkan fences
        │  enqueue (ABI v2)
        ▼
Kernel Submission Queue                  M4  FIFO, state machine, backpressure
        │  dequeue
        ▼
XNU Mali Backend                         M5  Execution Backend
  ├── Resource registry (BO refs)        M5a
  ├── GPU VM                             M5b
  ├── CSF engine                         M5c
  └── Cache governor                     M5d
        │  completion
        ▼
Fence Service                            M6  fence objects, waiters, deadlines
        │  read-only query
        ▼
Display boundary                         M7  unchanged behavior; fence-aware later
```

This is a logical split inside the existing binaries; it does not require new KEXTs or
processes (spec §1 note).

### 2.2 Responsibility table

| Module | Owns | Must not do |
|---|---|---|
| M3 Front-End | Submission descriptor, CALL/stream buffers, resource refs, Vulkan→X9 fence mapping, queue order | program CSF, build page tables, dereference `IOMemoryDescriptor`, touch display |
| M4 Queue | FIFO of submissions, submission state, backpressure, id allocation | interpret resources, do cache maintenance, wait for completion |
| M5 Backend | BO/VM lifetime, AS install/remove, doorbell, halt, cache maintenance, health | decide Metal/Vulkan semantics, own display |
| M6 Fence | fence identity, signal state, waiter list, deadlines, completion record | hold GPU locks while waking waiters |
| M7 Display | present buffer selection, panel/TE, legacy copy | take submit lock, program CSF |

### 2.3 What moves where (current → target)

| Current location | Current responsibility | Target |
|---|---|---|
| `render_submit` (`SweetX9RenderEngine.inc:269`) | plan + maintenance + AS + doorbell + poll + halt + post-invalidate | split: M4 dequeue → M5 execute → M6 signal |
| `render_wait` (`:593`) | history lookup | M6 fence query (v1 Wait maps to it) |
| `SweetX9RenderService::externalMethod` context lock | serializes all selectors | M4/M6 use their own locks; context lock only for registry ops |
| `metal_service_lock` | CSF + desktop + submit | M5 `hw_lock` only around hardware programming; desktop wrapper stops holding it |
| `RenderCachePlan` over all mappings | maintenance scope | M5d takes M3 resource set; fallback = current plan |
| 64-entry completion history | completion records | M6 ring + fence objects (v1 Wait compatibility view) |

---

## 3. New interfaces (PROPOSED)

### 3.1 Userspace ↔ kernel ABI v2 (additive; v1 untouched)

Keep `X9_RENDER_ABI 1` for every v1 struct and selector. Add a capability bit and four
selectors after `X9RenderTimestamp = 11`:

```
X9RenderSubmitAsync = 12
X9RenderFenceWait   = 13
X9RenderFenceQuery  = 14
X9RenderSubmissionRelease = 15   (optional; only if refs are not dropped at signal)
```

Capability bit (next free after `X9RenderSubmissionCacheOwnership = 32`):

```
X9RenderAsyncSubmit = 64
```

Structures (new, with explicit `size` so future growth is detectable):

```c
#define X9_RENDER_MAX_RESOURCES 64u

struct X9RenderResourceRange {
    uint32_t abi;          /* X9_RENDER_ABI */
    uint32_t handle;       /* X9RenderBuffer.handle */
    uint32_t flags;        /* X9RenderGPURead|Write|Execute + role bits below */
    uint32_t reserved;
    uint64_t offset;       /* 256 B aligned */
    uint64_t bytes;        /* 256 B aligned, > 0 */
};                          /* 32 B */

enum X9RenderResourceRole {
    X9RenderResourceRenderTarget = 0x100u,
    X9RenderResourceSampled    = 0x200u,
    X9RenderResourceVertex     = 0x400u,
    X9RenderResourceIndex      = 0x800u,
    X9RenderResourceIndirect   = 0x1000u,
};

struct X9RenderSubmissionV2 {
    uint32_t abi, size, group, stream_count;
    uint32_t flags, resource_count, reserved0, reserved1;
    uint64_t submission_id;
    struct X9RenderStream streams[X9_RENDER_MAX_STREAMS];
    struct X9RenderResourceRange resources[X9_RENDER_MAX_RESOURCES];
};                          /* 16 + 8 + 3*24 + 64*32 = 2144 B */

struct X9RenderFenceWait {
    uint32_t abi, size, flags, group;
    uint64_t submission_id;
    uint64_t timeout_ns;    /* 0 = poll; -1 = 10 s clamp as today */
};

struct X9RenderFenceState {
    uint32_t abi, size, status, group;
    uint64_t submission_id, completed_id;
    uint32_t fault_status, exception_type;
    uint64_t fault_address;
};                          /* same semantic fields as X9RenderCompletion */
```

Flag bits for `X9RenderSubmissionV2.flags`:

```
X9RenderSubmissionAsync      = 1   /* return after enqueue, not after completion */
X9RenderSubmissionExactSet   = 2   /* resource list is complete; no fallback needed */
```

Rules:
- `size` must equal the compiled size; unknown size → `kIOReturnBadArgument`.
- `abi` must be `X9_RENDER_ABI`; the v2 capability bit gates all four selectors.
- `resource_count == 0` or `ExactSet` absent → kernel uses the Task 1 reachability plan.
- `resource_count > 64` → `kIOReturnBadArgument` (userspace falls back to reachability).
- Offsets/bytes must be 256 B aligned and inside the resource (same checks as
  `render_cache_plan_build`).
- Handles are resolved under the connection registry; unknown/foreign handle → error.

**C1 workaround:** raise the input ceiling from `sizeof(X9RenderSubmission)` to
`sizeof(X9RenderSubmissionV2)` in `externalMethod`, but only for selectors 12–15; v1
selectors keep the 96 B ceiling. No descriptors are used, so the existing rejection of
`structureInputDescriptor` stays.

### 3.2 PanVK ↔ X9 backend (C)

Add to `X9PanVKMemory.h` / backend, without changing `pan_kmod_ops` semantics for old
callers:

```c
struct x9_submit_resource {
    uint32_t handle;
    uint32_t flags;
    uint64_t offset, bytes;
};

struct x9_submit_desc {
    uint32_t group;
    uint32_t stream_count;
    uint32_t resource_count;
    uint32_t flags;                 /* X9_SUBMIT_ASYNC | X9_SUBMIT_EXACT_SET */
    uint64_t submission_id;
    struct X9RenderStream streams[3];
    struct x9_submit_resource resources[X9_RENDER_MAX_RESOURCES];
};

int x9_pan_kmod_submit_async(struct pan_kmod_dev *dev,
                             const struct x9_submit_desc *desc,
                             uint64_t *submission_id);
int x9_pan_kmod_fence_wait(struct pan_kmod_dev *dev, uint32_t group,
                           uint64_t submission_id, uint64_t timeout_ns,
                           struct X9RenderCompletion *out);
int x9_pan_kmod_fence_query(struct pan_kmod_dev *dev, uint32_t group,
                            uint64_t submission_id,
                            struct X9RenderCompletion *out);
```

`x9_pan_kmod_submit()` keeps its present signature and becomes:

```
submit_async(desc{flags=0}) + fence_wait(submission_id, 10 s)
```

so the existing PanVK call sites and the Mesa code paths do not change until M3 opts in.

The `pan_kmod_ops` vtable gains **no** new pointer: the added functions are X9-specific
symbols, not generic kmod ops, so upstream-shaped Mesa code stays intact.

### 3.3 Kernel-internal interfaces

```c
struct x9_submission {
    struct x9_conn *conn;
    struct x9_group *group;          /* ref */
    struct x9_vm *vm;                /* ref */
    struct x9_bo *bos[64];           /* refs, resource_count */
    uint32_t resource_count;
    struct x9_packet packet;         /* 3 stream copies, as render_submit builds today */
    uint64_t fence_value;
    struct x9_fence *fence;          /* M6 */
    uint32_t state;
    struct x9_submission *next;
};

int  x9_queue_enqueue(struct x9_submit_queue *, struct x9_submission *);
struct x9_submission *x9_queue_dequeue(struct x9_submit_queue *);
int  x9_backend_execute(struct x9_submission *);      /* maintenance + AS + doorbell + halt */
int  x9_backend_complete(struct x9_submission *, struct X9RenderCompletion *);
struct x9_fence *x9_fence_create(struct x9_conn *, uint32_t group, uint64_t value);
int  x9_fence_wait(struct x9_fence *, uint64_t timeout_ns, struct X9RenderCompletion *);
int  x9_fence_query(struct x9_fence *, struct X9RenderCompletion *);
```

M5 internal interfaces (unchanged behavior, extracted from current code):
- `x9_backend_prepare(submission)` — resolve BOs, build `RenderCachePlan` from the
  resource set; fall back to reachability.
- `x9_backend_clean_before(submission)` — current pre-CIVAC logic, no `hw_lock`.
- `x9_backend_program(submission)` — `flush_gpu`, `install_as`, doorbell; **holds
  `hw_lock`**; same instructions as `SweetX9RenderEngine.inc:396-455`.
- `x9_backend_quiesce(submission)` — halt(SUSPEND), `flush_gpu`, `remove_as`, detached;
  holds `hw_lock`.
- `x9_backend_invalidate_after(submission)` — current post-CIVAC, no `hw_lock`.

### 3.4 Display interface (defined now, used later)

```c
struct x9_present_fence { uint32_t group; uint64_t submission_id; };

/* Lock-free, non-blocking; never takes hw_lock or submit locks. */
bool x9_present_fence_signaled(const struct x9_present_fence *f);
```

M7 behavior in phase 1 is unchanged: it keeps using `sweet_x9_desktop_frame_poll`'s
generation match and CPU verification. The fence query is the seam for Task 7 and is not
wired to panel behavior by this design.

### 3.5 Deliberately not interfaces

- No direct `metal_service_lock` export.
- No BO/VM pointers crossing to userspace.
- No display call into CSF or into the submission queue.
- No new Vulkan extension or `VkSubmitInfo` side channel: the resource set is a PanVK
  internal or a declared hint, not a Vulkan contract.

---

## 4. Resource ownership (PROPOSED)

| Object | Owner | Lifetime rule |
|---|---|---|
| `x9_submission` | M4 queue | Allocated at enqueue; freed after completion record is observed or after `SubmissionRelease`; refcount starts at 1 for the queue + 1 per waiter |
| CALL/stream buffer | M3 PanVK | Ref held by submission; released to PanVK quarantine only after fence signal |
| BO | M5a registry | `x9_bo_ref` at enqueue for each resource; `x9_bo_unref` at completion; free deferred until refs drop |
| VM | M5b | Ref held by every submission; `vm_destroy` refuses/defer until refs drop (fixes LIFE-1) |
| Group | M5c | Ref held by queue while a submission is queued/running; group destroy waits for drain (no new `d->failed` on late destroy) |
| Fence | M6 | Created before enqueue; signaled exactly once; freed when submission and waiters release; value is `(group, submission_id)` |
| Completion record | M6 | One per submission, kept until all observers retire; v1 `Wait` reads the same record |
| Present buffer | M7 | Display holds its own buffer/surface refs; never a submission ref |
| Cache ranges | M5d | Derived from the resource set; no persistent ownership |

Rules:
1. A submission never holds a raw pointer without a ref. This is the main new invariant.
2. Userspace may free a BO after its fence signal; the queue must not touch it afterwards.
3. On fault, refs are dropped only after the queue drains or the connection is closed
   (current code retains them forever; the design keeps retention but makes it explicit
   and bounded to the connection lifetime).
4. VM/group teardown becomes drain-then-destroy, replacing the current
   "destroy after groups, else poison" rule (`X9PanVKMemory.c:196-203`).

---

## 5. Synchronization model (PROPOSED)

### 5.1 Lock inventory and total order

| Lock | Protects | Held across |
|---|---|---|
| `display.fLock` | framebuffer state | composition only |
| `conn.state_lock` | enabled/failed, resource registry, id allocation | registry lookup only |
| `queue.lock` | FIFO, counts, backpressure | enqueue/dequeue only |
| `bo.lock` | BO refcount + mapping state | maintenance on that BO |
| `vm.lock` | page table + mapping list | map/unmap/publish |
| `hw_lock` | CSF registers, AS install, doorbell, halt, `flush_gpu` | one programming step only |
| `fence.lock` | signal state + waiter list | signal/broadcast only |

Total order (outer → inner), enforced by construction:

```
display.fLock → conn.state_lock → queue.lock → bo.lock → vm.lock → hw_lock → fence.lock
```

- Display takes only `display.fLock` plus lock-free fence reads (no edge into GPU locks).
- Fence wait takes only `fence.lock` and sleeps on a wait channel; it must **not** hold
  `conn.state_lock`, `queue.lock` or `hw_lock`.
- The worker takes `queue.lock` to dequeue, releases it, then takes `bo.lock`/`vm.lock`,
  then `hw_lock` for programming. `hw_lock` is never held while waiting for a BO or for
  completion.
- Wakeups happen after releasing `fence.lock` where possible.

### 5.2 State machines

Submission:
```
Prepared → Queued → Running → Completed
                    │
                    └→ Faulted        (health check or GPU fault)
```
A wait timeout does not change the state; it returns `kIOReturnTimeout` and leaves the
submission Running (health permitting).

Fence:
```
Unsignaled → Signaled
           └→ Faulted  (carries X9RenderCompletion fault fields)
```

Group:
```
Idle → Running → Idle
              └→ Faulted (terminal until close)
```

Connection:
```
Ready → Faulted (terminal)
```

### 5.3 Execution sequence in the worker (one submission)

1. Dequeue (no lock held afterwards).
2. `x9_backend_prepare`: resolve resource set, build plan. Fallback: reachability.
3. `x9_backend_clean_before`: pre-CIVAC; no `hw_lock`; per-BO serialization.
4. `hw_lock`: `flush_gpu` → `install_as` → doorbell → unlock.
5. Observe completion: poll fence page + health (current cadence) or an IRQ if one is
   proven available. `UNKNOWN` whether an IRQ path exists for these submissions.
6. `hw_lock`: `halt(SUSPEND)` → `flush_gpu` → `remove_as` → unlock.
7. `x9_render_memory_detached`.
8. `x9_backend_invalidate_after`: post-CIVAC.
9. Build completion record; `x9_fence_signal`; release BO/VM/group refs; notify waiters.

Step 8 precedes step 9 so that a signaled fence still means "CPU-visible data is
coherent", preserving today's contract.

### 5.4 Wait semantics

- `X9RenderFenceWait`: deadline-based. `timeout_ns == 0` polls and returns immediately;
  `-1` clamps to 10 s (current clamp in `wait_bo`). Timeout → `kIOReturnTimeout`, no
  poisoning by itself.
- `X9RenderFenceQuery`: non-blocking; returns the same record fields as
  `X9RenderCompletion`.
- v1 `X9RenderWait` maps to query + historical record, preserving its current
  non-blocking contract.
- Completion handlers in Metal stay on the runtime's thread; the runtime already polls
  fences, so no change above PanVK is required.

### 5.5 Fault and error propagation

- Health check failure or GPU fault fields → fence Faulted, group Faulted, connection
  Faulted; all queued submissions complete with `kIOReturnIOError`.
- No reset, no retry, no automatic recovery (unchanged).
- Waiters wake with the fault status; display keeps the previous frame.
- Retained refs are released at connection close only.

### 5.6 Cache governance

| Transition | Action | Placement |
|---|---|---|
| CPU → GPU (read) | CIVAC on exact spans | worker step 3, no `hw_lock` |
| GPU → CPU (read) | CIVAC after quiescence | worker step 8, no `hw_lock` |
| GPU → GPU | no action (same AS, ordered streams) | n/a |
| GPU → Display | display only reads completed buffer | after fence signal |
| Display → CPU | unchanged legacy copy path | M7 |

Resource-set inputs to the governor:
- `flags & GPUWrite` → include in phase 1;
- `flags & GPURead` without write → phase 0;
- roles `RenderTarget`/`Sampled`/`Vertex`/`Index`/`Indirect` are forwarded for logging
  and future precision, not for different instructions in phase 1.
- 256 B atom, WC skip, coherent skip, never-exposed whole-BO clean: unchanged.

---

## 6. Build dependencies

### 6.1 Component matrix (PROPOSED)

| Change | Kernel | PanVK | metal2vulkan | Metal shim | Allowlist revision |
|---|---|---|---|---|---|
| ABI v2 structs + selectors | yes | yes (header copy) | no | no | yes |
| Queue + fence service | yes | no | no | no | no |
| `submit_async` in X9 backend | no | yes | no | no | yes |
| PanVK resource tracking | no | yes | no | no | yes |
| Resource-set cache plan | yes | yes (sends set) | no | no | yes |
| Narrow `hw_lock` | yes | no | no | no | no |
| Display fence query | yes | no | no | no | no |

Consequence: stage 1 (queue/fence/lock split) can be validated with the existing
synchronous userspace, because v1 behavior is preserved. Opting PanVK into async requires
a plugin rebuild and allowlist regeneration (revision bump), exactly like commits
`f03f53d`/`5b3caad`.

### 6.2 Build-time correctness checks (PROPOSED)

1. `diff` the two `X9RenderABI.h` copies as a build step; fail on mismatch (C7).
2. `static_assert` sizes for every new struct in both C and C++ builds.
3. Generate the allowlist in the same build invocation that builds the plugin; fail if
   the bundle hash is stale.
4. Add the new kernel source files to the XNU build list explicitly; no wildcard.

### 6.3 What must not be rebuilt

- metal2vulkan and the Metal shim are unchanged by all stages of this design.
- Mesa changes are limited to PanVK's X9 integration; no upstream-shaped code outside
  `#ifdef X9_NATIVE_KMOD` blocks.

---

## 7. Compatibility plan

### 7.1 ABI compatibility

- v1 structs and selectors are frozen; no field is added, removed or reinterpreted.
- v2 is capability-gated: old userspace never sees the new selectors; new userspace
  checks `X9RenderAsyncSubmit` before using them.
- Kernel accepts both; the synchronous path is `submit_async` + `fence_wait` internally,
  so there is one execution engine, not two.

### 7.2 Behavior compatibility

| Setting | Behavior |
|---|---|
| `x9_submit_async=0` (default for first boot) | PanVK uses v1 sync submit; kernel may still queue internally but the caller waits |
| `x9_submit_async=1` | PanVK uses SubmitAsync; `vkQueueSubmit` returns after enqueue |
| `X9RenderSubmissionExactSet` off | reachability cache plan (current behavior) |
| `ExactSet` on | resource-set plan; used only after device validation |

### 7.3 Staged rollout (each stage independently revertible)

| Stage | Content | Userspace change | Verification gate |
|---|---|---|---|
| D1 | Fence service + query, no async | none | v1 `Wait` reproduces current results; no behavior change |
| D2 | Kernel queue + worker, sync submit goes through it | none | boot/GUI unchanged; `render_submit` timing logs comparable |
| D3 | SubmitAsync selector + PanVK opt-in | plugin | boot-arg A/B; submit latency drops, rendering identical |
| D4 | Resource set from PanVK tracking | plugin | cache byte/hole logs match or shrink; pixels identical |
| D5 | Narrow `hw_lock`, move maintenance out | none | display tick counters improve; no lock-order warning |
| D6 | Display fence query (no behavior change) | none | display unchanged; query counts only |

### 7.4 Rollback

- Runtime: set `x9_submit_async=0`; v1 path is unchanged.
- Source: revert the stage's patch; D1/D2 are kernel-only, D3/D4 need a matching plugin
  revision. Kernel and plugin must be reflashed together because the allowlist pins the
  bundle hash.
- No on-disk or partition state is involved.

---

## 8. Risk list

| # | Risk | Likelihood | Impact | Mitigation | Verification |
|---|---|---|---|---|---|
| R1 | Resource set incomplete → stale/garbled pixels | Medium | High | `ExactSet` opt-in; default reachability; per-resource validation; log set size vs plan | A/B pixels; cache byte logs; replay tests already used for 226–229 |
| R2 | Async changes observable ordering | Low | High | Single FIFO per group; signal only after all streams + halt; keep group order | Compare fence completion order to submission order over N frames |
| R3 | `fence_wait` under the context lock deadlocks | Medium | High | Do not use `externalMethod`'s context lock for wait; dedicated wait path with `fence.lock` only | Static lock-order check; stress two threads (submit + wait) |
| R4 | Lock inversion with display `fLock` | Low | High | Display never takes GPU locks; fence query is lock-free | Lock-order annotations; boot with heavy GUI |
| R5 | BO freed while in flight | Medium | High | BO/VM refs at enqueue, drop at completion | Alloc/free stress; leak counters after close |
| R6 | Worker starves or adds latency | Medium | Medium | Poll cadence same as today; health checks; no sleep in submit caller | Submit-to-fence latency histogram (kernel log) |
| R7 | Timeout semantics mask a real fault | Medium | Medium | On timeout run `x9_csf_group_health`; only poison on failed health/fault fields | Fault-injection build (no hardware changes) |
| R8 | ABI header drift between two copies | Medium | High | Build-step diff + static asserts | CI build failure |
| R9 | Allowlist mismatch after partial rebuild | High | High | Generate allowlist in the same invocation; revision bump | Boot log plugin hash check |
| R10 | Async gives no GPU speedup on one group | High | Low (expectation) | Document that stage 1 removes CPU blocking, not GPU serialization | Benchmarks before/after, measured only |
| R11 | PanVK resource tracking misses descriptor-set BOs | Medium | High | Collect from bound descriptors, render targets, index/vertex/indirect; fallback reachability | Validation build logs requested vs covered handles |
| R12 | Fault handling with queued work | Medium | High | Drain queue to Faulted; no reset; display keeps last frame | Fault injection; compare to current uncertain path |

---

## 9. Acceptance criteria for implementation (Task 6)

1. v1 path produces identical completion semantics and records.
2. Fence signal order equals submission order per group; no signal before post-invalidate.
3. Submit caller does not block on GPU completion when async is enabled.
4. Cache maintenance volume does not exceed the Task 1 baseline when `ExactSet` is off,
   and is ≤ baseline when on.
5. Display tick counters (`fCadenceGpuBusy`) do not regress; ideally improve after D5.
6. No BO/VM leak after N frames; refcounts reach zero at close.
7. Fault injection yields Faulted fences and preserved display output.
8. No change to CSF register values, fence page layout, or cache instructions.

Performance numbers are `NOT TESTED` until measured; no claim of speedup is made here.

---

## 10. Decisions requested before implementation

1. **Queue placement**: kernel worker thread (proposed) vs. `IOCommandGate` action vs. a
   userspace pipelined model that keeps the kernel synchronous. The proposed kernel queue
   is the only option that removes GPU completion from the Metal call path for a single
   hardware group.
2. **Fence identification**: reuse `(group, submission_id)` (proposed) vs. a separate
   64-bit fence id namespace. Reuse avoids a second identifier and maps directly to the
   existing hardware fence value.
3. **Resource-set source**: extend PanVK tracking (proposed, keeps layering) vs. a
   metal2vulkan-declared hint passed through a private channel. The second is faster to
   prototype but leaks Metal knowledge below Vulkan.
4. **Resource-set bound**: 64 ranges per submission (proposed). Larger sets fall back to
   reachability.
5. **Default for first candidate**: `x9_submit_async=0` and `ExactSet` off (proposed), so
   the first build is behavior-neutral.

No implementation begins until these are confirmed.
