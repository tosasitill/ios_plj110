# Local migration status (2026-11-01)

The build Mac now keeps required inputs inside this repository's gitignored
`.local/` directory. `local.json` and `config.example.json` resolve relative
`dxb` and `root_manifest` paths against the repository root.

| Local input | Purpose |
| --- | --- |
| `.local/dxb/` | Frozen XNU objects, export headers, source headers and fakeroot |
| `.local/x9/x9/out/pan220-batched-cache-ranges/` | Private root extent manifest and retained candidate inputs |
| `.local/x9/work/x9-metal-render/` | Mesa/AIR frontend, latest pan205 runtime source used by pan207, libdrm and host build tools |
| `.local/heliodarwin/work/ios16-20H392/` | Apple input for root construction (never commit) |
| `.local/x9/work/x9-metal-install-pan172/edited-root.hfs` | Single retained historical installation root; NOT a current device backup |

`python3 x9/build/prepare-local.py --check` verifies the required layout.
Without `--check` it connects the local workspace to maintained scripts and
`x9/metal/General` with relative symlinks, preserving replaced entries.
Inputs must already exist; cloning git alone does not supply private data or
third-party build trees. `requirements-metal.txt` records the migrated venv
versions; `local-source-lock.json` records the source snapshot hashes before
the maintained build-time patches described below.
The migrated Mesa configuration text and venv entrypoints were rebased to the
new workspace. For another checkout location regenerate build configurations
and the venv; copying Meson caches is not a portable installation method.

User-space plugins are built through `build.py` (see "Metal/PanVK plugins"
below); the individual entrypoints in `build/scripts/` remain callable.

Metal defaults to the retained pan205 runtime and enables the latest runtime
bridge macros. `--runtime-source` and `--source-library` options from the old
workspace are preserved. Rust's old `general-target` cache was discarded;
its next build will regenerate it. Mesa/AIR and PanVK source trees serve
different purposes and must not be collapsed into one “newest Mesa” tree.
The patch snapshots below preserve historical provenance (including original
paths); applying them to fresh dependencies still requires path substitution.

