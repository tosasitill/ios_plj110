# X9 native GPU backend

The MT6993 backend uses real CSF shader execution, separately from the older
G57 job-slot and CPU rasterizer code in the parent directory. The current
hardware baseline for system Metal plus the private tile is pan166.
Pan167 verifies full-frame copies but publishes none; pan168 is the next build
candidate for workgroup batching. Pan164 hung without a logo in the owner attempt.
Pan165 restores the validated display path: the owner confirms normal desktop,
and the capture shows continuing frames. Its plugin rejected boot mode2, so
no user compute or offscreen copy ran. Pan166 fixes the plugin mode parser.
Its owner capture confirms normal
desktop, eight root/UID501 Metal submissions and a successful private desktop
tile copy, including all2048 pixels, all guards and complete GPU power cleanup.

## Verified system compute

With `x9_metal_system=1`, GPU-permitted processes discover `X9Device` through
the original iOS `MTLCreateSystemDefaultDevice()`. The IOKit service checks GPU
permission at open and on every method. Each request copies bounded inputs,
runs the shader in private GPU memory, validates actual arithmetic, finishes
AS/MCU/cache/power cleanup, and returns its own GPU output. Clients share a
submission lock.

Pan163's owner capture proves four root submissions and four submissions from
a separately executed UID/GID501 process: eight unique kernel API sequences,
802 correct results, completion callbacks, and complete cleanup. The root
launchd job forks; its child clears supplementary groups, changes GID/UID to
501 and execs a fresh image before using Objective-C, Metal or IOKit. The
capture also has GUI swaps and no crashes from the two clients, backboardd,
SpringBoard or chronod. Other daemon crashes remain in that capture.

Evidence is in the original workspace:
`logs/x9-pan163-system-metal-20261028-194203-562688.{raw,json}`.

The Metal subset remains shared staging buffers up to4096 bytes, built-in
`add_u32`, three buffer bindings, 1D dispatches of up to256 elements with local
size up to128, synchronous command buffers and completion callbacks.
Arbitrary MSL/AIR/metallib compilation, Metal render pipelines, textures,
samplers and Metal IOSurface rendering remain unsupported. Unsupported
library/pipeline operations return nil/NSError, without successful placeholders.

Known render owners `backboardd`, `SpringBoard`, `chronod`, `mediaserverd`,
`renderserverd` and `RenderServer` retain passive Metal discovery. Chronod's
RenderBox abort loop in pan162 was absent from the pan163 capture.

## Pan166 mode parser fix and private desktop copy

Candidate: `x9/out/pan166-metal-system-mode/boot.img`.

The pan164 capture reaches the75-dispatch GPU baseline and complete power
release, but deployment returns EEXIST17. It ends mid-line at9.499s, before
any GUI takeover or `X9 DESKTOP GPU` completion. The owner reports a hang and
no logo. These observations do not establish the final hang site or prove
that the new copy shader ran.

The pan165 owner capture confirms deployment errno0, GUI takeover12.193s,
84 recorded LK frame submissions and a heartbeat swap ID179. The original
Metal factory returns nil at14.942s and no user compute reply or offscreen
worker is observed. The plugin initializer still accepted only mode1 while
the probe and kernel accepted mode2. Pan166 shares an exact-token mode parser
between the plugin and probe and logs accepted mode and connection failures.
Evidence: `logs/x9-pan165-desktop-20261028-204602-033564.{raw,json}` in the
original workspace. This capture proves display recovery, not Metal recovery.

`x9_metal_system=2` enables system compute plus a one-shot offscreen copy.
The framebuffer uses pan163's original CPU pixel-copy and LK-submit path.
It never waits for a GPU request or its provider lock. After four successful
root submissions and four successful UID501 submissions, it may capture an
actual64×32 desktop tile into a private16KiB buffer and enqueue a detached
worker. The worker serializes with compute clients outside the display
workloop and runs the new2D copy shader into a second private16KiB buffer.

The private copy uses source stride272 and destination stride288, checks all
2048 output pixels plus every row/tail guard after MCU stop/full GPU flush,
and requires at least one output pixel to differ from the original sentinel.
Input GPU pages are read-only. Neither mapping names a live IOSurface or the
panel. Unknown DMA/power state retains the private buffers and disables GPU
requests while ordinary CPU display continues. There is no retry or reset.

