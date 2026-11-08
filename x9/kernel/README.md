# X9 kernel overlay

The X9 kernel is the frozen SWEET XNU object tree (XNU `8796.141.3` plus the
sweet/topaz lineage through patch 0185, built once on the build Mac) relinked
with 57 X9 overlay objects. This directory is the single source of truth for
those overlay objects; `x9/build/build.py` compiles and links them.

| Path | Contents |
| --- | --- |
| `src/` | Overlay sources and headers at their XNU-relative paths |
| `generated/` | Generated inputs (CSF firmware, compute/desktop programs, the plugin allowlist header) |
| `recipe.json` | Compile templates (exact argument lists, include order kept), each object's source and template, link and strip commands |
| `link-order.txt` | The exact link order: `xnu:<object in the frozen tree>` or `x9:<overlay object>` |
| `prebuilt/` | Objects whose source is known but whose compile command was never recorded |

Placeholders in `recipe.json`: `{DEVELOPER_DIR}` (Xcode), `{DXB}` (the XNU
build directory `.../xnu/build-dxb/dxb`, which holds the frozen object tree,
`EXPORT_HDRS`, `fakeroot` and the `dxb/xnu` source tree the headers come
from), `{SRC}` and `{GEN}` (this directory).

## Origin

Imported 2026-11-01 from pan220-batched-cache-ranges with
`x9/build/scripts/collect-build-inputs.py`. Before that, every candidate was a
copy of the previous one with a few files string-patched and only those
objects recompiled; the 57 objects came from 15 different candidate
directories (pan111 through pan220). For each object the source was taken from
the compiler depfile, else from the object's debug info, else from the recorded
command. All 93 files had a single content across the candidates that used
them. The `ccache-bin/clang` wrapper used for some objects was replaced by the
Xcode clang it caches.

`prebuilt/` holds six objects from pan117: `start.o`, `SweetCPU.cpo`,
`sweet_early.o`, `sleh.o`, `prng_random.o`, `IOKitBSDInit.o`. Their sources are
in `src/` (from pan117, pan116 `reconstructed-inputs` and
`work/x9-usb-live/recover-build`), but no compile command exists for them, so
editing those sources has no effect until a command is reconstructed and the
object moves into `recipe.json` with a template.

`x9/xnu-patches/` remains as history of how these sources evolved; it is not
an input to the build.

## Where things are now

Live status, the current image and its boot arguments are in `../../NOTES.md`.
Plugin allowlist header (`generated/x9_render_external_manifest.h`) is rewritten
by `build.py plugins|all`; see `../BUILD.md` and `../mailbox/README.md`.
