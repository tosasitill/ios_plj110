#!/usr/bin/env python3
"""Derive offline boot configurations for the one-file Bluetooth diagnostic."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'x9/tools'))
from shared import payload
from x9_boot_defaults import board_defaults

BT_STOCK = {'x9_bt': '1', 'x9_bt_init': '1', 'x9_bt_fc': '1'}
SCOPED_KEYS = {'x9_bt_root', 'x9_bt_settings_diag', 'x9_bt_audio_diag'}
# Each key below is consumed solely through sweet_x9_arg() and the token's
# explicit value equals its pe_serial.c board default, so dropping the token
# changes nothing: x9pl/x9sl by the render engine, x9_wifi_power by the Wi-Fi
# bring-up stage reader (X9WifiNet.inc). Do not drop other defaults: some are
# read directly from the raw boot command line.
COMPACT = ('x9pl', 'x9sl', 'x9_wifi_power')


def derive(base):
    tokens = base['boot_args']
    options = base['options']
    if (not isinstance(tokens, list) or not isinstance(options, list)
            or not all(isinstance(x, str) and x and not any(c.isspace() or c == '\0' for c in x)
                       for x in tokens)):
        raise ValueError('invalid boot argument list')
    keyed = {}
    for token in tokens:
        key, sep, val = token.partition('=')
        if key in keyed or key in SCOPED_KEYS:
            raise ValueError(f'duplicate or preconfigured selector: {key}')
        keyed[key] = val if sep else None
    if any(keyed.get(key) != value for key, value in BT_STOCK.items()):
        raise ValueError('use the full X9 boot-device config with explicit board BT selectors')

    def fit(args):
        return len(' '.join(args).encode('utf-8')) < payload.BOOT_LINE_LENGTH

    defaults = board_defaults()
    results = {}
    for mode, selector, diag, audio in (('verify', '0', '1', '1'),
                                        ('install', '1', '1', '1'),
                                        ('rollback', '2', '0', '0')):
        args = list(tokens)
        if mode == 'rollback':
            args = [{'x9_bt_init=1': 'x9_bt_init=0', 'x9_bt_fc=1': 'x9_bt_fc=0'}.get(t, t)
                    for t in args]
        tail = [f'x9_bt_root={selector}', f'x9_bt_settings_diag={diag}',
                f'x9_bt_audio_diag={audio}']
        for key in COMPACT:
            if fit(args + tail):
                break
            token = f'{key}={defaults.get(key)}'
            if defaults.get(key) is not None and token in args:
                args.remove(token)
        if not fit(args + tail):
            raise ValueError(f'{mode} boot line exceeds {payload.BOOT_LINE_LENGTH - 1} bytes')
        results[mode] = dict(base, boot_args=args + tail)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--boot-config', type=Path, default=ROOT / 'x9/build/boot-device.json')
    parser.add_argument('--out', type=Path, required=True)
    a = parser.parse_args()
    try:
        configs = derive(json.loads(a.boot_config.read_text()))
        a.out.mkdir(parents=True, exist_ok=False)
        for mode, config in configs.items():
            file = a.out / f'boot-{mode}.json'
            with file.open('x') as out:
                json.dump(config, out, indent=2)
                out.write('\n')
            print(f'{mode}: {len(" ".join(config["boot_args"]).encode())} bytes, {file}')
    except (OSError, ValueError, KeyError) as e:
        sys.exit(f'settings_boot failed: {e}')


if __name__ == '__main__':
    main()
