#!/bin/sh
# x9 storm prep: materialize the staged storm payload onto the Data volume
# and flip the public prefix symlink target. Idempotent. Must run as root.
# Usage: prep.sh [version]   (default: the single staged payload version)
set -eu
PAYLOAD_ROOT=/private/var/x9/storm/payload
DATA_ROOT=/private/var/mobile/Library/X9Storm

if [ "$(id -u)" != "0" ]; then
    echo "storm-prep: must run as root" >&2
    exit 1
fi
if [ $# -ge 1 ]; then
    VERSION=$1
else
    VERSION=$(ls "$PAYLOAD_ROOT" | head -n 1)
fi
SRC="$PAYLOAD_ROOT/$VERSION"
if [ ! -f "$SRC/VERSION" ]; then
    echo "storm-prep: staged payload $VERSION not found" >&2
    exit 1
fi

mkdir -p "$DATA_ROOT"
DST="$DATA_ROOT/$VERSION"
if [ ! -d "$DST" ]; then
    TMP="$DATA_ROOT/.prep-$VERSION-$$"
    rm -rf "$TMP"
    mkdir -p "$TMP"
    cp -a "$SRC/." "$TMP/"
    # dpkg state belongs to the Data copy, never to the staged payload.
    mkdir -p "$TMP/Library/dpkg/info" "$TMP/Library/dpkg/updates" \
             "$TMP/Library/dpkg/parts" "$TMP/Library/dpkg/alternatives" \
             "$TMP/Library/dpkg/triggers"
    [ -f "$TMP/Library/dpkg/status" ] || : > "$TMP/Library/dpkg/status"
    chmod 755 "$TMP" "$TMP/Library/dpkg"
    mv "$TMP" "$DST"
    echo "storm-prep: materialized $VERSION"
else
    echo "storm-prep: $VERSION already materialized"
fi

# apt state/cache skeleton (0.2.0): the bootstrap tree ships these,
# but create them unconditionally so a tree materialized by an older
# prep gains them too. apt itself also creates them on demand; having
# them from the start keeps ownership/permissions uniform.
mkdir -p "$DST/var/lib/apt/lists/partial" \
         "$DST/var/cache/apt/archives/partial" \
         "$DST/var/log/apt" \
         "$DST/etc/apt/sources.list.d" "$DST/etc/apt/preferences.d" \
         "$DST/etc/apt/apt.conf.d" "$DST/etc/apt/trusted.gpg.d"

# Atomic flip of the public target.
ln -s "$DST" "$DATA_ROOT/.current-$$"
mv -f "$DATA_ROOT/.current-$$" "$DATA_ROOT/current"

# Fallback: if the staged /private/var/storm symlink is absent and the live
# /private/var is writable, create it here (see README discriminator 2).
if [ ! -L /private/var/storm ]; then
    ln -s "$DATA_ROOT/current" /private/var/storm 2>/dev/null || \
        echo "storm-prep: note: /private/var/storm not created (not writable?)" >&2
fi

echo "storm-prep: current -> $DST"
