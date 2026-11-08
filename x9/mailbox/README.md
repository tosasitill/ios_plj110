# X9 plugin mailbox

The Metal/PanVK plugin bundles are not in the boot image. They are delivered
through a device-specific reserved area (the safe slices listed in
`mailbox-manifest.json`) backed by two pinned, immutable Android files. Only
listed slices are ever written; the manifest and both files are inputs for every
later update.

## Contract

A candidate carries its extent map, the protected reservations and an exact
path/length/SHA-256 allowlist (`x9/kernel/generated/x9_render_external_manifest.h`,
matching `plugin-package/mailbox.json`). At XNU startup, before the original
launchd, the kernel verifies the mailbox contents against that allowlist,
installs absent versioned plugin files through HFS, then re-checks the installed
files before enabling Metal discovery. Existing conflicting files are kept;
leftover `.x9-update-tmp` files may be removed on retry. A failed update leaves
discovery disabled. A candidate whose plugin files are already installed
exactly does not need the mailbox.

## When to stage

Only when a build produced a **new plugin package** (a new `--revision` or
changed plugin hashes; plugin rebuilds are not byte-reproducible). A kernel-only
rebuild with an unchanged allowlist header needs no staging. A kernel is paired
with exactly one plugin payload: booting it against a mailbox holding another
payload disables Metal (black or stuck-at-logo screen).

## Staging (Android running, device attached)

```bash
X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/<name>
```

The root preflight is never skipped: by default it runs
`x9/tools/check_plaintext_root.py` against `root_manifest` from
`x9/build/local.json` (serial, pin+immutable, inode, FIEMAP, userdata geometry,
dm route, live HFS UUID/geometry). `--root-manifest` or `--root-check <script>`
override it. The updater verifies mailbox identity, invalidates the commit
marker, writes only mapped slices with old-content checks and raw readback, then
publishes the marker. It never scans the iOS root volume or writes a boot
partition. The owner then boots the candidate's `boot.img` by hand.

Current plugin revision: **275** (`X9GeneralMetal275`, `X9PanVK275`).
