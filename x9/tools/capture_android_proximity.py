#!/usr/bin/env python3
"""Read-only Android PS/SCP evidence. Never enables a sensor or writes a node.

Run after opening Android's existing sensor test and manually covering/uncovering
its front receiver. X9_SERIAL must select the device; output is private x9/out.
Missing near/far log lines are NOT proof of a missing proximity sensor.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    out = (args.out or ROOT / 'x9/out' /
           ('android-proximity-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))).resolve()
    if not out.is_relative_to((ROOT / 'x9/out').resolve()) or out.exists():
        parser.error('use a NEW private directory under this checkout x9/out/')
    serial, adb = os.environ.get('X9_SERIAL'), shutil.which('adb')
    if not serial or not adb:
        parser.error('set X9_SERIAL and install adb; no arbitrary device selection')
    base = [adb, '-s', serial]
    try:
        state = subprocess.run(base + ['get-state'], capture_output=True, timeout=10)
    except subprocess.TimeoutExpired:
        parser.error('adb get-state timed out; no capture created')
    if state.returncode or state.stdout.strip() != b'device':
        parser.error('selected Android device unavailable; no capture created')
    out.mkdir(parents=True, mode=0o700)
    records = []
    selected = "grep -i -E 'proxim|psensor|tcs37|alsps|sensor_ready|sensor_list|sensorhub|sensor_comm|\\[SCP\\]'"
    for name, command, root, timeout in [
        ('sensorservice.txt', 'dumpsys sensorservice', False, 45),
        ('device-tree.dtb', 'cat /sys/firmware/fdt', True, 30),
        ('sensor-kernel.txt', 'dmesg | ' + selected, True, 30),
        ('sensor-logcat.txt', 'logcat -d -b all -v threadtime | ' + selected, False, 45),
        ('scp-status.txt', 'for n in scp_A_status scp_reset_counts; do '
         'p=/sys/class/misc/scp/$n; [ ! -f "$p" ] || { echo "$n"; cat "$p"; }; done', True, 30),
    ]:
        argv = base + ['exec-out'] + (['su', '-c', command] if root else ['sh', '-c', command])
        try:
            result = subprocess.run(argv, capture_output=True, timeout=timeout)
            code, data, errors = result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired as e:
            code, data, errors = 124, e.stdout or b'', e.stderr or b''
        for filename, contents in [(name, data), (name + '.stderr', errors)]:
            p = out / filename
            p.write_bytes(contents)
            p.chmod(0o600)
        records.append({'file': name, 'command': command, 'exit': code,
                        'bytes': len(data), 'complete': code == 0})
        print(f'{name}: exit={code} bytes={len(data)}')
    manifest = out / 'capture.json'
    manifest.write_text(json.dumps({'read_only': True, 'captures': records,
        'polarity': 'unverified: manually correlate actual PS values with near/far',
        'hardware_enabled_by_script': False}, indent=2) + '\n')
    manifest.chmod(0o600)
    print(out)
    return 0 if records[0]['complete'] and records[0]['bytes'] else 2

if __name__ == '__main__':
    raise SystemExit(main())
