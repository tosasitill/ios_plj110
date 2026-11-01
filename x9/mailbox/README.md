# X9 Metal update mailbox

The mailbox is a device-specific reserved area composed of the safe slices in
`mailbox-manifest.json`. Both Android files remain pinned and immutable. Only
listed slices may be used; the other parts overlap historical reservations.
The manifest and both files are necessary inputs for all later updates.

A candidate includes its immutable extent map, the corresponding protected
reservations, and an exact file/length/SHA-256 allowlist. The payload is not in
the boot image. On XNU startup, before original launchd, the kernel verifies
all mailbox file contents against that allowlist, installs absent versioned
plugin files through HFS, then checks the installed files before enabling Metal
discovery. Existing conflicting files are preserved. Temporary files with the
reserved `.x9-update-tmp` suffix may be removed on a retry. A failed update
leaves discovery disabled. Booting a candidate that already has exact files
installed does not require the mailbox.

With Android running and the device connected, stage a candidate built with
this mailbox format:

```
python3 x9/mailbox/update.py <candidate-dir> --serial "$X9_SERIAL"   # root preflight: tools/check_plaintext_root.py + local.json root_manifest
```

The updater verifies root and mailbox identity, pin and immutable status and
full mappings; invalidates the commit marker; writes only mapped mailbox
slices with old-content checks and raw readback; then publishes the marker.
It does not scan the iOS root volume or write a boot partition. A candidate's
built-in allowlist must match its `plugin-package/mailbox.json`; future plugin
bytes require a new candidate with its new hashes. Manual boot flashing and
actual XNU execution remain required. Keep the current log and root backups.

The current payload is the pan184 plugin set. A hardware run of pan183 already
proved the install-and-verify path on device (marker published, files
installed, hashes re-checked, discovery armed) and completed four GPU
submissions, but device creation still failed; no desktop frame has been
produced by the GPU yet.

Root preflight: the old generated `check-root.sh` was not present in the
supplied directories. `update.py` now runs `x9/tools/check_plaintext_root.py`
against the private root manifest (`--root-manifest`, default `root_manifest`
in `x9/build/local.json`) and derives the device's `pinfile` from it;
`--root-check <script>` substitutes another verified preflight. The check is
never skipped. `x9/build/build.py all --revision N` produces a candidate
directory this updater accepts.