Deployment uses unique pan166 bundle/program paths, accepts only the exact
original, pan164, pan165 or pan166 launchd cache, preserves the predecessor
via hard link, and atomically installs an original-derived cache containing
one root validation job. That job execs the separate UID501 child. Discovery
also requires successful deployment; a conflict does not advertise a missing
or mismatched plugin. Conflict logs identify the actual path. Apple executable
bytes are untouched; older files remain.

- `/System/Library/Extensions/X9MetalCompute166.bundle`
- `/private/var/x9-metal-pan166/x9-metal-probe`
- `/private/var/x9-metal-pan166/x9-metal-mobile`
- `/System/Library/xpc/launchd.x9-metal-pan166-before.plist`

Value1 enables system compute without the offscreen copy. Value0 or an absent
system flag disables discovery, the service and deployment. The startup kernel
compute campaign retains its separate boot-argument gates.

Pan166 hardware evidence is saved in the original workspace as
`logs/x9-pan166-metal-desktop-20261028-205333-118029.{raw,json}`. Deployment
returns errno0 with exact pan165 predecessor. Both clients use the original
Metal factory and actual pan166 programs; root PID87 and independently execed
UID/GID501 PID238 complete four submissions each, sequences1–8,802 correct
arithmetic results and eight completion callbacks.

At23.757s the private worker captures an actual desktop tile. Its shader
completes with fence44000001, EXTRACT24,64×32 geometry and localX64. After
MCU stop/full GPU cache flush, all2048 pixels match, all2048 differ from the
initial sentinel, and every row/tail guard survives (digest2369b5c5). AS1/AS0
detach, L2 release and GPUEB active/power0 precede successful worker completion
at23.784s. GUI takeover is9.757s, later frames continue and heartbeat swap ID
reaches105. The owner reports normal operation.

The roughly60.5s capture has no kernel panic or CRASH from backboardd,
SpringBoard, chronod or either compute client;66 other daemon crash events
remain. FitnessWidget is denied GPU access by the existing permission check.
This is a sequential compute pair and one private copy, not a long-term or
concurrent performance result. Metal rasterization and GPU output publication
to the panel were not implemented in that verified version.

Normal compilation/linking/packaging and their integrated checks are allowed.
Builders do not execute probes, run test suites, flash, reboot or write the
attached device.

## Pan167 staged full-frame presentation candidate

Candidate: `x9/out/pan167-desktop-staged-present/boot.img`.
Owner result: still laggy. Captured pan167 runtime completes eight full-frame
copies with every pixel/guard correct and complete cleanup, but discards all
eight as stale. There are zero GPU-result publications and zero panel readback
passes. Worker duration is713–726ms (mean718.875ms); the experiment ends and
releases its private pool at32.210s. Later frames continue on CPU. The capture
ends at90.529s, so later persistent sluggishness is not explained by ongoing
GPU experiments. Pan166 remains the successful Metal/private-tile baseline.
Evidence in the original workspace:
`logs/x9-pan167-staged-present-20261028-212213-283139.{raw,json}`.

In mode2, after four root and four UID501 compute replies, a detached worker
allocates two reusable private full-frame buffers. Eligible later swaps
compose into the input buffer, preserving untouched pixels from the current
panel. The GPU copies all1256×2760 pixels into the output buffer, with input
stride5120 and output stride5184. Pixel data starts at byte64, with poisoned
prefix, row padding and tail guards. GPU input mappings are read-only and
neither GPU mapping includes a live IOSurface or panel page.

The worker checks all3466560 pixels and every guard after MCU stop/full GPU
cache flush. It publishes completion only after AS detach, descriptor
completion and GPUEB power release. The framebuffer polls without waiting on
the GPU provider lock. It accepts output only for the same frame generation,
with no pending newer swap, display power on and the LK engine idle. It copies
the validated GPU result into the known panel memory, reads every pixel back
from that CPU mapping, and invokes the existing LK submission. This has CPU
composition, preservation and publication copies: it is a staged integration
of GPU output into displayed frames, with no zero-copy or speedup claim.

First-frame display keeps the working CPU/LK path. GPU-busy swaps use the CPU
path; a later generation discards the older GPU result. A swap naming the
in-place default panel surface stays on CPU because its userspace writes
cannot be protected by the driver's frame counter. Existing clipping, layer
order, display offsets and native1256×2760 geometry remain unchanged.

