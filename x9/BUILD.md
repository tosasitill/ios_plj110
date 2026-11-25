# Building the X9 (OPPO Find X9 / PLJ110 / MT6993) boot image

This directory holds the complete X9 work: the bare-metal shim, the kernel
overlay, the Mali/PanVK/Metal userspace and the plugin update mailbox. Logs,
device dumps, build outputs and `boot.img` artifacts are not committed.

`../README.md` and `README.md` describe the architecture and status; `../NOTES.md`
is the live handoff (current candidates, boot args, open problems). Patch and
diagnostic-token reference for PanVK/metal2vulkan: `PANVK-PATCHES.md`.
Inputs that live outside git: `EXTERNAL-DEPENDENCIES.md`.

## What goes into one boot image

| Component | Source in this repo | External input |
| --- | --- | --- |
| XNU kernel | `kernel/` overlay (104 objects in the current full build, 3 prebuilt; see `kernel/README.md`) | the frozen SWEET XNU object tree on the build Mac |
| Shim / loader | `shim/`, `tools/` | LLVM/lld 22 |
| ADT + payload + boot.img | `tools/boot-xnu.py`, `build/boot-device.json` | `root-manifest.json` for the device's root extents |
| Mali userspace (PanVK) | `build/third_party/00-mesa-panvk-x9.patch` | Mesa `300b5fd7b44120b80607d1c90b40bb126e8d3ec8` |
| Metal runtime | `build/third_party/01-metal2vulkan-x9.patch`, `build/third_party/metal2vulkan-pan205/` | metal2vulkan |
| AIR → NIR frontend | `build/scripts/build-compiler.py`, `build/scripts/build-v14.py` | AIR frontend `c84b4634277a70beac73921029b1fb6afc2530af` |
| Plugin delivery | `mailbox/` | independent staged/root-installed package |

Plugin binaries are **not** in the boot partition. The kernel carries only an
exact `path`/`size`/`SHA-256` allowlist (`kernel/generated/x9_render_external_manifest.h`,
currently plugin revision **280**), checks the installed files at every boot,
and disables Metal discovery when a file is missing or differs. Rebuilding the
plugins means regenerating that header and staging the matching package.

## Kernel and boot.img

One full-device command, on the build Mac:

```bash
cp x9/build/config.example.json x9/build/local.json   # once; edit tool paths
python3 -I x9/build/prepare-local.py --check
PATH=/opt/homebrew/opt/llvm@22/bin:$PATH \
python3 -I x9/build/build.py all \
  --boot-config x9/build/boot-device.json \
  --name <unique-name>
```

If LLVM 22 tools are not on PATH, set `X9_OBJCOPY` and `X9_NM` explicitly to the
matching `llvm-objcopy` and `llvm-nm`. Every candidate must use a new output name.

`local.json` (gitignored) names the machine paths:

| Key | Meaning |
| --- | --- |
| `developer_dir` | Xcode 15 `Contents/Developer` (Apple clang 15.0.0, MacOSX14.0 SDK) |
| `dxb` | XNU build tree: `.local/dxb` (macOS kext ABI) or `.local/dxb-iosabi` (iOS kext ABI); built by `xnu/build-xnu.sh` |
| `llvm_clang`, `lld` | LLVM/lld 22 for the bare-metal shim |
| `root_manifest` | the device's `root-manifest.json` (holds the serial and extent map; never commit it) |
| `compiler_launcher` | optional, e.g. `ccache`: prefixes every kernel compile, so a new output directory reuses earlier compiles |

The output goes to `x9/out/<name>/`: `kernel-objects/`,
`kernel-link.filelist`, `kernel.development.sweet`, and `boot.img` with its
`boot.json`, plus `build.log`. A failed compile prints the compiler output followed
by `!! <object> failed`, the remaining objects still compile, and the build stops
with the list of failed objects. Other forms:

```bash
python3 -I x9/build/build.py kernel --name <unique-name>
python3 -I x9/build/build.py boot --kernel <kernel> \
  --boot-config x9/build/boot-device.json --name <unique-name>
python3 -I x9/build/build.py --dry-run
python3 -I x9/build/build.py all --revision <N> --name <unique-name>
```

`build/boot.json` only carries base arguments. **Use `build/boot-device.json` for
a full device candidate**; it carries the current Wi-Fi, GPU, UFS, modem, sensor,
camera and Bluetooth configuration. Add experiment tokens with `--boot-arg`.
The current default Bluetooth configuration has only `x9_bt=1`; a full-init
candidate explicitly adds `x9_bt_init=1 x9_bt_fc=1`.

Output directory names must be unique: reusing one overwrites its image.

