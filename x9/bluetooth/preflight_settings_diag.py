#!/usr/bin/env python3
"""Strictly read-only preflight checker for single-file Preferences BT diag.

Validates offline inputs (payload, Apple originals, optional root-manifest) and
optionally performs a bounded read-only HFSX catalog check for:
  - clean HFSX journal
  - /usr and /usr/lib non-symlink directories
  - absence of /usr/lib/X9SettingsBTDiag.dylib.x9-bt-temp
  - /usr/lib/X9SettingsBTDiag.dylib precondition for verify/install/rollback
  - /usr/sbin/bluetoothd and /usr/sbin/BlueTool identity match

This tool NEVER writes to the device, HFSX image, or host root.
"""
import argparse
import hashlib
import json
from pathlib import Path
import stat
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import root_header
import settings_header

DIAG_PATH = settings_header.PATH
TEMP_PATH = DIAG_PATH + '.x9-bt-temp'
DEFAULT_READ_BUDGET = 32 * 1024 * 1024  # 32 MiB bounded read-only probe


class PreflightError(ValueError):
    """Fail-closed preflight violation."""


class ReadOnlyBudgetedDevice:
    """Read-only block reader adapter for TargetHFS with a strict byte budget."""

    def __init__(self, reader, max_bytes=DEFAULT_READ_BUDGET):
        self._reader = reader
        self.max_bytes = max_bytes
        self.read_bytes = 0

    def read(self, offset, length):
        if offset < 0 or length < 0:
            raise PreflightError('negative read offset or length')
        if self.read_bytes + length > self.max_bytes:
            raise PreflightError(
                f'read-only preflight budget exceeded ({self.read_bytes + length} > {self.max_bytes})'
            )
        data = self._reader(offset, length)
        if len(data) != length:
            raise PreflightError(f'short read: expected {length}, got {len(data)}')
        self.read_bytes += length
        return data

    def write(self, *args, **kwargs):
        raise RuntimeError('preflight is strictly read-only; writes are forbidden')


def validate_offline_inputs(payload, apple, root_manifest=None):
    """Validate offline payload, Apple originals, and optional root manifest."""
    entries, requires = root_header.load(payload.resolve(), apple.resolve())
    selected = [e for e in entries if e[0] == DIAG_PATH]
    if len(selected) != 1 or selected[0][2] != 0o755 or selected[0][4] is not None:
        raise PreflightError('diagnostic must be a unique new 0755 root payload file')
    diag = {
        'path': selected[0][0],
        'size': len(selected[0][1]),
        'mode': selected[0][2],
        'sha256': selected[0][3].hex(),
    }
    cand_manifest = payload.resolve().parent / 'manifest.json'
    if cand_manifest.is_file():
        meta = json.loads(cand_manifest.read_text())
        if meta.get('built_complete') is not True:
            raise PreflightError('candidate manifest.json does not record built_complete=true')
    req_list = [
        {'path': p, 'size': sz, 'sha256': digest.hex()}
        for p, sz, digest in requires
    ]
    manifest_checked = False
    if root_manifest is not None:
        tools_dir = ROOT / 'x9/tools'
        if str(tools_dir) not in sys.path:
            sys.path.insert(0, str(tools_dir))
        import ufs_root_manifest
        try:
            ufs_root_manifest.validate(Path(root_manifest).resolve(), native=True)
        except Exception as exc:
            raise PreflightError(f'root manifest validation failed: {exc}') from exc
        manifest_checked = True
    return {
        'diag': diag,
        'requires': req_list,
        'manifest_checked': manifest_checked,
    }


def _require_dir_nofollow(hfs, path):
    entry = hfs.lookup(path, follow_symlinks=False)
    if entry is None or entry.get('type') != 'dir':
        raise PreflightError(f'required directory missing or not a directory: {path}')
    if stat.S_ISLNK(entry.get('mode', 0)):
        raise PreflightError(f'required directory is a symlink: {path}')


