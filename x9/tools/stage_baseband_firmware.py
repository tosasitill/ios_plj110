#!/usr/bin/env python3
"""Provision one separate, 34 MiB, pinned+immutable Mav17 firmware file.

Requires explicit --create --yes after user approval. Never reuses an existing
file in create mode. --resume requires this operation's captured private state.
Raw writes target only that file's verified extents; root/plugin/log are read-only.
No flash, reboot, mount, unpin, immutable removal, or normal payload-file writes.
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
BLOCK = 4096
ALLOCATION = 34 * 1024 * 1024
NAME = 'baseband-Mav17-20H392.pack'
DEVICE = '/dev/block/sdc100'
MEMBERS = {'Info.plist', 'Options.plist', 'acdb.mbn', 'apps.mbn', 'bbcfg.mbn',
           'mba.mbn', 'qdsp6sw.mbn', 'restoresbl1.mbn', 'rpm.mbn', 'sbl1.mbn', 'tz.mbn'}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def now():
    return datetime.datetime.now().astimezone().isoformat()


def uncovered(start, count, covers):
    """Return the portions not covered by partition-relative reservations."""
    left = [(start, start + count)]
    for e in covers:
        p, end = e['partition_block'], e['partition_block'] + e['blocks']
        next_left = []
        for a, b in left:
            if b <= p or end <= a:
                next_left.append((a, b))
            else:
                if a < p:
                    next_left.append((a, p))
                if end < b:
                    next_left.append((end, b))
        left = next_left
        if not left:
            break
    return left


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
    ap.add_argument('--pack', type=Path, required=True)
    ap.add_argument('--metadata', type=Path, required=True)
    ap.add_argument('--state-dir', type=Path, required=True)
    ap.add_argument('--serial')
    ap.add_argument('--retired-reservation-manifest', type=Path, action='append', default=[],
                    help='historical root manifest whose source path is proven absent; preserves its exclusion union')
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument('--create', action='store_true')
    mode.add_argument('--resume', action='store_true')
    ap.add_argument('--yes', action='store_true', help='confirm the already-approved file-only operation')
    a = ap.parse_args()
    if not a.yes:
        raise ValueError('explicit user approval and --yes are required')
    os.umask(0o077)
    state_dir = a.state_dir.resolve()
    if not state_dir.is_relative_to((ROOT / '.local').resolve()):
        raise ValueError('private staging state must be inside the gitignored .local directory')
    m = load(a.root_manifest, native=True)
    if m.get('runtime') != 'native-ufs-root' or not m.get('write_policy') or m.get('firmware_policy'):
        raise ValueError('requires the existing native mutable-root manifest without a firmware policy')
    pack = a.pack.read_bytes()
    meta = json.loads(a.metadata.read_text())
    if (meta.get('build') != '20H392' or meta.get('device_class') != 'd22ap' or
        meta.get('storage') != 'external' or meta.get('complete') is not True or
        meta.get('excluded') or meta.get('allocation_bytes') != ALLOCATION or
        meta.get('pack_bytes') != len(pack) or meta.get('pack_sha256') != digest(pack) or
        pack[:8] != b'X9BBF001' or {f['name'] for f in meta['files']} != MEMBERS or
        len(meta['files']) != len(MEMBERS)):
        raise ValueError('requires the exact complete external 20H392 Mav17 package within the approved 34 MiB allocation')
    if not 16 <= len(pack) <= ALLOCATION:
        raise ValueError('payload exceeds the approved allocation')
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
    root_identity = digest(a.root_manifest.read_bytes())
    binding = {'root_manifest_sha256': root_identity, 'pack_sha256': digest(pack),
               'device_file': path, 'allocation_bytes': ALLOCATION, 'adb_serial': serial}
    state_file = state_dir / 'provisioning.json'
    if a.create:
        state_dir.mkdir(parents=True, exist_ok=False)
        state = {'binding': binding, 'started': now(), 'phase': 'preflight'}
        save(state_file, state)
    else:
        state = json.loads(state_file.read_text())
        if state['binding'] != binding or 'firmware' not in state:
            raise ValueError('resume state does not match this root/package/device/file')
        if state.get('phase') == 'complete':
            raise ValueError('already complete; run check_plaintext_root.py instead of restaging')
    audit = (state_dir / 'operations.jsonl').open('a')

    def record(value):
        audit.write(json.dumps({'time': now(), **value}) + '\n')
        audit.flush()

    def adb(command, data=None, timeout=120):
        p = subprocess.run(['adb', '-s', serial] +
                           (['exec-out'] if data is None else ['shell', '-T']) +
                           ['su', '-c', command], input=data, capture_output=True, timeout=timeout)
        record({'command': command, 'input_bytes': 0 if data is None else len(data),
                'returncode': p.returncode, 'stdout_bytes': len(p.stdout)})
        if p.returncode:
            # Do not turn raw-read stdout or authentication fields into console logs.
            raise RuntimeError('adb operation failed: ' + p.stderr[-800:].decode(errors='replace'))
        return p.stdout

    def preflight(manifest, name):
        p = subprocess.run([sys.executable, '-I', str(ROOT / 'x9/tools/check_plaintext_root.py'),
                            '--manifest', str(manifest), '--serial', serial],
                           capture_output=True, timeout=180)
        (state_dir / name).write_bytes(p.stdout + p.stderr)
        record({'preflight': name, 'returncode': p.returncode})
        if p.returncode:
            raise RuntimeError('root/firmware preflight failed; see private ' + name)
        print(p.stdout.decode().strip(), flush=True)

    def capture(file):
        status = adb('stat -c "%i %s %f" ' + q(file) + ' && lsattr ' + q(file) +
                     ' && ' + q(tool) + ' status ' + q(file)).decode().splitlines()
        inode, size, mode_bits = status[0].split()
        if (int(mode_bits, 16) & 0o170000) != 0o100000:
            raise ValueError('not a regular pinned file')
        if 'i' not in status[1].split()[0] or ': pinned (GC move failures 0)' not in status[2]:
            raise ValueError('pin/immutable protection missing or GC move failures present')
        table = json.loads(adb(q(tool) + ' extents ' + q(file) + ' --json'))
        return {'device_file': file, 'inode': int(inode), 'bytes': int(size),
                'android_extents': table,
                'extents': extents(table, m['partition_blocks'])['extents']}

    def root_header():
        header = adb(q(tool) + ' rawreadat ' + q(m['device_file']) + ' ' + DEVICE + ' 0 65536 2>/dev/null')
        if len(header) != 65536:
            raise ValueError('short root header read')
        return digest(header)

    try:
        preflight(a.root_manifest, 'preflight-before.log')
        root_before = root_header()
        plugins = json.loads((ROOT / 'x9/mailbox/mailbox-manifest.json').read_text())
        original_files = [m['device_file']] + [f['file'] for f in plugins['files']]
        if m.get('log_policy'):
            original_files.append(m['log_policy']['device_file'])
        before = {file: capture(file) for file in dict.fromkeys(original_files)}
        for f in plugins['files']:
            live = before[f['file']]
            if live['inode'] != f['inode'] or live['bytes'] != f['bytes'] or live['android_extents'] != f['android_extents']:
                raise ValueError('existing plugin mailbox identity/map changed')
        save(state_dir / 'existing-files-before.json', before)
        if a.create:
            # O_EXCL in pinfile is the final authority, even if this test races.
            adb('test ! -e ' + q(path) + ' && test ! -L ' + q(path))
            adb(q(tool) + ' create ' + q(path) + ' 34')
            adb('chattr +i ' + q(path))
            fw = capture(path)
            if fw['bytes'] != ALLOCATION or len(fw['extents']) > 256:
                raise ValueError('new allocation size/extent count mismatch')
            fw.update({'version': 1, 'kind': 'dedicated-readonly-bbfw', 'pinned': True,
                       'immutable': True, 'initial_content_verified': False,
                       'payload_bytes': len(pack), 'sha256': digest(pack)})
            # Persist the identity even if an overlap check refuses the payload.
            state.update({'firmware': fw, 'phase': 'allocated-unvalidated', 'root_header_before': root_before})
            save(state_file, state)
        else:
            fw = state['firmware']
        identity = {key: fw[key] for key in ('device_file', 'inode', 'bytes', 'android_extents', 'extents')}
        if capture(path) != identity:
            raise ValueError('captured allocation identity/map changed')
        # Always repeat ownership/overlap checks in resume mode as well.
        reserved = m['extents'] + m.get('log_policy', {}).get('extents', [])
        reserved += [e for f in before.values() for e in f['extents']]
        for e in fw['extents']:
            for other in reserved:
                if (e['partition_block'] < other['partition_block'] + other['blocks'] and
                    other['partition_block'] < e['partition_block'] + e['blocks']):
                    raise ValueError('new file overlaps a live root/plugin/log file')
        retired, retired_sources = [], []
        for source in a.retired_reservation_manifest:
            old = load(source, native=True)
            if any(old[k] != m[k] for k in ('device_serial', 'lun', 'partition_first', 'partition_blocks', 'partition_guid_hex')):
                raise ValueError('retired reservation source belongs to another device/partition')
            old_path = old['device_file']
            adb('test ! -e ' + q(old_path) + ' && test ! -L ' + q(old_path))
            retired += old['extents']
            retired_sources.append({'manifest': str(source.resolve()), 'sha256': digest(source.read_bytes()),
                                    'device_file': old_path, 'original_inode': old['inode'], 'live_path_absent': True})
        protected = m['write_policy']['protected_extents']
        collisions = []
        for index, other in enumerate(protected):
            for e in fw['extents']:
                begin = max(e['partition_block'], other['partition_block'])
                end = min(e['partition_block'] + e['blocks'], other['partition_block'] + other['blocks'])
                if begin < end:
                    if uncovered(begin, end - begin, retired):
                        raise ValueError('new file overlaps a reservation without a proven absent historical owner')
                    collisions.append({'protected_index': index, 'old': other,
                                       'reclassified': {'partition_block': begin, 'blocks': end - begin}})
        # Only reclassify the overlap: all old excluded bytes remain excluded,
        # via residual protected ranges OR the new firmware reservation in ADT.
        protected_after = [{'partition_block': p, 'blocks': end - p}
                           for e in protected
                           for p, end in uncovered(e['partition_block'], e['blocks'], fw['extents'])]
        if any(uncovered(e['partition_block'], e['blocks'], protected_after + fw['extents']) for e in protected):
            raise ValueError('reservation reclassification would lose an existing exclusion')
        save(state_dir / 'reservation-reclassification.json',
             {'sources': retired_sources, 'collisions': collisions,
              'old_exclusion_union_preserved': True, 'protected_after': protected_after})
        state['phase'] = 'allocated'
        save(state_file, state)

        def write_at(offset, data):
            if offset % BLOCK or not data or len(data) % BLOCK or len(data) > 256 * 1024 or offset + len(data) > ALLOCATION:
                raise ValueError('raw write exceeds this file-only operation')
            if capture(path) != identity:
                raise ValueError('new file identity/protection/map changed before write')
            old = adb(q(tool) + ' rawreadat ' + q(path) + ' ' + DEVICE +
                      f' {offset} {len(data)} 2>/dev/null')
            if len(old) != len(data):
                raise ValueError('short old-range read; refusing write')
            if old != data:
                reply = adb(q(tool) + ' rawwriteat ' + q(path) + ' ' + DEVICE +
                            f' {offset} {digest(old)} --yes', data).decode()
                if f'wrote {len(data)} bytes' not in reply or 'sha256 read back: ' + digest(data) not in reply:
                    raise ValueError('raw write/readback mismatch')
            record({'range_offset': offset, 'range_bytes': len(data),
                    'old_sha256': digest(old), 'new_sha256': digest(data), 'written': old != data})

        packet = pack + bytes((-len(pack)) % BLOCK)
        write_at(0, bytes(BLOCK))
        state['phase'] = 'staging'
        save(state_file, state)
        print('Separate BBFW header invalidated; staging only the new file.', flush=True)
        for offset in range(BLOCK, len(packet), 256 * 1024):
            write_at(offset, packet[offset:offset + 256 * 1024])
            if (offset - BLOCK) % (8 * 1024 * 1024) == 0:
                print('BBFW staged', min(offset + 256 * 1024, len(packet)), '/', len(packet), 'bytes', flush=True)
        write_at(0, packet[:BLOCK])
        report = adb(q(tool) + ' rawhash ' + q(path) + ' ' + DEVICE + ' ' + str(len(pack))).decode()
        (state_dir / 'payload-rawhash.txt').write_text(report)
        if not any(line.startswith(digest(pack) + '  ') and line.endswith('first ' + str(len(pack)) + ' bytes') for line in report.splitlines()):
            raise ValueError('final raw payload SHA256 mismatch')
        after = {file: capture(file) for file in before}
        save(state_dir / 'existing-files-after.json', after)
        if after != before or root_header() != root_before or capture(path) != identity:
            raise ValueError('existing identity/map, root header, or new file changed during staging')
        for old in retired_sources:
            adb('test ! -e ' + q(old['device_file']) + ' && test ! -L ' + q(old['device_file']))
        fw['initial_content_verified'] = True
        result = copy.deepcopy(m)
        result['write_policy']['protected_extents'] = protected_after
        result['firmware_policy'] = fw
        validate(result, native=True)
        manifest = state_dir / 'root-manifest.json'
        if manifest.exists() and json.loads(manifest.read_text()) != result:
            raise ValueError('refusing to replace a different output manifest')
        save(manifest, result)
        preflight(manifest, 'preflight-after.log')
        state.update({'phase': 'complete', 'completed': now(), 'root_header_after': root_before,
                      'published_manifest': str(manifest), 'padded_bytes_written': len(packet)})
        save(state_file, state)
        print('BBFW committed:', len(pack), 'payload bytes in a separate 34 MiB file; full raw hash verified.', flush=True)
        print('Existing root/plugin/log identities and maps, and the root header, are unchanged.', flush=True)
        print('Private manifest:', manifest.relative_to(ROOT), flush=True)
    finally:
        audit.close()


if __name__ == '__main__':
    main()