Compiles are incremental through compiler depfiles. Boot-args and packaging
options live in the selected boot config. To change the kernel, edit `kernel/src/`;
to add an object, add its source, a template and an entry in
`kernel/recipe.json`, and its place in `kernel/link-order.txt`.

The XNU object tree is built from apple-oss `xnu-8796.141.3` plus the patches in
`xnu/` (`0001` = the complete X9 source delta, `0002` = iOS kext C++ ABI) by
`xnu/build-xnu.sh macos|ios [--setup]`; see `../NOTES.md` section 3.1.

Parallel work must not edit or rebuild another session's shared `.local/dxb*`
tree. Build a new scratch snapshot instead:

```bash
x9/xnu/build-xnu.sh macos --isolated x9/out/<unique-name>/dxb
# Or ios for the iOS kext ABI. The matching fixed .local tree must exist.
```

This APFS-clones the fixed bootstrap/object inputs (not hardlinks), restores
XNU from the pinned tag and applies this worktree's complete patch set, including
`0006-x9-bt-uart-irq.patch`. Copied text dependencies and symlinks are relocated
inside the new tree before `make install`. The original source, wrappers and
objects remain untouched. The destination must be a new directory inside this
worktree's gitignored `x9/out/`. To link the overlay against it, make a private
copy of `local.json` in the same output directory with `dxb` set to that
repo-relative scratch path, then pass `build.py --config <private-copy>`.
An isolated XNU build is still not a native-Bluetooth hardware validation.

To resume that same isolated tree after a build-tool fix, without touching the
shared inputs or copying the tree again:

```bash
x9/xnu/build-xnu.sh macos --isolated-existing x9/out/<unique-name>/dxb
```

The existing tree must remain entirely below this worktree's `x9/out/` and its
source must still equal the pinned tag plus this branch's patches. Previous
kernel link outputs are kept under `build/x9-previous-link.*`; only the normal
final link dependency chain is forced. A private `cc/python` wrapper points at
the actual `python3` compile tool for XNU's built-in dSYM macro installer. The
build requires zero exit status from both `make` and `tee`, no compile error,
and a fresh kernel link recorded in the log. It does not ignore dSYM failures.

## Native Bluetooth root payload

The H4 bridge and launch adapters are root filesystem files, **not** boot-image
contents or Metal mailbox plugins. After the full native/power bridge build
with both original 20H392 launch jobs, assemble their offline root payload:

```bash
python3 -I x9/bluetooth/package.py \
  --runtime x9/out/<complete-bt-build> \
  --out x9/out/<new-bt-root-payload>
```

It produces `root/`, separate exact `originals/` launch jobs and
`root-payload.json`. Only `Program` changes in the two derived jobs; stock
Apple executables, arguments, identity and MachServices remain unchanged.
Package metadata binds each new file and both previous job byte hashes and
requires absent new binary destinations. This is build-time packaging, not
installation or a second image inspection. `candidate_ready` is distinct from
`runtime_verified`; the latter stays false until the authoritative iOS flow
has actually been exercised. `stageable` remains false because no installer
or device-write approval is implied by packaging.

Do not pass this package to `mailbox/update.py`: its kernel consumer only
accepts the built-in Metal file manifest and cannot replace launch jobs.
Root installation requires explicit user confirmation, the live root
preflight, compare-before-write against both original jobs and stock
`bluetoothd`, preserved originals, binary publication before job publication,
fsync/readback and a private receipt. Closed-transport deepsleep/resume does
not establish active-connection suspend, wake-enhance or a hardware wake IRQ.

`build/scripts/collect-build-inputs.py` gathers a candidate directory's real
build inputs from the build Mac; it produced the current `kernel/` import.

## Original Mav17 firmware (ABM/BBUpdater)

**Do not embed the complete firmware in boot.img.** The historical 47 MB
rev252eq image left the device black-screened until a full restart. The packer
now limits embedded BBFW to 4 MiB; the existing 20 MiB boot-image guard remains.
`qdsp6sw.mbn` is required by the 20H392 `validateBootupItems()` method even
though `UpdateSource::init()` can skip a missing file. An excluded subset is
not a complete boot firmware input.

Prepare a complete **local-only** package without replacing the active build
header (no device access):

```bash
python3 -I x9/tools/baseband_firmware.py --storage external \
  --out .local/x9/baseband/external-156/Mav17.pack \
  --header .local/x9/baseband/external-156/x9_baseband_firmware.inc \
  --metadata .local/x9/baseband/external-156/source.json
```