The Metal build applies `0023-quality-of-service-on-ios.patch` before compiling
the runtime, and accepts an already-patched source snapshot. It enables Apple's
QoS requests on iOS for submission, completion and compiler threads. The patch
comes from [AdrenoMetal at 831a9dc](https://github.com/UniXNU/AdrenoMetal/blob/831a9dcbd6ac7de89a11bdaf031e07b9aadfa3c8/bridge/patches/0023-quality-of-service-on-ios.patch).

The native PanVK builds also apply `02-panvk-host-linear-images.patch` to the
retained X9 integration. It enables modifier image layouts and accepts only
the linear modifier for host-pointer imports; fd sharing stays disabled.
The Metal build applies `0025-host-pointer-linear-textures.patch`, which
negotiates the modifier extension independently of dma-buf support. The X9
bridge opts into direct rendering to host textures, following AdrenoMetal's
`InPlaceWrites` policy. Format, usage, explicit pitch, memory type, alignment
and allocation bounds are checked before binding; unsupported textures keep
their shadow path. `X9 TEXTURE routes` logs direct imports, explicit-pitch
images and fallback counters at the existing sampled surface-log cadence.

`03-kraid-hsr-metadata.patch` fills Kraid's previously empty fragment HSR
metadata from its final scheduled instructions, following the retained BIR
`va_gather_hsr_info.c` contract. Tile reads, centroid/coverage reads and work
before ATEST/ZS_EMIT feed the existing DCD eligibility decisions. This neither
enables the HSR prepass nor disables HSR globally. The first four tile-reading
DCD decisions log `X9 HSR tile-read`; hardware flicker repair is not yet proven.

`04-panvk-hsr-cull-all-rts.patch` (applied after 03) closes a gap between
upstream v13+ `hsr_can_cull` and the legacy FPK rule: FPK refuses to kill
earlier fragments unless the draw writes every bound colour RT
(`!(rt_mask & ~rt_written)`), HSR did not. CoreAnimation keeps a memoryless
colour1 scratch across draws and reads it back with LD_TILE, so an RT0-only
opaque draw could cull an earlier draw's colour1 writes. With the patch a
draw may cull only if it really writes every bound RT (write-disabled or
zero-mask RTs count as unwritten). It only removes culling. Policy is chosen
once per process from an exact boot-arg token, so one plugin build supports
an A/B without a mailbox update: none = fixed rule, `x9h2` = unmodified
upstream, `x9h0` = HSR cull and be-culled both off. Logs: `X9 HSR policy`,
up to 48 distinct `X9 HSR DCD` signatures per process and `X9 HSR totals`
at power-of-two draw counts (>=256) and every 65536 draws. Variant boot
images: `build.py boot --kernel <out>/kernel.development.sweet --name <n>
--boot-arg x9h0` (the normal 602-byte args leave room for one 4-byte token).

`05-panvk-index-fetch-audit.patch` (after 04) only logs: every vkCmdDrawIndexed
whose (firstIndex+indexCount)*indexSize exceeds the bound INDEX_BUFFER_SIZE
(`X9 PANVK IDX OOB`, first 32 then powers of two), up to 16 distinct
topology/index-size/restart/instancing kinds (`X9 PANVK IDX KIND`) and totals
every 65536 indexed draws. No state change.

`x9vf` (exact boot-arg token, frontend) captures whole backboardd command
buffers: at uptime 60/80/100/120 s (or `x9_metal_geometry_start=` + k*20 s) the
next three command buffers, at most 160 draws each. Per draw: shaders, blend
and write masks, pass attachments, viewport/scissor, raster, index buffer
identity/offset/length, effective vertex ids (up to 96), decoded attribute 0/1
of every referenced vertex (up to 64) and the first 16 floats of vertex slots
2/3 (`X9 FRAME DRAW/STATE/VBUF/IDX/I/V/U`). The vertex and index byte ranges
each draw fetches are snapshotted at encode and copied again by a GPU blit
appended at commit; after completion `X9 FRAME GPU CHECK` reports differing
bytes (GPU copy vs encode snapshot) and CPU bytes changed since encode, with up
to 24 `X9 FRAME GPU DIFF` rows. The blit is extra work after the original
passes; original draws/state are unchanged. Use the normal boot.json plus
`--boot-arg x9vf` (fills the 607-byte bound, so no x9h* token together).

`06-panvk-widen-index-range.patch` (common panvk_vX_cmd_draw.c): when an index
buffer is bound with an explicit size (metal2vulkan's maintenance5 path binds
exactly count*indexSize), the bound range is widened to the end of the
VkBuffer, never past it. 219 showed the last triangle of such draws lost or
pulled to vertex 0 while GPU-visible index/vertex bytes were correct. Exact
token `x9i0` keeps the requested size for A/B. Logs `X9 PANVK IB policy` and
up to 8 `X9 PANVK IB WIDEN` lines per process.

`07-panvk-fpk-write-masks.patch` (after 04/05): upstream
`allow_forward_pixel_to_kill` requires `!(rt_mask & ~rt_written)`, but
`rt_written` is the shader's outputs and ignores colour write masks.
CoreAnimation's mask/clear draws output both RTs with colour0 write mask 0,
so they were allowed to kill earlier, still-unshaded colour0 fragments they
never overwrite. With the patch the rule uses the RTs really written
(write-disabled or zero-mask RTs excluded). Token `x9f0` keeps upstream FPK.
Logs `X9 FPK policy`, up to 32 `X9 FPK DCD` signatures and `X9 FPK totals`.

`08-panvk-serialize-passes.patch` (after 07) is a diagnostic switch, off by
default: exact token `x9s1` makes each render pass's vertex/tiler work wait
for all earlier fragment work of the queue, so render passes never overlap.
`X9 PASS SERIALIZE` logs the mode per process. Separately, the X9 queue bridge
accepts `x9tb` to link 16 tiler-heap chunks (32 MiB) instead of 5 (10 MiB);
`X9 TILER HEAP` logs requested/linked chunks.

`09-panvk-v14-cull-zero-area.patch` (after 08): upstream sets the DCD
`Cull Zero Area` bit only below v14, although the v14 DCD still has it.
CoreAnimation's Control Center platter draws are triangle lists whose first
two triangles are zero-area; on v14 their last real triangle was missing.
The patch sets the bit on v14 (default); token `x9z0` keeps upstream.
`X9 CULL ZERO AREA` logs the mode per process.

Detailed Metal blur/graph/pixel capture is now opt-in, not normal rendering
work. Default builds do not copy diagnostic setBytes data, retain diagnostic
resource tables, format per-pass graphs, add readback encoders or dump pixels.
`x9_metal_capture=1` in the boot arguments enables bounded CPU input/graph
capture; `x9_metal_pixels=1` also enables the intrusive GPU pixel readbacks.
The existing surface-route and aggregate render/display counters remain on.

Vertex/triangle-only investigation uses `x9_metal_geometry=1` (exact short
alias `x9vg` when the607-byte boot-arg payload is full), independently
of the old input/graph/pixel flags. It captures CPU inputs in three 30-second
uptime windows starting at45s (`x9_metal_geometry_start=<0..3600>` overrides
the start). Each window reserves two draws each for multi-quad blur, ordinary
blur and non-blur composition, at most18 draws/process. Up to96 effective
indices, every uniquely referenced shared-CPU vertex record (up to64 bytes),
vertex layouts/bindings, matrix/UV inputs, explicit/default viewport/scissor,
raster state and CB commit/completion identities are recorded. Counts beyond
the cap, unavailable/private data, instance stepping, OOB records and
truncated records are explicitly marked; these are not validated full draws.
Inline-byte offset records follow the active runtime's zeroing behavior.
This adds no GPU commands, barriers, readback or shader substitution, but CPU
logging/retention still perturbs timing. It is not a measured GPU-output or
FPS fix. Keep the normal `boot.json` unchanged: pass a separate packaging
JSON through `build.py --boot-config <path>` for this diagnostic candidate.

Validation: full kernel compile/link/boot packaging succeeded at
`x9/out/migration-check/boot.img` (9,416,704 bytes). PanVK rebuilt successfully
(836 Ninja steps) and a temporary revision 9999 bundle packaged/signed.
Logs are in `x9/out/migration-check/`. No device operations were run, no
allowlist changed, and the complete Metal Rust rebuild was not run after
cache deletion. These artifacts are not hardware-verified.

Historical diagnostic/recovery/install scripts are retained under
`x9/legacy/`; read its README before using them. The original generated
`check-root.sh` is absent from the supplied workspace. Mailbox updates now
require an explicit `--root-check` script; the check is never bypassed.

---

# Building the X9 (OPPO Find X9 / PLJ110 / MT6993) boot image

This directory holds the complete X9 work: the bare-metal shim, the kernel
overlay, the Mali/PanVK/Metal userspace and the plugin update mailbox. Logs,
device dumps, build outputs and `boot.img` artifacts are not committed.

`../README.md` (the inherited ios-sweet port) and `README.md` (X9 bring-up
notes) describe the architecture and the hardware evidence.

## What goes into one boot image

| Component | Source in this repo | External input |
| --- | --- | --- |
| XNU kernel | `kernel/` overlay (57 objects, see `kernel/README.md`) | the frozen SWEET XNU object tree on the build Mac |
| Shim / loader | `shim/`, `tools/` | LLVM/lld 22 |
| ADT + payload + boot.img | `tools/boot-xnu.py`, `build/boot.json` | `root-manifest.json` for the device's root extents |
| Mali userspace (PanVK) | `build/third_party/00-mesa-panvk-x9.patch` | Mesa `300b5fd7b44120b80607d1c90b40bb126e8d3ec8` |
| Metal runtime | `build/third_party/01-metal2vulkan-x9.patch`, `build/third_party/metal2vulkan-pan205/` | metal2vulkan |
| AIR → NIR frontend | `build/scripts/build-compiler.py`, `build/scripts/build-v14.py` | AIR frontend `c84b4634277a70beac73921029b1fb6afc2530af` |
| Plugin delivery | `mailbox/` | — |

Plugin binaries are **not** in the boot partition. The kernel carries only an
exact `path`/`size`/`SHA-256` allowlist (`kernel/generated/x9_render_external_manifest.h`,
currently the verified 207 bundles), checks the installed files at every boot,
and disables Metal discovery when a file is missing or differs. Rebuilding the
plugins means regenerating that header.

## Kernel and boot.img

One command, on the build Mac:

```bash
cp x9/build/config.example.json x9/build/local.json   # once; edit the paths
python3 -I x9/build/build.py                           # kernel + boot.img
```

`local.json` (gitignored) names the machine paths:

| Key | Meaning |
| --- | --- |
| `developer_dir` | Xcode 15 `Contents/Developer` (Apple clang 15.0.0, MacOSX14.0 SDK) |
| `dxb` | XNU build directory `.../xnu/build-dxb/dxb`: frozen object tree, `EXPORT_HDRS`, `fakeroot`, `dxb/xnu` |
| `llvm_clang`, `lld` | LLVM/lld 22 for the bare-metal shim |
| `root_manifest` | the device's `root-manifest.json` (holds the serial and extent map; never commit it) |
| `compiler_launcher` | optional, e.g. `ccache`: prefixes every kernel compile, so a new output directory (every commit gets one) reuses earlier compiles |

The output goes to `x9/out/<name>/` (default `x9-<git hash>[-dirty]`):
`kernel-objects/`, `kernel-link.filelist`, `kernel.development.sweet`, and
`boot.img` with its `boot.json`, plus `build.log` with every command and its
output. A failed compile prints the compiler output followed by
`!! <object> failed`, the remaining objects still compile, and the build stops
with the list of failed objects. Other forms:

```bash
python3 -I x9/build/build.py kernel                  # compile, link, strip only
python3 -I x9/build/build.py boot --kernel <kernel>  # package an existing kernel
python3 -I x9/build/build.py --dry-run               # print every command
python3 -I x9/build/build.py --name pan221-foo       # choose the output name
```

Compiles are incremental through compiler depfiles. Boot-args and packaging
options live in `build/boot.json`. To change the kernel, edit `kernel/src/`;
to add an object, add its source, a template and an entry in
`kernel/recipe.json`, and its place in `kernel/link-order.txt`.

The frozen XNU object tree is built from `xnu-8796.141.3` plus the sweet/topaz
patch lineage through 0185 and is an external input for now. `xnu-patches/`
documents how the X9 sources evolved; the build does not apply it.

`build/scripts/collect-build-inputs.py` gathers a candidate directory's real
build inputs from the build Mac; it produced the current `kernel/` import.

## Metal/PanVK plugins

One command builds both plugins, packages them for the mailbox and points the
kernel at them; with `all` it then builds the kernel and boot.img into the
same output directory, which is then a complete candidate:

```bash
python3 x9/build/prepare-local.py                     # once / after moving scripts: links .local
python3 -I x9/build/build.py all --revision 208       # plugins + kernel + boot.img
python3 -I x9/build/build.py plugins --revision 208   # plugins + package only
```

Steps (all logged to `x9/out/<name>/build.log`, long steps streamed):

1. `prepare-local.py --check`: the `.local` inputs and links exist.
2. PanVK: `build/scripts/build-panvk-incremental.py` (ninja on the configured
   `build-panvk-native-ios`, which compiles `x9/metal/General/panvk/*.c` as
   external sources) or, if not configured yet or `--panvk full`,
   `build-panvk-native-ios.py`. `--panvk skip` reuses the last library.
3. `package-panvk-native.py --revision N` -> `X9PanVK<N>.bundle`.
4. `build-general-metal.py --revision N` -> `X9GeneralMetal<N>.bundle`
   (pan205 runtime by default, `--runtime-source` to override).
5. `package-plugins.py --revision N --out x9/out/<name>`: copies both bundles
   to `plugin-package/` with `manifest.json`, `mailbox.json`, `SHA256SUMS`,
   `install-on-ios.sh`, and rewrites `kernel/generated/x9_render_external_manifest.h`.
   Commit that header with the revision it names: a kernel accepts only the
   plugin files it lists.

`package-plugins.py --from-mailbox <plugin-package>/mailbox.json` rewrites only
the allowlist (e.g. to go back to the installed 207 package). The Xcode and
Homebrew locations come from `developer_dir` and `homebrew` in `local.json`
(environment `DEVELOPER_DIR` / `X9_HOMEBREW` override).

Third-party trees (`.local/x9/work/x9-metal-render/`): Mesa
`300b5fd7b44120b80607d1c90b40bb126e8d3ec8` + `build/third_party/00-mesa-panvk-x9.patch`
(PanVK), the AIR frontend Mesa tree (`build-compiler.py`, `build-v14.py`,
`prepare-build.py` rebuild its host tools), metal2vulkan
`219f01365dc4c8eb346816c405b5b17403793764` + `build/third_party/metal2vulkan-pan205/`
in numeric order (`prepare-general-runtime.py` installs the Rust toolchain).
`build-render-cs.py` regenerates the kernel's CS packing include. The one-shot
source preparation and staged partial builds that produced these trees are in
`legacy/work/x9-metal-render/`.

## Plugin mailbox

With Android running, stage a candidate's `plugin-package/`:

```bash
python3 x9/mailbox/update.py x9/out/<name> --serial "$X9_SERIAL"
```

The root preflight is never skipped: by default it runs
`tools/check_plaintext_root.py` against `root_manifest` from `local.json`
(device serial, pin+immutable, inode, FIEMAP, userdata geometry, dm route,
live HFS UUID/geometry); `--root-manifest` or a custom `--root-check` script
override it. `mailbox/README.md` documents the mailbox contract and its safety
rules. Both backing files must stay pinned and immutable, and only the listed
slices are ever written. Then flash the candidate's boot.img by hand.
