#!/bin/bash
# Capture Android gen4m while the user joins networks by hand (e.g. RUC-Mobile, then RUC-Web).
# Run on the Mac: bash android_wifi_manual_capture.sh OUTDIR [SECONDS]  (default 240)
# Turns on all wlan debug modules (0xFF:0x13F) and TXD dumps of management frames (set_cfg DataTxDone 3),
# streams /proc/kmsg for the window, restores the defaults, then saves dmesg and Wi-Fi logcat.
export PATH=/opt/homebrew/bin:$PATH
S=$(adb devices | awk 'NR==2{print $1}')
D=$1; T=${2:-240}; mkdir -p "$D"
r() { adb -s "$S" exec-out su -c "$1"; }
r 'cat /proc/net/wlan/dbgLevel' > "$D/dbglevel-before.txt"
r 'cmd wifi status' > "$D/status-before.txt" 2>&1
r 'echo 0xFF:0x13F > /proc/net/wlan/dbgLevel; echo "set_cfg DataTxDone 3" > /proc/net/wlan/driver; cat /proc/net/wlan/dbgLevel' > "$D/dbglevel-after.txt"
date +%s > "$D/start-epoch.txt"
adb -s "$S" exec-out su -c "timeout $T cat /proc/kmsg" > "$D/kmsg.txt"
r 'echo 0xFF:0x10F > /proc/net/wlan/dbgLevel; echo "set_cfg DataTxDone 0" > /proc/net/wlan/driver'
r 'cmd wifi status; ip addr show wlan0' > "$D/status-after.txt" 2>&1
r 'dmesg' > "$D/dmesg-after.txt"
r 'logcat -b all -d 2>/dev/null | grep -i -E "wpa_supplicant|wifi|wlan" | tail -6000' > "$D/logcat-wifi.txt"
echo done > "$D/DONE"
ls -la "$D"
