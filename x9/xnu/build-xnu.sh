#!/bin/bash
# Build the X9 SWEET XNU object tree (xnu-8796.141.3 + x9/xnu patches).
#
#   x9/xnu/build-xnu.sh [macos|ios] [--setup]
#
#   macos  -> .local/dxb          macOS kext C++ ABI (vtable padding); the tree
#                                 every proven boot image so far was linked against
#   ios    -> .local/dxb-iosabi   iOS kext C++ ABI (0002 patch); needed to link the
#                                 original 20H392 kexts (IOUSBDeviceFamily, ...)
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
ABI=${1:-macos}; SETUP=0
[ "${2:-}" = --setup ] && SETUP=1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
P=$ROOT/x9/xnu
DEV=${DEVELOPER_DIR:-/Users/tosasitill/Downloads/Xcode.app/Contents/Developer}
KDK=${KDKROOT:-/Library/Developer/KDKs/KDK_13.5_22G74.kdk}
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_COMMIT=663906528d1cd5ea8fde7de6f16de73db9511549
XNU_REPO=https://github.com/apple-oss-distributions/xnu.git
XNU_TAG=xnu-8796.141.3
case $ABI in
  macos) D=$ROOT/.local/dxb; PATCHES="$P/0001-x9-sweet-xnu-8796.141.3.patch" ;;
  ios)   D=$ROOT/.local/dxb-iosabi; PATCHES="$P/0001-x9-sweet-xnu-8796.141.3.patch $P/0002-x9-ios-kext-cxx-abi.patch" ;;
  *) echo "usage: $0 [macos|ios] [--setup]" >&2; exit 2 ;;
esac

wrappers() {
  mkdir -p "$D/cc"
  for c in clang clang++; do
    printf '#!/bin/sh\nexec "%s/Toolchains/XcodeDefault.xctoolchain/usr/bin/%s" "$@"\n' "$DEV" "$c" > "$D/cc/$c"
  done
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
    git -C "$D/xnu" apply "$P/0002-x9-ios-kext-cxx-abi.patch"
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
export PATH="$DEV/Toolchains/XcodeDefault.xctoolchain/usr/bin:$DEV/usr/bin:/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin"
export CC="$D/cc/clang" CXX="$D/cc/clang++"
export LD="$(xcrun -find ld)" AR="$(xcrun -find ar)" RANLIB="$(xcrun -find ranlib)" STRIP="$(xcrun -find strip)" LIBTOOL="$(xcrun -find libtool)"
# Reproduce the banner of the 20H392-era kernel the userspace expects.
export KERNEL_BUILD_DATE="Mon Aug 14 05:06:07 UTC 2023" KERNEL_BUILD_OBJROOT="xnu-8796.141.3"
cd "$D/xnu"
# make exits non-zero only for the lldbmacros dSYM install; the kernel objects
# are complete when build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet exists.
make install -j8 VERBOSE=YES \
  CTFCONVERT=$D/fakeroot/usr/local/bin/ctfconvert CTFMERGE=$D/fakeroot/usr/local/bin/ctfmerge CTFDUMP=$D/fakeroot/usr/local/bin/ctfdump \
  SDKROOT=macosx TARGET_CONFIGS="DEVELOPMENT ARM64 SWEET" CONCISE=0 LOGCOLORS=n BUILD_WERROR=0 BUILD_LTO=0 \
  SRCROOT="$D/xnu" OBJROOT="$D/build/xnu.obj" SYMROOT="$D/build/xnu.sym" DSTROOT="$D/fakeroot" FAKEROOT_DIR="$D/fakeroot" \
  KDKROOT="$KDK" TIGHTBEAMC=tightbeamc-not-supported RC_DARWIN_KERNEL_VERSION=22.6.0 \
  MEMORY_SIZE=$(sysctl -n hw.memsize) SYSCTL_HW_PHYSICALCPU=$(sysctl -n hw.physicalcpu) \
  SYSCTL_HW_LOGICALCPU=$(sysctl -n hw.logicalcpu) KERNEL_BUILDS_IN_PARALLEL=1 || true
test -f "$D/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet"
echo "XNU object tree ready: $D/build/xnu.obj/DEVELOPMENT_ARM64_SWEET"
