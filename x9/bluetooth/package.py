#!/usr/bin/env python3
"""Assemble an offline native-Bluetooth root payload; never stage or install.

This is not a Metal mailbox package. It keeps both exact original launch jobs
separate from their Program-only overrides, and declares compare-before-write
requirements for a later, explicitly approved root installation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from launch_cache import CACHE_PATH, CACHE_NAME, CACHE_LIMIT, validate_pair

ROOT = Path(__file__).resolve().parents[2]
UUID = 'AA9424E8-0648-34E2-B3CF-E6C793B9D471'
STOCK_SHA = '8b7c0a0bea51544db82269d7f2775ab7996b8740927283428c4753e032df3a8f'
SETTINGS_PATH = '/System/Library/PrivateFrameworks/PreferencesUI.framework/Settings.plist'
SETTINGS_DIAG_PATH = '/usr/lib/X9SettingsBTDiag.dylib'
SETTINGS_SHA = 'eaa1d66b12febdb8f03aec21ae0724916830e499c14657d3da712039280d0957'
BINARIES = {
    'btctl': '/usr/bin/x9-btctl',
    'X9H4Transport.dylib': '/usr/lib/X9H4Transport.dylib',
    'X9BluetoothLaunch': '/usr/libexec/X9BluetoothLaunch',
    'X9BlueTool': '/usr/libexec/X9BlueTool',
    'X9SettingsBTDiag.dylib': '/usr/lib/X9SettingsBTDiag.dylib',
}
JOBS = (
    ('com.apple.bluetoothd', '/usr/sbin/bluetoothd', [],
     'com.apple.server.bluetooth', '/usr/libexec/X9BluetoothLaunch'),
    ('com.apple.BlueTool', '/usr/sbin/BlueTool', ['-R'],
     'com.apple.BlueTool', '/usr/libexec/X9BlueTool'),
)


def bounded(path, maximum):
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= maximum:
        raise ValueError(f'expected bounded regular package input: {path.name}')
    return path.read_bytes()


def settings_capability(original):
    """Drop only the Bluetooth root row's capability gate.

    20H392 PSCapabilityManager has no answer for the plain string
    "bluetooth" (unlike "wifi"/"cellular-data"), so the stock Settings root
    row is hidden on this shimmed device. The row itself stays untouched;
    the unchanged plist minus this one gate is the injected true answer.
    """
    if hashlib.sha256(original).hexdigest() != SETTINGS_SHA:
        raise ValueError('unexpected PreferencesUI Settings.plist original')
    value = plistlib.loads(original)
    if not isinstance(value, dict) or not isinstance(value.get('items'), list):
        raise ValueError('unexpected Settings.plist layout')
    hits = [item for item in value['items']
            if isinstance(item, dict) and item.get('id') == 'Bluetooth']
    if len(hits) != 1 or hits[0].get('requiredCapabilities') != ['bluetooth']:
        raise ValueError('unexpected Bluetooth root item capability gate')
    expected = plistlib.loads(original)
    for item in expected['items']:
        if isinstance(item, dict) and item.get('id') == 'Bluetooth':
            del item['requiredCapabilities']
    del hits[0]['requiredCapabilities']
    if value != expected:
        raise ValueError('Bluetooth gate edit changed unrelated Settings.plist keys')
    data = plistlib.dumps(value, fmt=plistlib.FMT_BINARY, sort_keys=False)
    if plistlib.loads(data) != value:
        raise ValueError('Settings.plist capability edit did not round-trip')
    return data


def package(runtime, out, settings_original=None):
    runtime, out = runtime.resolve(), out.resolve()
    if settings_original is not None:
        settings_original = settings_original.resolve()
    runtime.relative_to(ROOT)
    if not out.is_relative_to(ROOT / 'x9/out') or out == ROOT / 'x9/out' or out.exists():
        raise ValueError('output must be a NEW directory below this worktree x9/out/')
    value = json.loads(bounded(runtime / 'build.json', 65536))
    if (value.get('schema') != 1 or value.get('ios_build') != '20H392'
            or value.get('bluetoothd_uuid') != UUID
            or value.get('bluetoothd_sha256') != STOCK_SHA
            or value.get('client') != 'btctl'
            or value.get('bridge') != 'X9H4Transport.dylib'
            or value.get('launch_gate') != 'X9BluetoothLaunch'
            or value.get('bluetool_adapter') != 'X9BlueTool'
            or value.get('settings_diagnostic') != 'X9SettingsBTDiag.dylib'
            or value.get('settings_diagnostic_default') is not False
            or value.get('settings_diagnostic_contract') != {
                'preferences_uuid': 'B2ED1830-FB2B-3702-B254-17872B2E0A97',
                'manager_uuid': '54FB08F7-FE66-344D-B7CE-31838CDEE572',
                'mobile_bluetooth_uuid': 'FDE5B98E-2257-35C8-B323-92B824492207',
                'hook_count': 8, 'boot_arg': 'x9_bt_settings_diag=1',
                'log_interface': 'kern.x9_bt_note', 'manager_state_modified': False,
            }
            or value.get('power_interface') != 'kern.x9_bt_power'
            or value.get('power_lifecycle') != 'closed-transport-only'):
        raise ValueError('requires the complete version-locked native/power bridge build')
    launchd = value.get('launchd')
    if not isinstance(launchd, dict) or launchd.get('installed') is not False:
        raise ValueError('requires both original and candidate launch jobs, not an installed tree')
    if (launchd.get('originals') != 'launchd-originals'
            or launchd.get('candidates') != 'launchd-candidates'):
        raise ValueError('unexpected launch job input paths')
    # Hold every bounded input and validate all job identities before creating
    # ANY payload. No Apple executable is copied or modified.
    files = [(destination, bounded(runtime / name, 64 * 1024 * 1024), 0o755, None)
             for name, destination in BINARIES.items()]
    originals = []
    for label, program, args, service, replacement in JOBS:
        filename = label + '.plist'
        original = bounded(runtime / 'launchd-originals' / filename, 65536)
        candidate = bounded(runtime / 'launchd-candidates' / filename, 65536)
        stock, derived = plistlib.loads(original), plistlib.loads(candidate)
        if (not isinstance(stock, dict) or stock.get('Label') != label
                or stock.get('UserName') != 'mobile'
                or stock.get('ProgramArguments') != [program, *args]
                or stock.get('Program', program) != program
                or not isinstance(stock.get('MachServices'), dict)
                or service not in stock['MachServices']):
            raise ValueError(f'unexpected original 20H392 job identity: {label}')
        expected = dict(stock, Program=replacement)
        if derived != expected:
            raise ValueError(f'candidate changes keys other than Program: {label}')
        destination = '/System/Library/LaunchDaemons/' + filename
        digest = hashlib.sha256(original).hexdigest()
        files.append((destination, candidate, 0o644, digest))
        originals.append((filename, original, destination, digest))
    cache_original = bounded(runtime / 'launchd-originals' / CACHE_NAME, CACHE_LIMIT)
    cache_candidate = bounded(runtime / 'launchd-candidates' / CACHE_NAME, CACHE_LIMIT)
    cache_contract = validate_pair(cache_original, cache_candidate, JOBS)
    if launchd.get('cache') != dict(cache_contract,
            original='launchd-originals/' + CACHE_NAME, candidate='launchd-candidates/' + CACHE_NAME):
        raise ValueError('requires the bound append-only authoritative launchd cache update')
    cache_digest = hashlib.sha256(cache_original).hexdigest()
    settings = None
    if settings_original is not None:
        try:
            settings_original.relative_to(ROOT)
        except ValueError:
            raise ValueError('Settings.plist original must be a repo-local private input') from None
        settings_raw = bounded(settings_original, 1 << 20)
        settings_data = settings_capability(settings_raw)
        settings_digest = hashlib.sha256(settings_raw).hexdigest()
        settings = (SETTINGS_PATH, settings_data, 0o644, settings_digest,
                    'Settings.plist', settings_raw)
        files.append((SETTINGS_PATH, settings_data, 0o644, settings_digest))
    files.append((CACHE_PATH, cache_candidate, 0o644, cache_digest))
    originals.append((CACHE_NAME, cache_original, CACHE_PATH, cache_digest))
    if settings is not None:
        originals.append((settings[4], settings[5], settings[0], settings[3]))
    out.mkdir(parents=True, exist_ok=False)
    entries = []
    for destination, data, mode, previous in files:
        path = out / 'root' / destination.lstrip('/')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
        entries.append({'path': destination, 'source': path.relative_to(out).as_posix(),
                        'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                        'mode': f'{mode:04o}', 'uid': 0, 'gid': 0,
                        'precondition': {'sha256': previous} if previous else {'absent': True}})
    backups = []
    for filename, data, destination, digest in originals:
        path = out / 'originals' / filename
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o644)
        backups.append({'path': destination, 'source': path.relative_to(out).as_posix(),
                        'size': len(data), 'sha256': digest})
    manifest = {
        'schema': 1, 'kind': 'x9-bluetooth-native-root-payload',
        'ios_build': '20H392', 'bluetoothd_uuid': UUID, 'bluetoothd_sha256': STOCK_SHA,
        'runtime_build': runtime.relative_to(ROOT).as_posix(),
        'candidate_ready': value.get('candidate_ready') is True,
        'runtime_verified': False, 'audio_verified': False,
        'installed': False, 'stageable': False,
        'power_interface': 'kern.x9_bt_power', 'power_lifecycle': 'closed-transport-only',
        'settings_diagnostic': SETTINGS_DIAG_PATH, 'settings_diagnostic_default': False,
        'settings_diagnostic_contract': value['settings_diagnostic_contract'],
        'files': entries, 'original_jobs': backups[:2],
        'original_launchd_cache': backups[2],
        'original_settings': backups[3] if settings is not None else None,
        'launchd_cache': cache_contract,
        'install_order': [entry['path'] for entry in entries],
        'requirements': [
            'explicit user approval before any device root modification',
            'live check_plaintext_root.py preflight against the active private root manifest',
            'matching full native kernel candidate with x9_bt=1 x9_bt_init=1 and observed x9_bt_fc=1',
            'compare current Apple bluetoothd identity, BOTH jobs and exact original cache byte hashes before any mutation',
            'the PreferencesUI Settings.plist must match its recorded 20H392 original before the capability gate is removed',
            'new binary targets must be absent; never overwrite an unknown earlier runtime',
            'preserve exact original jobs/cache; publish executables, independent jobs, then append-only cached Program overrides',
            'fsync and readback through the same verified root route; keep a private install receipt',
            'no active-transport sleep, wake-enhance or system-suspend claim; no wake callback is installed',
            'native injection/initialization, scan, pairing, BLE, A2DP, HFP and SCO require device validation',
        ],
    }
    (out / 'root-payload.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Offline Bluetooth root payload: {out}; NOT installed or hardware verified')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--runtime', type=Path, required=True,
                    help='complete x9/bluetooth/build.py output, including both launch jobs')
    ap.add_argument('--settings-original', type=Path,
                    help='private 20H392 PreferencesUI Settings.plist original to gate the Bluetooth row open')
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    package(a.runtime, a.out, a.settings_original)
