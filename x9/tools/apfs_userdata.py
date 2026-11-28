#!/usr/bin/env python3
"""APFS container on Android userdata: a dedicated pinned+immutable file.

User request 2026-11-28: "开始做 apfs 不用管数据但还是用 data-a-live 设计在
userdata 上 pin，不删我的安卓数据". iOS-side content is disposable; Android data
is never touched. The design follows the existing dedicated windows
(log_policy / firmware_policy / extension_policy):

  image      macOS host, no sudo: sparse raw file -> newfs_apfs -C -> diskutil
             apfs addVolume (System role; Data role with --data-role) ->
             feature allowlist check against iOS 16 apfs 2142 -> sidecar json.
  inspect    re-run the offline checks on an image.
  provision  device (adb root): `pinfile create` a NEW file next to the root
             image (O_EXCL; never reuses a path), chattr +i, capture its live
             extents, prove no overlap with root/plugin/log/firmware/extension
             and every protected run, then raw-write ONLY the image's non-zero
             256 KiB chunks plus the checkpoint-descriptor area into that
             file's own verified extents (block 0 invalidated first, written
             last). Emits a private root manifest with apfs_policy.
  rewrite    same file identity (from provisioning state), new image content.

Why sparse writes are sufficient: an APFS container is reached only through
block 0 and the checkpoint descriptor ring (both written explicitly here, with
zeros where the image is zero); every other block is reached only through
references from those, so stale bytes in unreferenced blocks are never read.

No flash, reboot, mount, unpin, immutable removal, Android file deletion, or
writes outside the new file's extents. Device steps require --yes.
"""
import argparse
import copy
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shlex
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

ROOT = Path(__file__).resolve().parents[2]
DEVICE = '/dev/block/sdc100'
NAME = 'iosapfs-container-1.img'
BLOCK = 4096
CHUNK = 256 * 1024
NEWFS = '/System/Library/Filesystems/apfs.fs/Contents/Resources/newfs_apfs'

ROLE_SYSTEM, ROLE_DATA = 0x0001, 0x0040
# Bits iOS 16.7 apfs (2142.140.9) is known to accept; anything else rejects.
NX_INCOMPAT_OK = 0x2          # NX_INCOMPAT_VERSION2
NX_FEATURES_OK = 0x0
VOL_FEATURES_OK = 0x1 | 0x2 | 0x4 | 0x8 | 0x10   # defrag-pre, hardlink map, defrag, strictatime, volgrp ino space
VOL_INCOMPAT_OK = 0x1 | 0x8   # case-insensitive, normalization-insensitive (no sealed/restore/pfk/prealloc)


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


# ---------------------------------------------------------------- offline

def parse_nx(block0):
    """Container superblock fields needed for the checks and sparse writes."""
    if len(block0) < BLOCK or block0[32:36] != b'NXSB':
        raise ValueError('block 0 is not an APFS container superblock')
    block_size, block_count, features, ro_compat, incompat = struct.unpack_from('<IQQQQ', block0, 36)
    desc_blocks, data_blocks, desc_base, data_base = struct.unpack_from('<IIQQ', block0, 104)
    if block_size != BLOCK:
        raise ValueError('APFS block size must be 4096')
    if desc_blocks & 0x80000000:
        raise ValueError('non-contiguous checkpoint descriptor area is not supported')
    return {'block_size': block_size, 'block_count': block_count, 'features': features,
            'ro_compat': ro_compat, 'incompat': incompat, 'xp_desc_base': desc_base,
            'xp_desc_blocks': desc_blocks, 'xp_data_base': data_base,
            'xp_data_blocks': data_blocks & 0x7fffffff}


def scan_volumes(read_block, block_count):
    """Latest APSB per fs_index by a linear scan (images are freshly made)."""
    volumes = {}
    for index in range(block_count):
        b = read_block(index)
        if b is None or b[32:36] != b'APSB':
            continue
        xid, = struct.unpack_from('<Q', b, 16)
        fs_index, = struct.unpack_from('<I', b, 36)
        features, ro_compat, incompat = struct.unpack_from('<QQQ', b, 40)
        role, = struct.unpack_from('<H', b, 964)
        name = b[704:960].split(b'\0')[0].decode(errors='replace')
        if fs_index not in volumes or volumes[fs_index]['xid'] < xid:
            volumes[fs_index] = {'xid': xid, 'features': features, 'ro_compat': ro_compat,
                                 'incompat': incompat, 'role': role, 'name': name}
    return [volumes[k] for k in sorted(volumes)]


