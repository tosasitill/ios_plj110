#!/usr/bin/env python3
"""Read-only Android speaker evidence. Writes only a new host .local directory.

No mixer set, playback, calibration, reset, debug RPC, mount, or device write.
Run with Android connected; optionally play media manually during collection.
Captured driver bindings/firmware are evidence, not proof of iOS sound output.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial', default=os.environ.get('X9_SERIAL'))
    ap.add_argument('--card', type=int, default=0, choices=range(8))
    ap.add_argument('--out', type=Path, required=True,
                    help='new directory inside repository .local; raw data must not enter git')
    a = ap.parse_args()
    out = a.out.expanduser().resolve()
    if not out.is_relative_to((ROOT / '.local').resolve()) or out.exists():
        ap.error('--out must be a NEW directory within .local')
    adb = ['adb'] + (['-s', a.serial] if a.serial else [])
    # One presence check, no retries or getvar/reboot. Fail before making a
    # capture that could be mistaken for evidence from a connected device.
    try:
        subprocess.run(adb + ['get-state'], check=True, capture_output=True, timeout=10)
        root = subprocess.run(adb + ['exec-out', 'su', '-c', shlex.quote('id -u')],
                              check=True, capture_output=True, timeout=15)
        if root.stdout.strip() != b'0':
            raise RuntimeError('root read access unavailable')
    except (OSError, subprocess.SubprocessError, RuntimeError) as e:
        sys.exit(f'Android/root unavailable; no collection or retry: {e}')
    out.mkdir(parents=True, mode=0o700)
    os.chmod(out, 0o700)
    records = []

    def capture(name, command, required=False):
        started = datetime.datetime.now().astimezone().isoformat()
        # Streaming to a private host file also permits binary firmware; no
        # shell on the host, no base64, no source/output mixing.
        with (out / name).open('xb') as data, (out / (name + '.stderr')).open('xb') as err:
            os.chmod(out / name, 0o600)
            os.chmod(out / (name + '.stderr'), 0o600)
            try:
                r = subprocess.run(adb + ['exec-out', 'su', '-c', shlex.quote(command)],
                                   stdout=data, stderr=err, timeout=30)
                code = r.returncode
            except subprocess.TimeoutExpired:
                code = 124
        records.append(dict(file=name, command=command, started=started, exit=code,
                            bytes=(out / name).stat().st_size))
        if required and code:
            raise RuntimeError(f'required capture failed: {name} ({code})')

    error = None
    try:
        capture('identity.txt', 'getprop ro.product.device && getprop ro.board.platform && '
                'cat /proc/sys/kernel/random/boot_id && uname -r', True)
        capture('alsa.txt', 'cat /proc/asound/cards && cat /proc/asound/pcm', True)
        capture('pa-binding.txt',
                'for n in 6-0034 6-0035 6-005c 6-005d; do '
                'p=/sys/bus/i2c/devices/$n; echo "=== $n ==="; '
                'if [ -d "$p" ]; then readlink "$p/driver"; '
                'for f in name modalias uevent; do [ ! -r "$p/$f" ] || cat "$p/$f"; done; '
                'else echo absent; fi; done')
        capture('pa-module.txt',
                'for m in tfa98xx tfa98xx_v6 tfa98xx-v6; do '
                'p=/sys/module/$m; [ ! -d "$p" ] || { echo "=== $m ==="; '
                'for f in version parameters/fw_name parameters/fw_name_reverse; do '
                '[ ! -r "$p/$f" ] || { echo "$f"; cat "$p/$f"; }; done; }; done')
        # Only these three debugfs getters are software-state/string reads in
        # OPPO tfa98xx.c. Deliberately exclude R/TEMP/OTC/MTPEX/calibrate/rpc.
        capture('pa-state.txt',
                'for p in /sys/kernel/debug/tfa98xx-34 /sys/kernel/debug/tfa98xx-35; do '
                'echo "=== $p ==="; for f in version dsp-state fw-state; do '
                '[ ! -r "$p/$f" ] || { echo "$f"; cat "$p/$f"; }; done; done')
        capture('tinymix.txt',
                'if command -v tinymix >/dev/null 2>&1; then '
                f'tinymix -D {a.card}; else echo tinymix-unavailable; exit 127; fi')
        capture('audio-kernel.txt',
                "dmesg | grep -i -E 'tfa98|rt5512|adsp|audio_extend|I2SOUT4|I2SIN4' | tail -n 1500")
        # Read the DT's optional firmware override without guessing an I2C
        # address-space register or changing the driver register selector.
        for addr in ('0034', '0035'):
            capture(f'tfa-{addr}-fw-property.bin',
                    f'cat /sys/bus/i2c/devices/6-{addr}/of_node/tfa_fw_name')
        # Default/request_firmware alternatives only. A successful download
        # does not establish which profile or which file the driver loaded.
        for partition in ('vendor', 'odm'):
            for name in ('tfa98xx.cnt', 'tfa98xx_reverse.cnt'):
                capture(f'{partition}-{name}', f'cat /{partition}/firmware/{name}')
        capture('boot-end.txt', 'cat /proc/sys/kernel/random/boot_id', True)
    except (RuntimeError, OSError) as e:
        error = str(e)
    finally:
        manifest = out / 'capture.json'
        manifest.write_text(json.dumps(dict(started=records[0]['started'] if records else None,
            card=a.card, serial=a.serial, complete=error is None, error=error, captures=records),
            ensure_ascii=False, indent=2) + '\n')
        os.chmod(manifest, 0o600)
    print(f'Saved private read-only capture: {out}; no device writes')
    if error:
        sys.exit(error)


if __name__ == '__main__':
    main()
