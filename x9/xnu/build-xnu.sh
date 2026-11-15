#!/bin/bash
# Build the X9 SWEET XNU object tree (xnu-8796.141.3 + x9/xnu patches).
#
#   x9/xnu/build-xnu.sh [macos|ios] [--setup | --isolated x9/out/<name> |
#                                  --isolated-existing x9/out/<name>]
#
# --isolated clones only the fixed local compiler/bootstrap inputs into a new
# scratch tree, restores XNU from the pinned tag, applies this worktree's patches,
# and builds there. It never edits the shared .local source or object trees.
#
#   macos  -> .local/dxb          macOS kext C++ ABI (vtable padding); the tree
#                                 every proven boot image so far was linked against
#   ios    -> .local/dxb-iosabi   iOS kext C++ ABI (0002) + sparse-prelink kext memory
#                                 (0003) + unaligned ml_static_mfree report (0004, diagnostic) + prelinked kext
#                                 load failure report (0005, diagnostic);
#                                 needed for the original 20H392 kexts
#
# --setup creates the tree from scratch (network): blacktop/darwin-xnu-build at
# the pinned commit + dxb-build.sh.patch, apple-oss xnu at xnu-8796.141.3 + the
# patches here, then the harness builds bootstrap_cmds/dtrace/libfirehose/
# libsyscall/headers into fakeroot. For "ios" an existing .local/dxb is cloned
# instead (APFS clone, no extra space) and only the 0002 patch is added.
#
# Without --setup the existing tree's xnu source must equal tag+patches
# (checked) and only `make install` for DEVELOPMENT ARM64 SWEET runs; output:
# <tree>/build/xnu.obj/DEVELOPMENT_ARM64_SWEET, which x9/build/build.py links.
# Only compile tools live outside the repository: Xcode 15 (DEVELOPER_DIR),
# KDK_13.5_22G74, Homebrew python3.
set -euo pipefail
ABI=${1:-macos}; SETUP=0; ISOLATED=; EXISTING=0
if [ $# -gt 0 ]; then shift; fi
while [ $# -gt 0 ]; do
  case $1 in
    --setup) SETUP=1; shift ;;
    --isolated) [ $# -ge 2 ] || { echo 'error: --isolated needs a new x9/out path' >&2; exit 2; }
      ISOLATED=$2; shift 2 ;;
    --isolated-existing) [ $# -ge 2 ] || { echo 'error: --isolated-existing needs an existing x9/out path' >&2; exit 2; }
      ISOLATED=$2; EXISTING=1; shift 2 ;;
    *) echo "error: unknown option $1" >&2; exit 2 ;;
  esac
done
[ -z "$ISOLATED" ] || [ $SETUP = 0 ] || { echo 'error: --setup and --isolated are exclusive' >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
P=$ROOT/x9/xnu
DEV=${DEVELOPER_DIR:-/Users/tosasitill/Downloads/Xcode.app/Contents/Developer}
KDK=${KDKROOT:-/Library/Developer/KDKs/KDK_13.5_22G74.kdk}
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_COMMIT=663906528d1cd5ea8fde7de6f16de73db9511549
XNU_REPO=https://github.com/apple-oss-distributions/xnu.git
XNU_TAG=xnu-8796.141.3
PYTHON3=$(command -v python3)
case $ABI in
  macos) D=$ROOT/.local/dxb; PATCHES="$P/0001-x9-sweet-xnu-8796.141.3.patch $P/0006-x9-bt-uart-irq.patch" ;;
  ios)   D=$ROOT/.local/dxb-iosabi; PATCHES="$P/0001-x9-sweet-xnu-8796.141.3.patch $P/0002-x9-ios-kext-cxx-abi.patch $P/0003-x9-sparse-prelink-kext-memory.patch $P/0004-x9-diag-unaligned-static-mfree.patch $P/0005-x9-diag-prelinked-kext-load.patch $P/0006-x9-bt-uart-irq.patch" ;;
  *) echo "usage: $0 [macos|ios] [--setup | --isolated x9/out/<name>]" >&2; exit 2 ;;
esac