def check_compat(nx, volumes, data_role):
    if nx['incompat'] & ~NX_INCOMPAT_OK or nx['features'] & ~NX_FEATURES_OK or nx['ro_compat']:
        raise ValueError('container features outside the iOS 16 allowlist')
    for v in volumes:
        if v['features'] & ~VOL_FEATURES_OK or v['incompat'] & ~VOL_INCOMPAT_OK or v['ro_compat']:
            raise ValueError(f'volume {v["name"]!r} features outside the iOS 16 allowlist')
        if v['incompat'] & 0x1:
            raise ValueError(f'volume {v["name"]!r} is case-insensitive; iOS volumes are case-sensitive')
    roles = sorted(v['role'] for v in volumes)
    expected = sorted([ROLE_SYSTEM] + ([ROLE_DATA] if data_role else []))
    if roles != expected:
        raise ValueError(f'volume roles {roles} != expected {expected}')


def chunk_plan(path, size):
    """(sha256 of the whole image, sorted chunk offsets to write)."""
    h = hashlib.sha256()
    chunks = set()
    zero = bytes(CHUNK)
    with open(path, 'rb') as f:
        for offset in range(0, size, CHUNK):
            data = f.read(min(CHUNK, size - offset))
            h.update(data)
            if data != zero[:len(data)]:
                chunks.add(offset)
        f.seek(0)
        nx = parse_nx(f.read(BLOCK))
    if nx['block_count'] * BLOCK != size:
        raise ValueError('container block count does not cover the image exactly')
    first = nx['xp_desc_base'] * BLOCK
    last = (nx['xp_desc_base'] + nx['xp_desc_blocks']) * BLOCK
    chunks.update(range(first - first % CHUNK, last, CHUNK))
    chunks.add(0)
    return h.hexdigest(), sorted(chunks), nx