The run is bounded to three successful GPU-result publications or eight
submitted frame attempts. Every attempt has its own snapshot and fresh fence;
a stale result is discarded rather than retried. Clean failures disable
further GPU frames and recover the current input through the CPU if its
generation is still current. Uncertain DMA/power state retains private memory
and disables GPU requests, while the existing CPU display can continue.
After known-clean completion the private pool is released.

The owner capture should establish:

- Deployment errno0, actual pan167 root/mobile programs and system pair8/8.
- Normal first-frame GUI and continuing swaps.
- `X9 DESKTOP FRAME` GPU fence/extract, full3466560-pixel and guard pass,
  full GPU cleanup and worker completion.
- Matching-generation `GPU-result publication`, panel CPU readback pass and
  subsequent `LK submit result=1` (or a documented busy retry that completes).
- Up to three such publications using the same private buffers, followed by
  clean pool release and continued CPU display. New swaps/interaction may be
  needed to offer enough eligible frames.

Pan167 uses unique bundle/program/backup paths and accepts only the exact
original, pan165, pan166 or pan167 launchd cache. It preserves its predecessor
and atomically replaces the cache. Mode1 remains system compute only; mode0
or an absent flag disables deployment/discovery. No rendering owner is exposed
to the incomplete Metal rendering API. Arbitrary Metal shaders, textures and
rasterization remain separate unimplemented work.

## Pan168 8x8 workgroups and latency backoff

Candidate: `x9/out/pan168-desktop-workgroup-xy/boot.img`.
Status: normal compile/link/package completed; runtime NOT RUN.

Pan167's width1256 selected localX8. With localY1 it submitted157×2760 =
433320 workgroups, eight threads each. Pan168 changes only the dispatch
batching to local8×8,157×345 =54165 groups and64 threads/group. The same
3466560 pixel invocations run the unchanged shader. Both dimensions divide
exactly, so there are no extra out-of-bounds invocations or new shader bounds
branches. The workgroup-size word is generated from frozen Mesa v14 XML.
Shader-core mask1, clocks, firmware/PM lifecycle, buffer mapping, full pixel
and guard checks, and stale-generation rejection are retained.

New timing logs distinguish doorbell-to-fence wall time from end-to-end
worker time, including setup, dispatch, full validation and cleanup. A stale
result whose worker took more than100ms disables further staged desktop
attempts for this boot and releases the known-clean private pool. It does not
disable the separately verified Metal compute service. Matching-generation
results still require panel readback and successful LK submission. No speedup
or actual GPU desktop presentation is established by this host build.

Pan168 deploys unique program/bundle/backup paths and accepts only exact
original, pan166, pan167 or pan168 launchd caches. Original Apple executable
bytes are unchanged. The display remains CPU-composited; arbitrary Metal
rendering/texture pipelines remain unsupported. Even a staged publication
pass will not by itself constitute GPU-accelerated desktop composition.

## General shader compiler preparation

`work/x9-metal-render/` freezes Mesa
`300b5fd7b44120b80607d1c90b40bb126e8d3ec8` and the AIR frontend from hackios
`c84b4634277a70beac73921029b1fb6afc2530af`. The AIR→NIR frontend and Mesa's
Kraid/v14 compiler libraries build on the macOS host. Build recipes, the Mesa
API port, dependency/build logs and exact input manifest are retained there.
These host libraries are not yet an integrated AIR→GPU path, an iOS plugin
compiler, a Metal texture backend or a render pipeline.

## General native renderer under development

`General/` contains the iOS16 Metal frontend/factory, IOKit render ABI,
userspace PanVK adapter and kernel render primitives. The factory opens the
real `SweetX9Render` user client and validates caps before creating a device.
The native kernel now links the resident multi-stream engine and opt-in
bootstrap. Pan169 hardware logs show resident startup and caps0x3f; no
native render submission ran, and SpringBoard repeatedly crashed in the
original CoreImage GL context. Pan170 adds real desktop plugin deployment
and discovery. It has not booted yet. Host builds are not hardware validation.

The full native Mesa v14 library builds in
`work/x9-metal-render/build-panvk-native-ios/`. Its guarded X9 branch uses
IOKit physical-device enumeration, per-client512MiB quotas, a1–16GiB GPU VA
window, native group lifecycle and submission, fixed preallocated128MiB tiler
chunks plus a separate16KiB context, and no Linux DRM/WSI backend. It exports
vkGetInstanceProcAddr for direct loading. The16KiB-signed local package is
`work/x9-metal-render/X9PanVK170.bundle`; it is embedded in pan170 for
guarded deployment at owner boot. It has not been loaded on X9 yet.

