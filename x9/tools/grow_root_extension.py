#!/usr/bin/env python3
"""Provision one separate pinned+immutable file that extends the native HFS root.

User approval 2026-11-10: "再扩 16g". Requires --create --yes. Allocates the
file with the device's pinfile (F2FS pin + preallocation), sets immutable, and
records its live extents as the manifest's extension_policy. NO raw writes:
the new blocks keep their old contents (HFS marks them free when the kernel
grows the volume with x9_rootgrow=1). Existing root/plugin/log/firmware files
are only read. Before anything, the first and last 64 KiB of the root image
(HFS volume header and alternate header) are saved privately.
No flash, reboot, mount, unpin, immutable removal or partition writes.
"""
import argparse
import copy
import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ufs_manifest import extents
from ufs_root_manifest import load, validate

ROOT = Path(__file__).resolve().parents[2]
DEVICE = '/dev/block/sdc100'
NAME = 'iosroot-extension-1.hfs'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def now():
    return datetime.datetime.now().astimezone().isoformat()


def save(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    with temporary.open('w') as f:
        os.chmod(temporary, 0o600)
        json.dump(value, f, indent=2)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    temporary.replace(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root-manifest', type=Path, required=True)
    ap.add_argument('--state-dir', type=Path, required=True)
    ap.add_argument('--size-mib', type=int, default=16384)
    ap.add_argument('--serial')
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument('--create', action='store_true')
    mode.add_argument('--resume', action='store_true', help='continue after allocation with retired reservations')
    ap.add_argument('--retired-reservation-manifest', type=Path, action='append', default=[],
                    help='historical manifest whose file is proven absent; its extents may be reclassified')
    ap.add_argument('--yes', action='store_true', help='confirm the already-approved allocation')
    a = ap.parse_args()
    if not a.yes:
        raise ValueError('explicit user approval and --yes are required')
    if not 1024 <= a.size_mib <= 32768:
        raise ValueError('extension size outside 1..32 GiB')
    os.umask(0o077)
    state_dir = a.state_dir.resolve()
    if not state_dir.is_relative_to((ROOT / '.local').resolve()):
        raise ValueError('private state must be inside the gitignored .local directory')
    m = load(a.root_manifest, native=True)
    if m.get('runtime') != 'native-ufs-root' or not m.get('write_policy') or m.get('extension_policy'):
        raise ValueError('requires the native mutable-root manifest without an extension')
    size = a.size_mib * 1024 * 1024
    serial = a.serial
    if not serial:
        devices = subprocess.run(['adb', 'devices'], check=True, capture_output=True, text=True)
        candidates = [line.split()[0] for line in devices.stdout.splitlines()[1:]
                      if len(line.split()) >= 2 and line.split()[1] == 'device']
        if len(candidates) != 1:
            raise ValueError('select one adb device with --serial')
        serial = candidates[0]
    path = str(Path(m['device_file']).parent / NAME)
    tool = str(Path(m['device_file']).parent / 'pinfile')
    q = shlex.quote
    state_file = state_dir / 'provisioning.json'
    binding = {'root_manifest_sha256': digest(a.root_manifest.read_bytes()), 'device_file': path,
               'bytes': size, 'adb_serial': serial}
    if a.create:
        state_dir.mkdir(parents=True, exist_ok=False)
        state = {'binding': binding, 'started': now(), 'phase': 'preflight'}
        save(state_file, state)
    else:
        state = json.loads(state_file.read_text())
        if state['binding'] != binding or 'extension' not in state or state.get('phase') == 'complete':
            raise ValueError('resume state does not match this root/file or is already complete')
    audit = (state_dir / 'operations.jsonl').open('a')

    def record(value):
        audit.write(json.dumps({'time': now(), **value}) + '\n')
        audit.flush()

    def adb(command, timeout=120):
        p = subprocess.run(['adb', '-s', serial, 'exec-out', 'su', '-c', command],
                           capture_output=True, timeout=timeout)
        record({'command': command, 'returncode': p.returncode, 'stdout_bytes': len(p.stdout)})
        if p.returncode:
            raise RuntimeError('adb operation failed: ' + p.stderr[-800:].decode(errors='replace'))
        return p.stdout

    def preflight(manifest, name):
        p = subprocess.run([sys.executable, '-I', str(ROOT / 'x9/tools/check_plaintext_root.py'),
                            '--manifest', str(manifest), '--serial', serial],
                           capture_output=True, timeout=600)
        (state_dir / name).write_bytes(p.stdout + p.stderr)
        record({'preflight': name, 'returncode': p.returncode})
        if p.returncode:
            raise RuntimeError('preflight failed; see private ' + name)
        print(p.stdout.decode().strip(), flush=True)

    def capture(file):
        status = adb('stat -c "%i %s %f" ' + q(file) + ' && lsattr ' + q(file) +
                     ' && ' + q(tool) + ' status ' + q(file)).decode().splitlines()
        inode, nbytes, mode_bits = status[0].split()
        if (int(mode_bits, 16) & 0o170000) != 0o100000:
            raise ValueError('not a regular pinned file')
        if 'i' not in status[1].split()[0] or ': pinned (GC move failures 0)' not in status[2]:
            raise ValueError('pin/immutable protection missing or GC move failures present')
        table = json.loads(adb(q(tool) + ' extents ' + q(file) + ' --json'))
        return {'device_file': file, 'inode': int(inode), 'bytes': int(nbytes),
                'android_extents': table, 'extents': extents(table, m['partition_blocks'])['extents']}

    def root_range(offset):
        data = adb(q(tool) + ' rawreadat ' + q(m['device_file']) + ' ' + DEVICE + f' {offset} 65536 2>/dev/null')
        if len(data) != 65536:
            raise ValueError('short root metadata read')
        return data

    def overlap(a0, n0, a1, n1):
        lo, hi = max(a0, a1), min(a0 + n0, a1 + n1)
        return (lo, hi - lo) if lo < hi else None

    def uncovered(start, count, covers):
        left = [(start, start + count)]
        for e in covers:
            p, end = e['partition_block'], e['partition_block'] + e['blocks']
            left = [seg for x, y in left for seg in
                    ([(x, y)] if y <= p or end <= x else [(x, min(y, p))] * (x < p) + [(max(x, end), y)] * (end < y))]
            if not left:
                break
        return left

    if a.resume:
        ext = state['extension']
        if capture(path) != {k: ext[k] for k in ('device_file', 'inode', 'bytes', 'android_extents', 'extents')}:
            raise ValueError('allocated extension identity/map changed since allocation')
        before = json.loads((state_dir / 'existing-files-before.json').read_text())
        head = (state_dir / 'root-first-64k.bin').read_bytes()
        tail = (state_dir / 'root-last-64k.bin').read_bytes()
        retired, sources = [], []
        for source in a.retired_reservation_manifest:
            old = json.loads(source.read_text())
            if any(old.get(k) != m[k] for k in ('device_serial', 'lun', 'partition_first', 'partition_blocks', 'partition_guid_hex')
                   if k in old) or old.get('device_file') in [m['device_file'], path]:
                raise ValueError('retired source belongs to another device/partition or a live file')
            adb('test ! -e ' + q(old['device_file']) + ' && test ! -L ' + q(old['device_file']))
            # pin-test result.json keeps its map under validated_extent_contract.
            runs = old.get('extents') or old.get('validated_extent_contract', {}).get('extents')
            if not runs or not all({'partition_block', 'blocks'} <= set(r) for r in runs):
                raise ValueError('retired source has no partition extent map')
            retired += runs
            sources.append({'manifest': str(source.resolve()), 'sha256': digest(source.read_bytes()),
                            'device_file': old['device_file'], 'live_path_absent': True})
        protected = m['write_policy']['protected_extents']
        collisions = []
        for index, other in enumerate(protected):
            for e in ext['extents']:
                o = overlap(e['partition_block'], e['blocks'], other['partition_block'], other['blocks'])
                if o:
                    if uncovered(o[0], o[1], retired):
                        raise ValueError('extension overlaps a reservation without a proven absent historical owner')
                    collisions.append({'protected_index': index, 'reclassified': {'partition_block': o[0], 'blocks': o[1]}})
        protected_after = [{'partition_block': x, 'blocks': y - x} for e in protected
                           for x, y in uncovered(e['partition_block'], e['blocks'], ext['extents'])]
        if any(uncovered(e['partition_block'], e['blocks'], protected_after + ext['extents']) for e in protected):
            raise ValueError('reclassification would lose an existing exclusion')
        save(state_dir / 'reservation-reclassification.json',
             {'sources': sources, 'collisions': collisions, 'reclassified_blocks': sum(c['reclassified']['blocks'] for c in collisions),
              'old_exclusion_union_preserved': True, 'protected_after_count': len(protected_after)})
        result = copy.deepcopy(m)
        result['write_policy']['protected_extents'] = protected_after
        ext.update({'version': 1, 'kind': 'dedicated-root-extension', 'pinned': True, 'immutable': True})
        result['extension_policy'] = ext
        validate(result, native=True)
        after = {file: capture(file) for file in before}
        save(state_dir / 'existing-files-after.json', after)
        if after != before or root_range(0) != head or root_range(m['bytes'] - 65536) != tail:
            raise ValueError('existing identity/map or root HFS headers changed')
        manifest = state_dir / 'root-manifest.json'
        save(manifest, result)
        preflight(manifest, 'preflight-after.log')
        state.update({'phase': 'complete', 'completed': now(), 'published_manifest': str(manifest)})
        save(state_file, state)
        print(f'Root extension committed: {size} bytes in {len(ext["extents"])} extents; '
              f'{sum(c["reclassified"]["blocks"] for c in collisions)} retired-reservation blocks reclassified; no raw writes.', flush=True)
        print('Private manifest:', manifest, flush=True)
        audit.close()
        return

    try:
        preflight(a.root_manifest, 'preflight-before.log')
        head, tail = root_range(0), root_range(m['bytes'] - 65536)
        (state_dir / 'root-first-64k.bin').write_bytes(head)
        (state_dir / 'root-last-64k.bin').write_bytes(tail)
        if head[1024:1026] not in (b'H+', b'HX') or head[1128:1136].hex() != m['volume_uuid_hex']:
            raise ValueError('root HFS header/UUID mismatch')
        state.update({'root_first_64k_sha256': digest(head), 'root_last_64k_sha256': digest(tail)})
        plugins = json.loads((ROOT / 'x9/mailbox/mailbox-manifest.json').read_text())
        files = [m['device_file']] + [f['file'] for f in plugins['files']]
        for key in ('log_policy', 'firmware_policy'):
            if m.get(key):
                files.append(m[key]['device_file'])
        before = {file: capture(file) for file in dict.fromkeys(files)}
        save(state_dir / 'existing-files-before.json', before)
        print('Existing files captured; root HFS headers saved privately.', flush=True)
        # O_EXCL in pinfile is the final authority.
        adb('test ! -e ' + q(path) + ' && test ! -L ' + q(path))
        state['phase'] = 'allocating'
        save(state_file, state)
        print(f'Allocating {a.size_mib} MiB pinned file (may take a few minutes)...', flush=True)
        adb(q(tool) + ' create ' + q(path) + ' ' + str(a.size_mib), timeout=3600)
        adb('chattr +i ' + q(path))
        ext = capture(path)
        state.update({'extension': ext, 'phase': 'allocated-unvalidated'})
        save(state_file, state)
        if ext['bytes'] != size:
            raise ValueError('new allocation size mismatch')
        ext.update({'version': 1, 'kind': 'dedicated-root-extension', 'pinned': True, 'immutable': True})
        result = copy.deepcopy(m)
        result['extension_policy'] = ext
        validate(result, native=True)   # overlap with root/protected/log/firmware, counts, size
        reserved = [e for f in before.values() for e in f['extents']]
        for e in ext['extents']:
            for other in reserved:
                if (e['partition_block'] < other['partition_block'] + other['blocks'] and
                        other['partition_block'] < e['partition_block'] + e['blocks']):
                    raise ValueError('new file overlaps a live root/plugin/log/firmware file')
        after = {file: capture(file) for file in before}
        save(state_dir / 'existing-files-after.json', after)
        if after != before or root_range(0) != head or root_range(m['bytes'] - 65536) != tail:
            raise ValueError('existing identity/map or root HFS headers changed during allocation')
        manifest = state_dir / 'root-manifest.json'
        save(manifest, result)
        preflight(manifest, 'preflight-after.log')
        state.update({'phase': 'complete', 'completed': now(), 'published_manifest': str(manifest)})
        save(state_file, state)
        print(f'Root extension committed: {size} bytes in {len(ext["extents"])} extents; no raw writes.', flush=True)
        print('Private manifest:', manifest, flush=True)
    finally:
        audit.close()


if __name__ == '__main__':
    main()