def inspect(path, data_role):
    size = path.stat().st_size
    if size % CHUNK or not 64 << 20 <= size <= 64 << 30:
        raise ValueError('image size must be a 256 KiB multiple in 64 MiB..64 GiB')
    sha, chunks, nx = chunk_plan(path, size)
    with open(path, 'rb') as f:
        limit = min(nx['block_count'], (max(chunks) + CHUNK) // BLOCK)

        def read_block(i):
            f.seek(i * BLOCK)
            return f.read(BLOCK)
        volumes = scan_volumes(read_block, limit)
    check_compat(nx, volumes, data_role)
    return {'schema': 'x9-apfs-container-image-v1', 'bytes': size, 'sha256': sha,
            'data_role': data_role, 'container': nx, 'volumes': volumes,
            'write_chunks': chunks, 'write_bytes': len(chunks) * CHUNK}


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def build_image(out, size_mib, data_role):
    if out.exists():
        raise ValueError('refusing to overwrite an existing image')
    if not 64 <= size_mib <= 65536:
        raise ValueError('size outside 64 MiB..64 GiB')
    if data_role and size_mib < 1024:
        # nx_max_file_systems = container size / 512 MiB: two volumes need 1 GiB.
        raise ValueError('System + Data volumes need at least 1024 MiB')
    out.parent.mkdir(parents=True, exist_ok=True)
    run(['mkfile', '-n', f'{size_mib}m', str(out)])
    attach = run(['hdiutil', 'attach', '-nomount', '-imagekey', 'diskimage-class=CRawDiskImage', str(out)])
    whole = attach.split()[0]
    try:
        run([NEWFS, '-C', whole])
        listing = plistlib.loads(run(['diskutil', 'apfs', 'list', '-plist']).encode())
        stores = [c for c in listing.get('Containers', [])
                  if any(s.get('DeviceIdentifier') == whole.removeprefix('/dev/')
                         for s in c.get('PhysicalStores', []))]
        if len(stores) != 1:
            raise RuntimeError('could not identify the new container')
        container = stores[0]['ContainerReference']
        run(['diskutil', 'apfs', 'addVolume', container, 'Case-sensitive APFS', 'System', '-role', 'S', '-nomount'])
        if data_role:
            run(['diskutil', 'apfs', 'addVolume', container, 'Case-sensitive APFS', 'Data', '-role', 'D', '-nomount'])
    finally:
        subprocess.run(['hdiutil', 'detach', whole], capture_output=True)
    report = inspect(out, data_role)
    report.update({'built': now(), 'host': 'macOS ' + run(['sw_vers', '-productVersion']).strip()})
    save(out.with_suffix(out.suffix + '.json'), report)
    return report


# ---------------------------------------------------------------- device

class Device:
    def __init__(self, manifest, serial, state_dir):
        from ufs_manifest import extents
        self.extents = extents
        self.m, self.serial, self.state_dir = manifest, serial, state_dir
        self.q = shlex.quote
        self.tool = str(Path(manifest['device_file']).parent / 'pinfile')
        self.audit = (state_dir / 'operations.jsonl').open('a')

    def record(self, value):
        self.audit.write(json.dumps({'time': now(), **value}) + '\n')
        self.audit.flush()

    def adb(self, command, data=None, timeout=120):
        p = subprocess.run(['adb', '-s', self.serial] + (['exec-out'] if data is None else ['shell', '-T']) +
                           ['su', '-c', command], input=data, capture_output=True, timeout=timeout)
        self.record({'command': command, 'input_bytes': 0 if data is None else len(data),
                     'returncode': p.returncode, 'stdout_bytes': len(p.stdout)})
        if p.returncode:
            raise RuntimeError('adb operation failed: ' + p.stderr[-800:].decode(errors='replace'))
        return p.stdout

    def preflight(self, manifest, name):
        p = subprocess.run([sys.executable, '-I', str(ROOT / 'x9/tools/check_plaintext_root.py'),
                            '--manifest', str(manifest), '--serial', self.serial],
                           capture_output=True, timeout=600)
        (self.state_dir / name).write_bytes(p.stdout + p.stderr)
        self.record({'preflight': name, 'returncode': p.returncode})
        if p.returncode:
            raise RuntimeError('root preflight failed; see private ' + name)

    def capture(self, file):
        q = self.q
        status = self.adb('stat -c "%i %s %f" ' + q(file) + ' && lsattr ' + q(file) +
                          ' && ' + q(self.tool) + ' status ' + q(file)).decode().splitlines()
        inode, size, mode_bits = status[0].split()
        if (int(mode_bits, 16) & 0o170000) != 0o100000:
            raise ValueError('not a regular pinned file')
        if 'i' not in status[1].split()[0] or ': pinned (GC move failures 0)' not in status[2]:
            raise ValueError('pin/immutable protection missing or GC move failures present')
        table = json.loads(self.adb(q(self.tool) + ' extents ' + q(file) + ' --json'))
        return {'device_file': file, 'inode': int(inode), 'bytes': int(size), 'android_extents': table,
                'extents': self.extents(table, self.m['partition_blocks'])['extents']}

    def root_header(self):
        header = self.adb(self.q(self.tool) + ' rawreadat ' + self.q(self.m['device_file']) + ' ' + DEVICE +
                          ' 0 65536 2>/dev/null')
        if len(header) != 65536:
            raise ValueError('short root header read')
        return digest(header)

    def existing_files(self):
        plugins = json.loads((ROOT / 'x9/mailbox/mailbox-manifest.json').read_text())
        files = [self.m['device_file']] + [f['file'] for f in plugins['files']]
        for key in ('log_policy', 'firmware_policy', 'extension_policy'):
            if self.m.get(key):
                files.append(self.m[key]['device_file'])
        return list(dict.fromkeys(files))

    def free_kib(self, directory):
        lines = self.adb('df -k ' + self.q(directory)).decode().splitlines()
        return int(lines[-1].split()[3])

    def write_chunks(self, path, identity, image, chunks, size):
        q = self.q

        def write_at(offset, data):
            if offset % BLOCK or not data or len(data) % BLOCK or len(data) > CHUNK or offset + len(data) > size:
                raise ValueError('raw write exceeds this file-only operation')
            old = self.adb(q(self.tool) + ' rawreadat ' + q(path) + ' ' + DEVICE +
                           f' {offset} {len(data)} 2>/dev/null')
            if len(old) != len(data):
                raise ValueError('short old-range read; refusing write')
            if old != data:
                reply = self.adb(q(self.tool) + ' rawwriteat ' + q(path) + ' ' + DEVICE +
                                 f' {offset} {digest(old)} --yes', data).decode()
                if f'wrote {len(data)} bytes' not in reply or 'sha256 read back: ' + digest(data) not in reply:
                    raise ValueError('raw write/readback mismatch')
            self.record({'range_offset': offset, 'range_bytes': len(data), 'old_sha256': digest(old),
                         'new_sha256': digest(data), 'written': old != data})

        def chunk(offset):
            with open(image, 'rb') as f:
                f.seek(offset)
                return f.read(min(CHUNK, size - offset))

        if self.capture(path) != identity:
            raise ValueError('APFS file identity/protection/map changed before write')
        write_at(0, bytes(BLOCK))          # no valid container while staging
        for n, offset in enumerate(c for c in chunks if c):
            write_at(offset, chunk(offset))
            if n % 16 == 15:
                if self.capture(path) != identity:
                    raise ValueError('APFS file identity changed during staging')
                print(f'APFS staged {n + 1}/{len(chunks) - 1} chunks', flush=True)
        write_at(0, chunk(0))
        if self.capture(path) != identity:
            raise ValueError('APFS file identity changed during staging')


def adb_serial(serial):
    if serial:
        return serial
    out = subprocess.run(['adb', 'devices'], check=True, capture_output=True, text=True).stdout
    found = [l.split()[0] for l in out.splitlines()[1:] if len(l.split()) >= 2 and l.split()[1] == 'device']
    if len(found) != 1:
        raise ValueError('select one adb device with --serial')
    return found[0]


def provision(a, rewrite=False):
    from ufs_root_manifest import load, validate
    if not a.yes:
        raise ValueError('explicit user approval and --yes are required')
    os.umask(0o077)
    state_dir = a.state_dir.resolve()
    if not state_dir.is_relative_to((ROOT / '.local').resolve()):
        raise ValueError('private state must be inside the gitignored .local directory')
    report = json.loads(Path(str(a.image) + '.json').read_text())
    if inspect(a.image, report['data_role']) != {k: report[k] for k in report if k not in ('built', 'host')}:
        raise ValueError('image no longer matches its sidecar report')
    size, chunks = report['bytes'], report['write_chunks']
    state_file = state_dir / 'provisioning.json'
    if rewrite:
        state = json.loads(state_file.read_text())
        if state.get('phase') != 'complete':
            raise ValueError('rewrite needs a completed provisioning state')
        m = load(state['root_manifest'], native=True)
        if m.get('apfs_policy') or digest(Path(state['root_manifest']).read_bytes()) != state['binding']['root_manifest_sha256']:
            raise ValueError('rewrite base manifest changed')
        serial = adb_serial(a.serial or state['binding']['adb_serial'])
    else:
        m = load(a.root_manifest, native=True)
        if m.get('runtime') != 'native-ufs-root' or not m.get('write_policy') or m.get('apfs_policy'):
            raise ValueError('requires the native mutable-root manifest without an APFS window')
        serial = adb_serial(a.serial)
        state_dir.mkdir(parents=True, exist_ok=False)
        state = {'binding': {'root_manifest_sha256': digest(a.root_manifest.read_bytes()),
                             'adb_serial': serial, 'bytes': size},
                 'root_manifest': str(a.root_manifest.resolve()), 'started': now(), 'phase': 'preflight'}
        save(state_file, state)
    if size != state['binding']['bytes']:
        raise ValueError('image size differs from the allocated file')
    dev = Device(m, serial, state_dir)
    path = str(Path(m['device_file']).parent / NAME)
    q = dev.q
    try:
        dev.preflight(Path(state['root_manifest']), 'preflight-before.log')
        root_before = dev.root_header()
        before = {f: dev.capture(f) for f in dev.existing_files()}
        save(state_dir / ('existing-files-before' + ('-rewrite' if rewrite else '') + '.json'), before)
        if rewrite:
            identity = state['identity']
        else:
            directory = str(Path(path).parent)
            if dev.free_kib(directory) < size // 1024 + 4 * 1024 * 1024:
                raise ValueError('userdata needs the container size plus 4 GiB free')
            dev.adb('test ! -e ' + q(path) + ' && test ! -L ' + q(path))
            state['phase'] = 'allocating'
            save(state_file, state)
            print(f'Allocating {size >> 20} MiB pinned file...', flush=True)
            dev.adb(q(dev.tool) + ' create ' + q(path) + ' ' + str(size >> 20), timeout=3600)
            dev.adb('chattr +i ' + q(path))
            identity = dev.capture(path)
            state.update({'identity': identity, 'phase': 'allocated-unvalidated'})
            save(state_file, state)
            if identity['bytes'] != size:
                raise ValueError('new allocation size mismatch')
        policy = {'version': 1, 'kind': 'dedicated-apfs-container', 'pinned': True, 'immutable': True,
                  **identity, 'image_sha256': report['sha256'], 'data_role': report['data_role'],
                  'initial_content_verified': True}
        result = copy.deepcopy(m)
        result['apfs_policy'] = policy
        validate(result, native=True)   # overlap/bounds BEFORE any write
        reserved = [e for f in before.values() for e in f['extents']]
        for e in identity['extents']:
            for o in reserved:
                if e['partition_block'] < o['partition_block'] + o['blocks'] and o['partition_block'] < e['partition_block'] + e['blocks']:
                    raise ValueError('APFS file overlaps a live root/plugin/log/firmware file')
        state['phase'] = 'staging'
        save(state_file, state)
        dev.write_chunks(path, identity, a.image, chunks, size)
        after = {f: dev.capture(f) for f in before}
        if after != before or dev.root_header() != root_before:
            raise ValueError('existing identity/map or root header changed during staging')
        manifest = state_dir / ('root-manifest' + ('-rewrite' if rewrite else '') + '.json')
        save(manifest, result)
        dev.preflight(manifest, 'preflight-after.log')
        state.update({'phase': 'complete', 'completed': now(), 'published_manifest': str(manifest),
                      'image_sha256': report['sha256'], 'chunks_written': len(chunks)})
        save(state_file, state)
        print(f'APFS container staged: {size >> 20} MiB file, {len(identity["extents"])} extents, '
              f'{len(chunks)} chunks; Android files unchanged.', flush=True)
        print('Private manifest:', manifest, flush=True)
    finally:
        dev.audit.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('image')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--size-mib', type=int, default=8192)
    p.add_argument('--data-role', action='store_true', help='also create the Data-role volume (stage B)')
    p = sub.add_parser('inspect')
    p.add_argument('image', type=Path)
    p.add_argument('--data-role', action='store_true')
    for name in ('provision', 'rewrite'):
        p = sub.add_parser(name)
        if name == 'provision':
            p.add_argument('--root-manifest', type=Path, required=True)
        p.add_argument('--state-dir', type=Path, required=True)
        p.add_argument('--image', type=Path, required=True)
        p.add_argument('--serial')
        p.add_argument('--yes', action='store_true')
    a = ap.parse_args()
    if a.cmd == 'image':
        r = build_image(a.out, a.size_mib, a.data_role)
    elif a.cmd == 'inspect':
        r = inspect(a.image, a.data_role)
    else:
        provision(a, rewrite=a.cmd == 'rewrite')
        return
    print(json.dumps({k: v for k, v in r.items() if k != 'write_chunks'}, indent=1))
    print('write chunks:', len(r['write_chunks']), f'({r["write_bytes"] >> 10} KiB)')


if __name__ == '__main__':
    main()