### Resource and synchronization contracts

`X9RenderTransport.c` validates native reply sizes and caps. Native BO/VM ops
are in `panvk/X9PanVKMemory.c`; GPU executable buffers are read/execute and
data buffers read/write. Host CPU mappings are non-coherent and need explicit
cache maintenance. An uncertain map, teardown or completion faults the
connection and prevents resource/VA reuse. Imported host memory stays owned
by its caller: unmapping its Vulkan view never munmaps the original pointer.
The kernel import pins pages and rejects the physical panel range.

The native Vulkan host-import path supports HOST_ALLOCATION_EXT at16KiB
alignment, for buffers and linear2D images; fd, dma-buf and external sync-file
sharing remain unsupported. The Metal bridge now explicitly negotiates the native service's
X9RenderSubmissionCacheOwnership capability before enabling noncoherent
Shared/Managed/host-import memory. It refuses a missing capability or an
unexpected cache atom. The runtime defaults to coherent-only allocation for
other consumers; Vulkan HOST_COHERENT remains unset on X9. This code compiles
and links, but the handoff has not executed on X9. AMBA capability0 observations
are not proof of coherent operation.

`panvk/X9PanVKSync.c` implements process-local binary/timeline objects using
independent reference-counted fence dependency nodes. Pending payload transfer
captures a fence, not a mutable handle. Reset/destruction cannot change a
captured wait; timeline chains retain prior dependencies, and completed nodes
are reclaimed iteratively. Limits are4096 handles,16384 fence/point nodes and
256 waits/signals per batch. Clones share locked state. Waits use monotonic
absolute deadlines and Darwin condition variables. Output signals advance
only after native submission completion is validated; failure poisons the
provider and wakes waiters. External fd capabilities are cleared from Vulkan
sync types. No concurrency/runtime validation has been run for this code.

`panvk/X9PanVKSubmitPlan.c` preserves per-subqueue command order using generated
Mesa v14 CALL packing and the four reserved registers. It accepts only equal
initial waits across active queues and terminal signals. Internal dependencies,
commands after signals, waits after commands and binary wait/resignal reuse
remain refused. Timeline waits for0 are skipped; a wait for an older value
followed by a higher signal on the same timeline is accepted. Sync-only and
empty batches validate prior synchronous group completion and resolve waits
before publishing host signals, without manufacturing a GPU completion. `X9PanVKQueue.c` owns CALL allocation/clean/release/quarantine callbacks;
the actual PanVK init/main submit paths invoke it under X9_NATIVE_KMOD.
General submission semantics still need those restrictions removed.

### Kernel lifecycle and display primitives

`kernel/SweetX9RenderService.cpp` enforces task GPU permission, caller address
ownership, quotas, mapping geometry and submission sequencing. It publishes
only with a complete engine callback table and full native caps. Map/unmap
software records change only after engine success. Context teardown must
remove every hardware mapping/group before completing descriptors; uncertainty
retains prepared memory. The opt-in bootstrap supplies these callbacks through SweetX9RenderEngine.inc;
this installation path has been compiled/linked, not executed on X9.

`X9RenderPageTable.cpp` implements bounded4KiB GPU tables backed by prepared
16KiB host allocations. It reclaims empty tables while detached, cleans before
publication, and refuses mutation/destruction while attached. The1152-table
bound uses at most18MiB. `X9RenderMemory.cpp` adds explicit CPU cache operations
on descriptor-owned mappings, avoiding ARM_COHERENT_IO no-op helpers.

`X9CSFInterfaces.cpp` parses the live cache-invalidated64KiB firmware interface.
`X9CSFRenderQueue.cpp` programs three stream bindings and endpoint masks/counts,
checks fresh global/group doorbell ACKs, implements START/SUSPEND/RESUME/STOP,
and reports faults/OOM without fake ACKs or reset. It does not prove rendering
completion. `X9CSFQueueAccess.cpp` supplies real descriptor/cache/MMIO access,
write access restricted to host input blocks, and pan168 fault checks. The
hardware owner must retain all descriptors and serialize these accesses.