if [ -n "$ISOLATED" ]; then
  BASE=$D
  # Path normalization also rejects an output symlink escaping this worktree.
  D=$("$PYTHON3" - "$ROOT" "$ISOLATED" "$EXISTING" <<'PY'
import sys
from pathlib import Path
root = Path(sys.argv[1]).resolve()
p = Path(sys.argv[2]).expanduser()
p = (p if p.is_absolute() else root / p).resolve()
out = (root / 'x9/out').resolve()
if not p.is_relative_to(out) or p == out:
    raise SystemExit('error: isolated tree must be below this worktree x9/out/')
existing = sys.argv[3] == '1'
if not existing and p.exists():
    raise SystemExit('error: --isolated requires a NEW directory')
if existing:
    for name in ('xnu', 'xnu/.git', 'build', 'build/xnu.obj', 'fakeroot'):
        child = p / name
        if child.is_symlink() or not child.is_dir() or not child.resolve().is_relative_to(p):
            raise SystemExit('error: --isolated-existing requires a self-contained existing scratch tree')
    if (p / 'cc').is_symlink():
        raise SystemExit('error: isolated compiler wrappers must not be a symlink')
print(p)
PY
  )
  if [ $EXISTING = 0 ]; then
  [ -d "$BASE/xnu/.git" ] && [ -d "$BASE/build/xnu.obj" ] && [ -d "$BASE/fakeroot" ] || {
    echo "error: fixed $BASE bootstrap/object inputs missing" >&2; exit 1;
  }
  mkdir -p "$D"
  # APFS copy-on-write, not hardlinks: rebuilding cannot touch shared objects.
  for e in build fakeroot AvailabilityVersions bootstrap_cmds dtrace libdispatch libplatform templates; do
    [ ! -e "$BASE/$e" ] || cp -c -R "$BASE/$e" "$D/$e"
  done
  git clone -q --no-hardlinks --no-checkout "$BASE/xnu" "$D/xnu"
  git -C "$D/xnu" checkout -q "$XNU_TAG"
  git -C "$D/xnu" apply --whitespace=nowarn $PATCHES
  # Copied depfiles and generated build scripts use the old absolute tree.
  # Only copied text inputs are relocated. Binary objects are left intact;
  # changed source/header dependencies force normal make recompilation.
  python3 - "$BASE" "$D" <<'PY'
import os
import sys
from pathlib import Path
base, target = map(Path, sys.argv[1:])
old = {os.fsencode(base), os.fsencode(base.resolve())}
new = os.fsencode(target)
for directory in ('build', 'fakeroot'):
    for p in (target / directory).rglob('*'):
        if p.is_symlink():
            link = os.fsencode(os.readlink(p))
            changed = link
            for source in old:
                changed = changed.replace(source + b'/', new + b'/')
            if changed != link:
                p.unlink()
                p.symlink_to(os.fsdecode(changed))
        elif p.is_file():
            data = p.read_bytes()
            if b'\0' in data or not any(source + b'/' in data for source in old):
                continue
            changed = data
            for source in old:
                changed = changed.replace(source + b'/', new + b'/')
            if changed != data:
                p.write_bytes(changed)
PY
  fi
fi

wrappers() {
  mkdir -p "$D/cc"
  for c in clang clang++; do
    printf '#!/bin/sh\nexec "%s/Toolchains/XcodeDefault.xctoolchain/usr/bin/%s" "$@"\n' "$DEV" "$c" > "$D/cc/$c"
  done
  # XNU's built-in dSYM macro installer uses /usr/bin/env python. Its source
  # checker is Python-3-compatible; provide ONLY this exact tool alias rather
  # than adding arbitrary user bins or ignoring an install failure.
  printf '#!/bin/sh\nexec "%s" "$@"\n' "$PYTHON3" > "$D/cc/python"
  chmod +x "$D/cc/"*
}

verify_source() {
  local t; t=$(mktemp -d /tmp/x9xnu.XXXX)
  git -C "$D/xnu" archive "$XNU_TAG" | tar -x -C "$t"
  (cd "$t" && git init -q && git apply --whitespace=nowarn $PATCHES && rm -rf .git)
  if ! diff -rq -x .git "$t" "$D/xnu" >/dev/null; then
    rm -rf "$t"
    echo "error: $D/xnu differs from $XNU_TAG + $(basename -a $PATCHES | tr '\n' ' ')" >&2
    echo "       commit kernel source changes as a patch in x9/xnu first" >&2
    exit 1
  fi
  rm -rf "$t"
}

if [ $SETUP = 1 ]; then
  [ -e "$D" ] && { echo "error: $D exists" >&2; exit 1; }
  if [ $ABI = ios ] && [ -d "$ROOT/.local/dxb/build/xnu-hdrs.obj" ]; then
    mkdir -p "$D/build"
    for e in xnu fakeroot AvailabilityVersions bootstrap_cmds dtrace libdispatch libplatform templates; do
      [ -e "$ROOT/.local/dxb/$e" ] && cp -c -R "$ROOT/.local/dxb/$e" "$D/$e"
    done
    for b in bootstrap_cmds.obj bootstrap_cmds.sym dtrace.obj dtrace.sym libfirehose_kernel.obj \
             libfirehose_kernel.sym libsyscall.obj libsyscall.sym xnu-hdrs.obj; do
      [ -e "$ROOT/.local/dxb/build/$b" ] && cp -c -R "$ROOT/.local/dxb/build/$b" "$D/build/$b"
    done
    git -C "$D/xnu" apply "$P/0002-x9-ios-kext-cxx-abi.patch" "$P/0003-x9-sparse-prelink-kext-memory.patch" "$P/0004-x9-diag-unaligned-static-mfree.patch" \
      "$P/0005-x9-diag-prelinked-kext-load.patch"
    # macOS may already carry 0006; an older fixed tree may not.
    if ! grep -q x9_bluetooth_irq_filter "$D/xnu/osfmk/arm64/sleh.c"; then
      git -C "$D/xnu" apply "$P/0006-x9-bt-uart-irq.patch"
    fi
  else
    git clone "$DXB_REPO" "$D"
    git -C "$D" checkout -q "$DXB_COMMIT"
    git -C "$D" apply "$P/dxb-build.sh.patch"
    git clone -q --branch "$XNU_TAG" --depth 1 "$XNU_REPO" "$D/xnu"
    git -C "$D/xnu" apply --whitespace=nowarn $PATCHES
    wrappers
    # The harness sed edits are already part of 0001 (no-ops here); its own
    # patches/ directory stays empty because 0001 is the complete source delta.
    (cd "$D" && CCACHE_WRAP="$D/cc" DEVELOPER_DIR="$DEV" KERNEL_CONFIG=DEVELOPMENT \
       MACHINE_CONFIG=SWEET MACOS_VERSION=13.5 KDKROOT="$KDK" ./build.sh)
    exit 0
  fi
