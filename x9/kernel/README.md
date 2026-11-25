# X9 kernel overlay

The X9 kernel is the frozen SWEET XNU object tree (XNU `8796.141.3` plus the
sweet/topaz lineage through patch 0185) relinked with X9 overlay objects. The
current full builds report **104/104 overlay objects, including 3 prebuilt
objects**. This directory is the source of truth for the overlay;
`x9/build/build.py` compiles and links it. That count is a host-build result,
not a device feature claim.

| Path | Contents |
| --- | --- |
| `src/` | Overlay sources and headers at their XNU-relative paths |
| `generated/` | Generated inputs (CSF firmware, compute/desktop programs, plugin allowlist, private runtime manifests) |
| `recipe.json` | Compile templates, source/template/prebuilt selection and exact link/strip commands |
| `link-order.txt` | Exact link order: `xnu:<frozen-tree object>` or `x9:<overlay object>` |
| `prebuilt/` | Objects whose original compile command was not recorded |

Placeholders in `recipe.json`: `{DEVELOPER_DIR}` (Xcode), `{DXB}` (the frozen
XNU build tree, including `EXPORT_HDRS`, `fakeroot` and source headers), `{SRC}`
and `{GEN}` (this directory).

## Origin and prebuilt boundary

Imported on 2026-11-01 from `pan220-batched-cache-ranges` with
`x9/build/scripts/collect-build-inputs.py`. Before that, candidates copied the
previous image and recompiled only changed objects; the original 57 objects came
from 15 candidate directories (pan111 through pan220). Sources were recovered
from compiler depfiles, debug information or recorded commands. Historical
object counts are not current counts.

Three objects currently use `prebuilt/` without a reconstructable compile
command: `prng_random.o`, `sweet_early.o` and `start.o`. Editing their source has
no effect until a compile command is reconstructed and the object moves into the
normal recipe. `sleh.o` has a recipe entry and is not one of the three counted
prebuilt objects.

`x9/xnu-patches/` remains history and is not a build input. Current XNU source
changes live in `x9/xnu/`; overlay changes live in `src/` plus `recipe.json` and
`link-order.txt`.

## Current build and plugin contract

Live candidate paths, boot arguments and evidence labels are in
[`../../NOTES.md`](../../NOTES.md). Build commands are in
[`../BUILD.md`](../BUILD.md).

The generated Metal allowlist currently names plugin revision **280**. Plugin
binaries are not linked into the kernel or boot.img: they are staged through the
mailbox and installed on the iOS root. A revision/hash change requires a matching
mailbox/root package; an unchanged allowlist needs no staging.
