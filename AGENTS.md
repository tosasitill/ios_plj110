# Working on ios-sweet / X9

## Start and handoff

- Read `NOTES.md` before starting. It is the current source for goals, hardware,
  fixed inputs, build commands, candidate paths, verification labels and the next
  concrete action. Subsystem READMEs explain contracts; `docs/` holds durable
  research and historical reports.
- The user switches agents frequently. Update `NOTES.md` after each milestone and
  before stopping. Record source HEAD, commit/CI IDs, exact commands and exit
  status, artifact sizes, device operations and unfinished work.
- Separate **observations**, **inferences**, **host verification**, **candidate
  readiness**, **device boot** and **feature acceptance**. Compilation never proves
  hardware behavior. Do not fabricate samples, ACKs, frame times, installation
  success or performance gains.
- Keep handoffs concise. Put durable subsystem contracts in its README and research
  in `docs/`; do not copy historical build-by-build logs into `NOTES.md`. Recover
  older detail with `git log -S'<keyword>' -- NOTES.md` and `git show`.

## Repository and build rules

- Builds depend only on this repository plus Xcode 15, KDK_13.5_22G74, Homebrew
  LLVM/lld 22, ccache, Python and lz4. Private inputs stay in the fixed `.local/`
  layout checked by `x9/build/prepare-local.py --check`. Never point builds at
  `~/HelioDarwin` or other external source trees.
- Use `x9/build/boot-device.json` for full device candidates; `boot.json` is base
  arguments only. Always choose a new `--name`; do not overwrite an old candidate.
- XNU source changes live in `x9/xnu/` patches and require
  `x9/xnu/build-xnu.sh`. Overlay changes live in `x9/kernel/src/` plus
  `recipe.json` and `link-order.txt`.
- Plugin binaries are mailbox/root payloads, not boot.img contents. A revision or
  hash change rewrites the kernel allowlist and requires matching staging and
  installation. A kernel-only build with an unchanged allowlist needs no mailbox.
- After a successful build, do not add unpacking, hashing, disassembly, rebuilding
  comparisons or test suites unless requested. Built-in checks remain valid.
- Use `gh` for GitHub Actions kernel builds and logs. Local single-file
  cross-compiles are useful before spending a CI run.

## Branches and worktrees

- One feature or experiment gets `feat/<topic>` or `fix/<topic>`. Parallel work uses
  `git worktree add .worktrees/<topic> -b <branch>` and a
  `../../.local` symlink to the main checkout. Copy private `local.json` only
  when a build needs it.
- Scratch output belongs in that worktree's gitignored `x9/out/<name>/`. Do not
  create experiment directories under `.local/`.
- Do not overwrite, commit or delete `issues.jsonl`, another window's uncommitted
  work, old images, private manifests or backups.
- Commit completed work. Merge finished work into `main`, then remove only the
  worktree/branch created for that task. Push to `origin` only when the user has
  requested it for that work.

## Device safety

- Do not flash, reboot, reset, write root/mailbox, mutate partitions or probe live
  sysfs without explicit authorization for that exact action. Device writes are
  never inferred from a build, an earlier authorization or a general goal.
- This device does not support temporary `fastboot boot`. Authorized bring-up writes
  only `boot_b`. Do not change GPT, bootloaders, recovery, modem, persist,
  userdata outside the pinned iOS extents, or backups. Confirm every destructive
  step separately.
- The 32 GiB HFSX iOS root occupies pinned/immutable extents inside existing
  `userdata`. Root/mailbox operations require preflight and the private extent
  manifest; never treat an Android path as a writable HFS mount.
- Preserve Android installation and partition backups. Keep dumps, serial numbers,
  downloaded Apple artifacts and raw device logs out of git.
- Read the exact new log before drawing conclusions. “No log” does not mean “did
  not boot”; a visible desktop does not prove GPU rendering; a process staying
  alive does not prove Bluetooth discovery or audio.
- On an authorized X9 iOS → fastboot reboot, use
  `python3 -u x9/tools/ios-reboot-bootloader-usb` if not already in fastboot.
  A USB request ACK is not the endpoint; require the helper's fastboot detection
  and independently confirm exactly one device, `is-userspace: no`, and slot
  `b`. Do not auto-retry a failed reboot or run `set_active`.
- To return to Android on this phone, use the user-designated host restore image
  `/Users/tosasitill/boot.img` (64 MiB), not an iOS candidate. This file is only
  a device restore input, not a build dependency. After explicit authorization
  to flash that image and reboot, verify the device and partition size, run
  `fastboot -s "$id" flash boot_b /Users/tosasitill/boot.img`, and only after
  success automatically run `fastboot -s "$id" reboot`. Check Android boot
  with the unique adb device, PLJ110, `ro.boot.slot_suffix=_b`, and
  `sys.boot_completed=1`. If any check fails, stop; do not switch slots,
  reflash, or reset automatically.
- For an authorized iOS boot, start one USB logger before reboot with a fresh
  output path, preserve previous captures, and confirm the exact new log. See
  `NOTES.md` §3.5 for the device workflows.

## Research and documentation

- Use existing Android/OPPO/MediaTek, UniXNU and signed Apple static evidence before
  designing a new layer. Do not repeat closed investigations or migrate another
  SoC's registers, page sizes, ABI or runtime claims.
- Offline research uses existing source, captures and logs only. No device/sysfs
  access and no synthetic runtime evidence. Avoid blind disk-wide or repository-wide
  searches; follow `NOTES.md` and subsystem docs to known paths.
- Repository-owned READMEs must match current configuration and label historical
  evidence. Do not rewrite private/generated/upstream READMEs under `.local/` or
  `x9/out/`; keep archived `docs/sweet/` historical.
