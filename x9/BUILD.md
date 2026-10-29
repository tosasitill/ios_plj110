# Building the X9 (OPPO Find X9 / PLJ110 / MT6993) boot candidate

This directory holds the complete X9 work: the bare-metal shim, the XNU patch
series, the Mali/PanVK/Metal userspace, the kernel render engine and the
plugin update mailbox. Logs, device dumps, build objects and `boot.img`
artifacts are intentionally not committed.

`../README.md` (the inherited ios-sweet port) and `README.md` (X9 bring-up
notes) describe the architecture and the hardware evidence.

## What goes into one boot image

| Component | Source in this repo | Pinned third-party input |
| --- | --- | --- |
| XNU kernel | `xnu-patches/*.patch` applied to XNU `8796.141.3` | XNU source from Apple |
| Shim / loader | `shim/`, `tools/` | — |
| ADT + payload + boot.img | `tools/boot-xnu.py`, `tools/mkadt.py`, `tools/mkpayload.py` | `build/candidate-pan184/root-manifest.json` |
| Kernel render engine | `metal/` (`MaliMetal/X9/`) copied into the kernel build by the candidate recipe | — |
| Mali userspace (PanVK) | `build/third_party/00-mesa-panvk-x9.patch` | Mesa `300b5fd7b44120b80607d1c90b40bb126e8d3ec8` |
| Metal runtime | `build/third_party/01-metal2vulkan-x9.patch` | metal2vulkan `720ef1ce9ae2a2068999a8675717418b9416283e` |
| AIR → NIR frontend | `build/scripts/build-compiler.py`, `build/scripts/build-v14.py` | AIR frontend `c84b4634277a70beac73921029b1fb6afc2530af` |
| Plugin delivery | `mailbox/` | — |

Plugin binaries are **not** in the boot partition. The kernel carries only an
exact `path`/`size`/`SHA-256` allowlist (`build/candidate-pan184/generated/x9_render_external_manifest.h`),
checks the installed files at every boot, and disables Metal discovery when a
file is missing or differs.

## Prerequisites

- macOS with Xcode 15.x and the iPhoneOS 16 SDK (arm64 iOS16 target).
- Python 3.10+ and `lz4`.
- Rust `1.98.0` through rustup (used by the panfrost compiler and by
  metal2vulkan); the scripts call `rustup which --toolchain 1.98.0 rustc`.
- `meson` and `ninja` inside `build-env/` (created by the scripts), plus
  LLVM/clang from Homebrew for the host shader tools.
- XNU `8796.141.3` source tree, already patched with the sweet/topaz lineage
  that precedes `0186`.

## Workspace layout

The scripts resolve everything relative to their own directory, so the X9 tree
is expected at the top of a workspace:

```text
<workspace>/
  x9/                     # this directory
  xnu/                    # XNU 8796.141.3 sources (patched)
  MaliMetal/X9/           # copy of x9/metal
  work/x9-metal-render/   # build/scripts/*.py + third-party trees + build dirs
```

`build/scripts/*.py` were written for that layout; run them from
`work/x9-metal-render/` with the third-party trees checked out in the same
directory as `mesa-render-source/` and `metal2vulkan-source/`.

## 1. Kernel

```bash
# Apply the whole series in order, or only the X9 tail on top of an already
# patched 0185 tree. 0186 selects the board; later patches add UFS, decmpfs,
# keybag, userspace compatibility and the render engine.
for p in x9/xnu-patches/*.patch; do git -C xnu apply "$p"; done
```

Build the `SWEET` board configuration with `ARM64_BOARD_CONFIG_SWEET` and the
Xcode 15-compatible toolchain. The result is `kernel.development.sweet` plus
`kernel.development.sweet.unstripped`.

`build/candidate-pan184/commands.json` and `kernel-link.filelist` record the
exact per-object compile and link commands used for the latest candidate,
including every `-I`/`-D` flag and the frozen XNU build directory. They are an
audit record of the release that produced `boot.img`; the absolute paths in
them belong to the build machine and are not portable as-is.

## 2. PanVK (Mesa)

```bash
git clone https://gitlab.freedesktop.org/mesa/mesa.git mesa-render-source
git -C mesa-render-source checkout 300b5fd7b44120b80607d1c90b40bb126e8d3ec8
git -C mesa-render-source apply ../../x9/build/third_party/00-mesa-panvk-x9.patch
python3 build-panvk-native-ios.py   # meson cross build -> libvulkan_panfrost.dylib
# build-panvk-native-ios.py shells out to host shader tools built by
# build-compiler.py / build-v14.py and to the meson+ninja venv in build-env/
```

The patch adds the `X9_NATIVE_KMOD` backend (native IOKit user client instead of
DRM), the Mach-O build-id length fix, the fixed 256-byte cache atom, paired
IOKit mapping release, and disables the capabilities the backend does not
implement (`EXT_map_memory_placed`, DRM format modifiers, external fd
handles). It applies cleanly to the pinned commit.

## 3. Metal runtime (metal2vulkan)

```bash
git clone https://github.com/steelbrain/metal2vulkan.git metal2vulkan-source
git -C metal2vulkan-source checkout 720ef1ce9ae2a2068999a8675717418b9416283e
git -C metal2vulkan-source apply ../../x9/build/third_party/01-metal2vulkan-x9.patch
python3 build-general-metal.py --revision <N>   # X9GeneralMetal<N>.bundle
python3 package-panvk-native.py --revision <N>  # X9PanVK<N>.bundle
```

The patch negotiates the native submission cache ownership, keeps Vulkan's
noncoherent flags truthful, isolates host resources on 256-byte atoms, and lets
device priming use a visible noncoherent type for its never-submitted probe
command buffer.

## 4. Plugin package, mailbox and boot image

```bash
# Candidate recipe: manifest header + versioned bundles + root manifest.
python3 prepare-pan174.py        # snapshot of the lineage that built pan184

# Stage the package into the pinned mailbox while Android is running.
python3 x9/mailbox/update.py <candidate-dir>

# Boot image: ADT from the root manifest, payload, then the shim.
python3 -I x9/tools/boot-xnu.py --kernel <kernel.development.sweet>
```

`mailbox/README.md` documents the mailbox contract and its safety rules. The
mailbox is a device-specific reserved area; both backing files must stay
pinned and immutable, and only the listed slices are ever written.

## Status

Latest candidate: `pan184` (2026-10-29). Built, linked and packaged; **not yet
run on hardware**. The last hardware run (pan183) completed four GPU
submissions through the render engine but never obtained a Metal device
(`OutOfMemory(Device)` from the priming probe), so no desktop frame has been
produced by the GPU yet. See `README.md` for the full claim boundary.
