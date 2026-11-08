# Building the X9 (OPPO Find X9 / PLJ110 / MT6993) boot image

This directory holds the complete X9 work: the bare-metal shim, the kernel
overlay, the Mali/PanVK/Metal userspace and the plugin update mailbox. Logs,
device dumps, build outputs and `boot.img` artifacts are not committed.

`../README.md` and `README.md` describe the architecture and status; `../NOTES.md`
is the live handoff (current image, boot args, open problems). Patch and
diagnostic-token reference for PanVK/metal2vulkan: `PANVK-PATCHES.md`.
Inputs that live outside git: `EXTERNAL-DEPENDENCIES.md`.

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
currently plugin revision 275), checks the installed files at every boot,
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

`build/boot.json` only carries the base arguments (rootdisk, `cpus=8`, `-v`, ...).
Feature/experiment tokens (`x9_wifi_real=1`, GPU clock/park/async, `x9pl`, ...)
are passed per build as `--boot-arg`; packaging with the base alone gives a
system without Wi-Fi or GPU tuning. Recover the previous full set with
`strings x9/out/<prev>/shim/embedded-payload.bin | grep x9_metal_render`.
Output directory names must be unique: reusing one overwrites its image.

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
source preparation and staged partial builds that produced these trees were in
`x9/legacy/work/x9-metal-render/` (removed; see git history at ca5b8d5).

## Plugin mailbox

With Android running, stage a candidate's `plugin-package/`:

```bash
X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/<name>
```

Only needed when the build produced a new plugin package (new `--revision` or
changed plugin hashes). A kernel-only rebuild with the same allowlist header
needs no staging. Plugin rebuilds are not byte-reproducible, so commit the
regenerated allowlist header with the revision it names.

The root preflight is never skipped: by default it runs
`tools/check_plaintext_root.py` against `root_manifest` from `local.json`
(device serial, pin+immutable, inode, FIEMAP, userdata geometry, dm route,
live HFS UUID/geometry); `--root-manifest` or a custom `--root-check` script
override it. `mailbox/README.md` documents the mailbox contract and its safety
rules. Both backing files must stay pinned and immutable, and only the listed
slices are ever written. Then flash the candidate's boot.img by hand.
