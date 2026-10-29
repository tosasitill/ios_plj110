#!/system/bin/sh
# X9 boot record capture: /sys/fs/pstore is consumed/zapped by Android's own
# services minutes (sometimes seconds) after mount, so the HelioDarwin record
# must be copied the moment pstore is mounted.
mkdir -p /data/local/tmp/x9boot
cp /sys/fs/pstore/* /data/local/tmp/x9boot/ 2>/dev/null
chmod 644 /data/local/tmp/x9boot/* 2>/dev/null