def _match_regular_file(hfs, path, expected_size, expected_sha256, expected_mode=None, check_owner=False):
    entry = hfs.lookup(path, follow_symlinks=False)
    if entry is None:
        return 'absent'
    mode = entry.get('mode', 0)
    if entry.get('type') != 'file' or not stat.S_ISREG(mode) or stat.S_ISLNK(mode):
        raise PreflightError(f'unexpected non-regular catalog entry: {path}')
    if expected_mode is not None and stat.S_IMODE(mode) != expected_mode:
        raise PreflightError(
            f'mode mismatch on {path}: got 0o{stat.S_IMODE(mode):o}, expected 0o{expected_mode:o}'
        )
    if check_owner and (entry.get('uid'), entry.get('gid')) != (0, 0):
        raise PreflightError(
            f'owner mismatch on {path}: got {entry.get("uid")}:{entry.get("gid")}, expected 0:0'
        )
    if entry.get('size') != expected_size:
        raise PreflightError(
            f'size mismatch on {path}: got {entry.get("size")}, expected {expected_size}'
        )
    data = hfs.read_file(path, max_bytes=max(expected_size, 1))
    actual_sha = hashlib.sha256(data).hexdigest()
    if actual_sha != expected_sha256:
        raise PreflightError(f'sha256 mismatch on {path}')
    return 'matched'


def audit_hfs_target(hfs, offline_spec, mode):
    """Run read-only HFSX checks for verify, install, or rollback."""
    if mode not in ('verify', 'install', 'rollback'):
        raise PreflightError(f'unsupported mode: {mode}')
    hfs.require_clean_journal()
    _require_dir_nofollow(hfs, '/usr')
    _require_dir_nofollow(hfs, '/usr/lib')
    temp_entry = hfs.lookup(TEMP_PATH, follow_symlinks=False)
    if temp_entry is not None:
        raise PreflightError(f'pre-existing temp path blocks safe operation: {TEMP_PATH}')

    diag = offline_spec['diag']
    diag_state = _match_regular_file(
        hfs,
        diag['path'],
        diag['size'],
        diag['sha256'],
        expected_mode=diag['mode'],
        check_owner=True,
    )

    if mode in ('install', 'verify'):
        for req in offline_spec['requires']:
            req_state = _match_regular_file(hfs, req['path'], req['size'], req['sha256'])
            if req_state != 'matched':
                raise PreflightError(f'required Apple binary missing: {req["path"]}')

    if mode == 'install':
        status = 'install_ready' if diag_state == 'absent' else 'already_installed'
    elif mode == 'rollback':
        status = 'rollback_ready' if diag_state == 'matched' else 'already_absent'
    else:
        status = 'installed_will_inject' if diag_state == 'matched' else 'absent_will_skip'

    return {
        'mode': mode,
        'diag_state': diag_state,
        'status': status,
        'temp_absent': True,
        'requires_checked': mode in ('install', 'verify'),
    }


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--payload', type=Path, required=True)
    p.add_argument('--apple-originals', type=Path, required=True)
    p.add_argument('--root-manifest', type=Path, default=None)
    p.add_argument('--mode', choices=('verify', 'install', 'rollback'), default='install')
    args = p.parse_args(argv)
    try:
        spec = validate_offline_inputs(args.payload, args.apple_originals, args.root_manifest)
    except (OSError, ValueError, KeyError, PreflightError) as exc:
        sys.exit(f'preflight_settings_diag failed: {exc}')
    summary = {
        'offline_ok': True,
        'mode': args.mode,
        'diag_path': spec['diag']['path'],
        'diag_size': spec['diag']['size'],
        'diag_sha256': spec['diag']['sha256'],
        'requires': [r['path'] for r in spec['requires']],
        'manifest_checked': spec['manifest_checked'],
        'device_probed': False,
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