`SweetX9CSFResident.inc` derives powered resident firmware startup from the
pan168 parser, AS0, delegation and PING path. It preallocates eight three-stream
slot I/O/suspend ranges and retains MCU/AS0/GPUEB refs between requests. Trace
storage is coalesced at its original GPU VAs to preserve the original32-entry
Plan and legacy kernel stack size. Final teardown requires all slots returned,
all groups terminated and AS1 detached. Slot claims use monotonic session
epochs; release requires termination/AS1 detach and full GPU flush before its
user I/O is reset. Resident-only capability bits remain0. The engine callback publishes its
implemented capabilities only after resident startup and callback installation
under the same owner lock used by the legacy compute service. Actual GPU
execution and platform ABI compatibility remain unverified.

The opt-in `x9_metal_render=1` display source uses an ordinary RAM IOSurface
as the default target, while fPixels remains the inherited panel mapping.
Swap handles that RAM surface as a normal source and copies through the
existing LK path. It preserves native geometry and keeps panel PA unmapped
by the GPU. This is a compiled display delivery path, not GPU composition,
zero-copy scanout or a hardware display result.

### Builds and retained inputs

Run normal builds only; these do not access the device or execute tests:

```
python3 work/x9-metal-render/build-panvk-native-ios.py
python3 work/x9-metal-render/package-panvk-native.py
python3 work/x9-metal-render/build-render-kernel.py
python3 work/x9-metal-render/build-resident-kernel.py
python3 work/x9-metal-render/build-render-display.py
```

The normal build/link results and compiler inputs are under
`work/x9-metal-render/`: `panvk-native-full-build.log`,
`resident-kernel-build.log`, `render-kernel-build.log`,
`render-display-build.log`, `panvk-native-package.log`, and per-directory
commands.json. Port diffs are retained as `panvk-native-*.patch` plus
`resident-lifecycle-integration.patch` and `native-default-surface-display.patch`.
Kernel patches0243–0251 preserve the service/VM, budgets, interface, resident,
RAM display, execution engine, cache ownership and standalone render bootstrap.
The pan169 boot image packages the latest kernel; runtime remains NOT RUN.
The separate development kernel is
`work/x9-metal-render/native-kernel/kernel.development.sweet`, built by
`build-native-kernel-link.py`. Its exact object filelist and compile/link
commands are retained in that directory. This normal link completed.
Outstanding work includes hardware validation of cache ownership, unrestricted
queue synchronization, loader/iOS16 ABI validation, and actual GPU rendering
through desktop display.

### Resident execution engine

Patch0249 adds the engine implementation and bootstrap call from the existing
GPU startup worker. `SweetX9RenderEngine.inc` creates per-client contexts and
groups, allocates three kernel-owned ring/body/fence resources, and maps them
through separate kernel-only page-table helpers below the user VA window.
The user-client Map operation cannot access those helpers or handle0.

Each ring prepublishes immutable CALL packets; only completed body slots are
reused. The CS packing is generated from pinned Mesa v14 XML by
`build-render-cs.py`. Submission publishes the AS1 root, confirms PA-range
cache synchronization, starts/resumes the three-stream group, publishes the
producers, rings the user doorbell, and waits for independent GPU64-bit fences,
exact EXTRACT positions and fence guards. Before returning success it suspends
the group, confirms full GPU flush, detaches AS1 and returns CPU cache
ownership. Completed receipts are retained in a64-entry per-context history.
Failure with uncertain GPU ownership retains MCU/AS/DMA/power references and
disables further rendering. These are implemented checks, not evidence of
shader execution or desktop output on this kernel.

The bootstrap is gated by x9_metal_render=1 and requires the existing startup
campaign/GPUEB handoff first. It does not alter Metal discovery/deployment for
render owners yet. The Metal bridge negotiates software cache ownership from
the native service; enabling the kernel flag alone still does not deploy the
plugins or deliver a Metal desktop. The engine performs conservative
whole-resource CPU cache maintenance; no frame-rate improvement is established.
`native-kernel-link.log` records the successful full compile/link. No adb device
was attached during the read-only availability query in this session, and no
new code has been flashed, loaded or run on hardware.

### Shared memory cache ownership

Patch0250 supplies X9RenderSubmissionCacheOwnership (bit32), a synchronous
software ownership contract, not a hardware coherency claim. On entry to GPU
work the engine cleans and invalidates every client resource through a real
descriptor-owned cacheable mapping. After independent fences, exact EXTRACT,
group suspension, full GPU cache flush and AS1 detach, it cleans/invalidates
GPU-writable resources before publishing completion. Dirty CPU cache lines of
other unused resources in a pooled BO are preserved. The caller must not
access a GPU-owned cache atom until synchronized completion; simultaneous
CPU/GPU writes in one atom remain unsupported. Failure never signals success.

