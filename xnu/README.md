# XNU sources for ios-sweet / X9

This directory's `patches/` tree is the historical **sweet / SM7150** patch
series. It is not the current X9 kernel build input. Current X9 work uses XNU
`xnu-8796.141.3` plus the patches in [`x9/xnu/`](../x9/xnu/) and the overlay in
[`x9/kernel/`](../x9/kernel/README.md).

## Current X9 build

From the repository root, with the fixed private inputs prepared:

```bash
python3 -I x9/build/prepare-local.py --check
x9/xnu/build-xnu.sh macos
# Or the iOS kext ABI tree needed by the original 20H392 kexts:
x9/xnu/build-xnu.sh ios
```

`macos` maintains `.local/dxb`; `ios` maintains `.local/dxb-iosabi`. Both apply
this worktree's complete `x9/xnu/` patch set, including the Bluetooth UART IRQ
patch, verify that the source equals the pinned tag plus those patches, and run
`make install` for `DEVELOPMENT_ARM64_SWEET`. See
[`x9/xnu/build-xnu.sh`](../x9/xnu/build-xnu.sh) and
[`x9/BUILD.md`](../x9/BUILD.md) for the exact contract.

Parallel work must not rebuild another session's shared tree. Create an isolated
copy below the current worktree's gitignored output directory:

```bash
x9/xnu/build-xnu.sh macos --isolated x9/out/<unique-name>/dxb
# Resume the same isolated tree after a tool fix:
x9/xnu/build-xnu.sh macos --isolated-existing x9/out/<unique-name>/dxb
```

The isolated path must be new and self-contained below `x9/out/`; the script
APFS-clones fixed inputs, relocates copied text dependencies and never edits the
shared source/object tree. Use a private build config whose `dxb` points at that
scratch tree.

## Full kernel and boot image

```bash
PATH=/opt/homebrew/opt/llvm@22/bin:$PATH \
python3 -I x9/build/build.py all \
  --boot-config x9/build/boot-device.json \
  --name <unique-name>
```

Use LLVM 22 `llvm-objcopy`/`llvm-nm` (or set `X9_OBJCOPY`/`X9_NM` explicitly).
The generated Metal allowlist currently names plugin revision **280**; plugin
binaries are mailbox/root payloads, not kernel or boot.img contents.

Current status, candidate paths and verification labels are in
[`NOTES.md`](../NOTES.md). Host compilation is not device verification. The old
GitHub macOS-runner artifact workflow and `dipa-studio/ios-sweet` download command
are historical sweet instructions and are not used for current X9 builds.
