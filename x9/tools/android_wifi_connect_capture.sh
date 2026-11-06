#!/bin/bash
# Capture Android gen4m's full join sequence to RUC-Web (open network). Run on the Mac: bash connect_capture.sh OUTDIR
export PATH=/opt/homebrew/bin:$PATH
S=$(adb devices | awk 'NR==2{print $1}')
D=$1; mkdir -p "$D"
r() { adb -s "$S" exec-out su -c "$1"; }
r 'cmd wifi disconnect 2>/dev/null; cmd wifi forget-network $(cmd wifi list-networks 2>/dev/null | awk "/RUC-Web/{print \$1}") 2>/dev/null; true'
sleep 3
r 'cat /proc/net/wlan/dbgLevel' > "$D/dbglevel-before.txt"
r 'echo 0xFF:0x13F > /proc/net/wlan/dbgLevel; echo "set_cfg DataTxDone 3" > /proc/net/wlan/driver; cat /proc/net/wlan/dbgLevel' > "$D/dbglevel-after.txt"
# stream kernel log while connecting
adb -s "$S" exec-out su -c 'timeout 45 cat /proc/kmsg' > "$D/kmsg.txt" &
KP=$!
sleep 2
r 'cmd wifi connect-network RUC-Web open' > "$D/connect.txt" 2>&1
sleep 25
r 'cmd wifi status; ip addr show wlan0' > "$D/status.txt" 2>&1
wait $KP
r 'echo 0xFF:0x10F > /proc/net/wlan/dbgLevel; echo "set_cfg DataTxDone 0" > /proc/net/wlan/driver'
r 'dmesg' > "$D/dmesg-after.txt"
r 'logcat -b all -d 2>/dev/null | grep -i -E "wpa_supplicant|wifi|wlan" | tail -4000' > "$D/logcat-wifi.txt"
ls -la "$D"