The negotiated atom is256 bytes; the engine refuses the capability when the
CPU's live cache line is not covered by it. Runtime pools, automatic heap
allocation, image heap plans and upload regions isolate separate placements
to this atom. Buffer heaps already use256-byte placement. Imported no-copy
ranges and rebinds must start and end on atoms; partial-atom imports refuse.
The current IOSurface frontend imports page-aligned, page-rounded backing.
Shared imported surfaces retain their existing owner/lifetime contract.

The native Vulkan driver continues to advertise noncoherent memory, with
nonCoherentAtomSize256. Explicit vkFlushMappedMemoryRanges and
vkInvalidateMappedMemoryRanges perform immediate bounded kernel maintenance
and propagate failure as device loss. CPU host-image-copy entrypoints are
not advertised, because they bypass submission ownership; runtime image
copy fallback submits through the same native driver. Internal and staging
GPU submissions therefore use the same kernel ownership boundary too.

Fresh read-only references are in work/references/UniXNU-gpu/: Adreno740,
AdrenoMetal, GopFramebuffer, UniXNU-sm8550-common and UniXNU-sheng. Their exact
commits are pinned in reference-inputs.json. The Adreno740 memory/VM code
separates explicit publication/acquisition from a board-proven IO-coherent
fast path. X9 reuses that ownership principle with MT6993 operations; it does
not import Qualcomm registers, SMMU setup or their4KiB host-page assumptions.

Source replay inputs are native-shared-cache-ownership.patch (workspace
delta), runtime-submission-cache-ownership.patch and
panvk-native-cache-ownership.patch (relative to their existing port inputs),
plus shared-cache-reference-inputs.json. Apply those deltas after the earlier
runtime/native Mesa port preparation when reconstructing an export. The
normal builds build-general-metal.py, build-panvk-native-ios.py,
build-native-kernel-link.py and package-panvk-native.py all completed with
this code. Both user bundles are linked/signed and the independent kernel is
linked. No test suite, device execution, candidate image packaging or hardware
coherency/performance validation was performed.

### Pan169 owner-run kernel candidate

`x9/out/pan169-native-render-cache/boot.img` packages the current renderer
kernel with x9_metal_render=1 replacing x9_metal_system=2. The600-byte
cmdline retains the startup compute campaign,8 CPUs, native geometry and
USB/pstore logging. Console base0xaec0f000, size0x200000 and pstore mirroring
are explicit. Normal kernel link and shim/image packaging completed; no
additional tests or device writes were run. The packaged boot is11149312
bytes and the decompressed Image22753280 bytes, as reported by the builder.

The standalone render bootstrap allocates the shared GPU lock itself. It
skips legacy compute discovery, cache deployment and the mode2 desktop-copy
worker. Its new logs identify resident startup failure stages and report
X9 RENDER CAPS on successful capability acquisition. RAM default surface
creation emits X9 RENDER DISPLAY. These logs distinguish service startup and
display compatibility from any submitted GPU drawing.

This candidate does not install or publish the general Metal/PanVK plugins to
desktop owners, nor automatically issue an arbitrary rendering submission.
It therefore tests resident startup/service publication and RAM surface/GUI
compatibility; a visible desktop alone is not GPU rendering or shared-cache
roundtrip proof. Its source/object snapshots, exact commands, filelist, root
manifest, packaging tools and patches are frozen alongside the image.
Unchanged kernel objects retain references to the earlier frozen builds.
`build-driver.log` retains initial toolchain failures followed by successful
packaging; the shim uses LLVM22/lld22 and the built-in host validator uses
the Xcode macOS SDK through build-tools/x9-clang.

### Native desktop Metal candidate pan170

`x9/out/pan170-native-metal-desktop/boot.img` contains the full native Metal
frontend and PanVK170 payload as normal linked assembler data. The builder
reports30785536 bytes for boot.img and69939200 bytes for the decompressed
Image. The600-byte render cmdline, native geometry, UFS root/sidecar and
USB/pstore configuration are inherited from pan169. Kernel deploy-hook
compilation, full link and shim/boot packaging completed. Hardware status
is NOT RUN.