External delivery uses a **separate** pinned+immutable Android file, never
free space in the plugin mailbox. Provisioning needs explicit user approval,
the existing root preflight, live inode/FIEMAP capture, extent-overlap checks,
compare-before-write/readback and a full raw payload hash. Do not invent a map
or normally overwrite an existing pinned file. Provisioning is not performed
by the build command.

After the user approves this exact 34 MiB file-only operation, provision it
with the dedicated stager (Android running):

```bash
python3 -I x9/tools/stage_baseband_firmware.py --create --yes \
  --root-manifest .local/x9/x9/out/pan220-batched-cache-ranges/root-manifest.json \
  --pack .local/x9/baseband/external-156/Mav17.pack \
  --metadata .local/x9/baseband/external-156/source.json \
  --state-dir .local/x9/baseband/staging-20261109-rev252hl
```

Create mode refuses existing state directories and device files. It creates
only `baseband-Mav17-20H392.pack` beside the existing root image, sets immutable,
captures the inode/map, checks root/plugin/log overlap, and rechecks protection
and mapping before each bounded raw write. The first block is invalidated and
published last. Each changed chunk is compared against its previous hash and
read back by pinfile. The final complete payload hash, unchanged existing
file identities/maps and unchanged root header are checked before publishing
the new private `root-manifest.json`. Neither local.json nor the old manifest
is changed by the stager.

If interrupted after allocation was captured, use the same arguments and
`--resume --yes` instead of `--create --yes`; it requires the exact saved
root/package/device/inode/map identity. A completed operation refuses restaging;
use `check_plaintext_root.py` with the published manifest for read-only checks.
State, commands, range digests and readback results remain private under `.local`.

An overlap with a **historical** reservation also stops staging before payload
writes. Only after tracing it to an old root manifest and proving that source
path absent may `--retired-reservation-manifest <old-manifest>` be supplied.
The source must match the active device/partition; every intersecting byte
must be attributable to its captured old map. Live root/plugin/log overlap
is never accepted. The stager keeps residual old protected ranges and moves
only the overlapping portion into the new firmware reservation. An internal
coverage check requires the complete original exclusion union to remain
protected. The old manifest stays unchanged; provenance is recorded privately
in `reservation-reclassification.json`. Resume repeats all these checks.

After provisioning, use a private copy of `root-manifest.json` with
`firmware_policy`: version `1`, kind `dedicated-readonly-bbfw`, `pinned`,
`immutable`, `initial_content_verified`, `device_file`, `inode`, `bytes`,
`payload_bytes`, `sha256`, `android_extents` and `extents`. Its extents must be
disjoint from the root, plugin reservations and disk log. The shim adds them
to root-write exclusions and emits the BBFW read-only map. The preflight
checks live protection, mapping and the entire compressed payload hash.

Set `baseband_firmware_storage` to `external` in `build/local.json`, keep the
original `baseband_firmware` input and remove `baseband_firmware_exclude`.
The builder emits `.local/x9/baseband/Mav17-external.pack` and a
`baseband-firmware.json` beside the kernel; boot packaging requires that
descriptor to match the provisioned manifest. Keep it beside an existing
kernel when using `build.py boot --kernel`.

At PID1, the kernel reads the pack into RAM, verifies its built-in hash and
every uncompressed member, then publishes only missing files. It reuses an
existing verified copy or writes one copy and hardlinks the other ABM path;
different files/symlinks remain untouched. A failed hardlink does **not** fall
back to a second large copy. No bind mount, Apple binary patch, personalization
or physical MTK modem firmware download is involved. Plugin revision is unchanged.

For historical diagnostic builds only, `--exclude` also requires
`--allow-incomplete` (`baseband_firmware_allow_incomplete: true` in local build
configuration). External mode rejects exclusions even with that flag.

## Metal/PanVK plugins

One command builds both plugins, packages them for the mailbox and points the
kernel at them; with `all` it then builds the kernel and boot.img into the
same output directory, which is then a complete candidate:

```bash
python3 -I x9/build/prepare-local.py --check
PATH=/opt/homebrew/opt/llvm@22/bin:$PATH \
python3 -I x9/build/build.py all --revision <N> \
  --boot-config x9/build/boot-device.json --name <unique-name>
PATH=/opt/homebrew/opt/llvm@22/bin:$PATH \
python3 -I x9/build/build.py plugins --revision <N> --name <unique-name>
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
the allowlist (for example, to match a previously installed package). This does not
install that package. The Xcode and Homebrew locations come from `developer_dir`
and `homebrew` in `local.json` (environment `DEVELOPER_DIR` / `X9_HOMEBREW` override).

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
slices are ever written. Staging is a device mutation and requires explicit user
authorization. The user then boots the authorized `boot_b` candidate by hand; this
device does not support temporary `fastboot boot`.
