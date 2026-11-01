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
versions; `local-source-lock.json` records retained source snapshot hashes.
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