Before original PID1 exec, x9_render_deploy.inc accepts only the exact
original/pan166/pan167/pan168/pan170 launchd cache, preserves a hardlink backup
at launchd.x9-render-pan170-before.plist, installs exclusive custom files with
fsync, and atomically installs the cache containing the one-shot root render
probe. Conflicting existing files or caches are preserved and deployment
refuses. Apple executable bytes are unchanged. The installed bundles are
X9GeneralMetal170.bundle and X9PanVK170.bundle; the probe lives at
/private/var/x9-render-pan170/x9-render-probe. The older compute deploy
remains disabled by absence of x9_metal_system.

The existing IOAcceleratorES now supplies X9GeneralMetal170 and
X9GeneralMetalFactory to native render clients, including SpringBoard and
backboardd, only after this boot's payload installation and real native
service registration. Lookup waits at most10s for the asynchronous GPU worker;
it never exposes compute-only metadata or pretends a missing service is ready.
The factory accepts x9_metal_render=1 and opens the native versioned PanVK
loader. X9 RENDER DISCOVERY/CLIENT/GROUP/SUBMIT logs distinguish each boundary.

Native physical-device enumeration exposes one real Vulkan GPU queue per
device, shared by Metal logical queues; its three CS subqueues still execute
together. The kernel serializes physical ownership. Extra Vulkan GPU queues
would reserve another128MiB tiler heap and another scarce resident slot without
adding native execution parallelism, so are not advertised. The fixed8 CSG
slots,512MiB per-client backing quota, fixed tiler arena, missing external fd
synchronization and untested private iOS16 ABI remain constraints. No claim
of unrestricted application/queue support or desktop success follows from
this candidate build.

The owner-run probe obtains MTLCreateSystemDefaultDevice, loads an authored
metallib compiled normally by Apple's metal/metallib tools, and creates real
Metal compute/render pipelines. It changes CPU input for3 GPU arithmetic/cache
roundtrips and compares256 results each. It then imports a64x64 IOSurface,
draws a full-screen triangle through vertex/fragment shaders and compares all
4096 pixels to magenta. CPU code writes only inputs and initial sentinels;
results must come from GPU commands. A failure is logged with the phase. The
probe is compiled/signed during the host build and runs only when the owner
boots the image. Its output does not by itself prove the original desktop
processes render; factory/submission/GUI evidence must identify those too.
Display delivery remains the existing CPU/LK copy of GPU-written RAM surfaces.

Build recipes: build-general-metal.py --revision170,
package-panvk-native.py --revision170, build-render-deploy.py,
build-native-kernel-link.py and prepare-pan170.py. The candidate freezes
its changed sources/objects, payload input files, link filelist, commands and
packaging tools; unchanged kernel objects retain the earlier frozen inputs.
Patch0252 preserves discovery/service/deploy source changes;
native-desktop-metal-source.patch preserves the userspace/build delta.
No extra tests or device writes were performed by these builders.

### Pan171: external plugin storage

`x9/out/pan171-external-metal/boot.img` contains the native render kernel and
small exact SHA256/length manifest, but no plugin binary or probe program.
The inherited compute-probe deployment and its embedded bytes are removed.
Normal packaging reported an18673664-byte decompressed Image and9383936-byte
boot image; this is a host build, not a hardware boot. Console and USB/pstore
settings match pan169. The independent archive `x9-metal-plugins-ios.tar.gz`
contains versioned X9GeneralMetal170 and X9PanVK170 bundles, a file manifest
and an installer; it contains no test program.

Install the extracted archive on the writable iOS root filesystem with
`install-on-ios.sh` as root. It verifies package hashes and refuses existing
versioned destination or staging names. It creates only its own bundle paths
under `/System/Library/Extensions`, then syncs; it never replaces launchd or
Apple executables. This is an iOS filesystem operation: Android's view of the
pinned root file is encrypted and is not a writable HFS mount. The setup
requires a way to transfer the package to and run a root shell in the iOS
environment. ADB is currently disconnected and no installation was performed.

At boot the kernel reads each exact plugin file and validates length/SHA256
before arming Metal discovery. A missing/mismatched file logs X9 RENDER
EXTERNAL and leaves discovery disabled. The resident engine may still start;
that does not mean GPU desktop rendering succeeded. Desktop rendering,
cache ownership and iOS16 Metal ABI still require hardware execution.