fi

[ -d "$D/xnu" ] || { echo "error: $D missing; run $0 $ABI --setup" >&2; exit 1; }
verify_source
wrappers
export DEVELOPER_DIR=$DEV
export PATH="$D/cc:$DEV/Toolchains/XcodeDefault.xctoolchain/usr/bin:$DEV/usr/bin:/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin"
export CC="$D/cc/clang" CXX="$D/cc/clang++"
export LD="$(xcrun -find ld)" AR="$(xcrun -find ar)" RANLIB="$(xcrun -find ranlib)" STRIP="$(xcrun -find strip)" LIBTOOL="$(xcrun -find libtool)"
# Reproduce the banner of the 20H392-era kernel the userspace expects.
export KERNEL_BUILD_DATE="Mon Aug 14 05:06:07 UTC 2023" KERNEL_BUILD_OBJROOT="xnu-8796.141.3"
cd "$D/xnu"
# Require real make/tee success AND a fresh kernel link; no dSYM or other
# nonzero status is swallowed. A moved tree has stale paths in its .d files:
# rewrite them first (perl -pi -e 's#<old>/#<new>/#g' on build/ text files).
K=$D/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet
if [ $EXISTING = 1 ]; then
  # Preserve the previous completed link outputs, then force just the normal
  # final link/strip/install dependency chain, not another entire tree copy.
  PREVIOUS=$(mktemp -d "$D/build/x9-previous-link.XXXX")
  for f in "$K" "$K.unstripped"; do
    [ ! -f "$f" ] || mv "$f" "$PREVIOUS/$(basename "$f")"
  done
fi
STAMP=$(mktemp /tmp/x9xnu-stamp.XXXX)
LOG=$(mktemp /tmp/x9xnu-make.XXXX)
set +e
make install -j8 VERBOSE=YES \
  CTFCONVERT=$D/fakeroot/usr/local/bin/ctfconvert CTFMERGE=$D/fakeroot/usr/local/bin/ctfmerge CTFDUMP=$D/fakeroot/usr/local/bin/ctfdump \
  SDKROOT=macosx TARGET_CONFIGS="DEVELOPMENT ARM64 SWEET" CONCISE=0 LOGCOLORS=n BUILD_WERROR=0 BUILD_LTO=0 \
  SRCROOT="$D/xnu" OBJROOT="$D/build/xnu.obj" SYMROOT="$D/build/xnu.sym" DSTROOT="$D/fakeroot" FAKEROOT_DIR="$D/fakeroot" \
  KDKROOT="$KDK" TIGHTBEAMC=tightbeamc-not-supported RC_DARWIN_KERNEL_VERSION=22.6.0 \
  MEMORY_SIZE=$(sysctl -n hw.memsize) SYSCTL_HW_PHYSICALCPU=$(sysctl -n hw.physicalcpu) \
  SYSCTL_HW_LOGICALCPU=$(sysctl -n hw.logicalcpu) KERNEL_BUILDS_IN_PARALLEL=1 2>&1 | tee "$LOG"
RESULT=("${PIPESTATUS[@]}")
set -e
if [ "${RESULT[0]}" != 0 ] || [ "${RESULT[1]}" != 0 ] ||
   grep -q -E "No rule to make target|error:" "$LOG" || [ ! "$K" -nt "$STAMP" ] ||
   ! grep -q -E '^[[:space:]]*LD kernel\.development\.sweet\.unstripped$' "$LOG"; then
  echo "error: XNU build failed (make=${RESULT[0]} tee=${RESULT[1]}, fresh kernel link required); see $LOG" >&2
  rm -f "$STAMP"; exit 1
fi
rm -f "$STAMP" "$LOG"
echo "XNU object tree ready: $D/build/xnu.obj/DEVELOPMENT_ARM64_SWEET"
