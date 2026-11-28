#!/usr/bin/env python3
"""Plan native Bluetooth root installation OFFLINE unless explicitly approved.

Apply uses the existing root contracts, pinfile compare-write, and macOS native
HFS filesystem code (build_full_ios_root.py), NOT a new HFS catalog writer:
live root||extension -> private snapshot -> APFS clone -> native HFS install ->
bounded block diff -> compare/fsync/readback -> full root hash verification.
The Android files stay pinned+immutable; no GPT, partition, mailbox, firmware,
boot, reboot, or normal Android payload-file writes are performed.

An interrupted multi-block update is NOT power-fail atomic. Keep Android running
until success or rollback. Backups and a durable write-intent ledger are retained
under x9/out; --rollback is explicit recovery, never an automatic retry/install.
"""
import argparse
from collections import deque
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shlex
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid as uuidlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / 'x9/tools'))
sys.path.insert(0, str(HERE))
from ufs_root_manifest import validate, device as root_device
from ufs_manifest import extents
from userdata_device import resolve_userdata
from package import BINARIES, JOBS, STOCK_SHA, UUID
from launch_cache import CACHE_PATH, CACHE_LIMIT, validate_pair

BLOCK = 4096
WRITE = 64 * BLOCK                         # pinfile's existing hard limit
CONFIRM = 'install-native-bluetooth-root'
MAX_DIFF = 128 * 1024 * 1024              # includes native HFS journal/metadata
SPACE_MARGIN = 16 * 1024 * 1024           # receipts, input copies and directory metadata
SYSTEM_VERSION = '/System/Library/CoreServices/SystemVersion.plist'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def now():
    return datetime.datetime.now().astimezone().isoformat()


def sync_directory(path):
    directory = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def sync_file(path):
    with path.open('rb') as stream:
        os.fsync(stream.fileno())


def save(path, value):
    fd, name = tempfile.mkstemp(prefix=path.name + '.', suffix='.tmp', dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(value, stream, indent=2)
            stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, path)
        sync_directory(path.parent)
    finally:
        if temporary.exists():
            temporary.unlink()


def repo_path(path):
    path = (ROOT / path) if not path.is_absolute() else path
    resolved = path.resolve(strict=True)
    if not (resolved.is_relative_to(ROOT) or
            resolved.is_relative_to((ROOT / '.local').resolve())):
        raise ValueError('inputs must be inside the repository or fixed shared .local')
    return resolved


def bounded(path, maximum):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= maximum:
            raise ValueError('expected bounded regular input: ' + path.name)
        with os.fdopen(fd, 'rb', closefd=False) as stream:
            data = stream.read(maximum + 1)
        if len(data) != info.st_size:
            raise ValueError('input changed during read')
        return data
    finally:
        os.close(fd)


def member(base, source):
    if not isinstance(source, str) or not source or source.startswith('/'):
        raise ValueError('invalid package-relative source')
    path = base
    for part in Path(source).parts:
        if part in ('.', '..'):
            raise ValueError('package source traversal')
        path = path / part
        if path.is_symlink():
            raise ValueError('package source contains a symlink')
    path.relative_to(base)
    return path


def bluetoothd_identity(data):
    if sha(data) != STOCK_SHA or len(data) < 32:
        raise ValueError('bluetoothd does not match the exact locked original SHA-256')
    magic, cpu, subtype, kind, count, size, flags, reserved = struct.unpack_from('<8I', data)
    if magic != 0xfeedfacf or cpu != 0x100000c or not 0 < count <= 4096 or size > len(data) - 32:
        raise ValueError('unexpected locked arm64 Mach-O header')
    found, position = [], 32
    for unused in range(count):
        if position + 8 > 32 + size:
            raise ValueError('truncated Mach-O load commands')
        command, length = struct.unpack_from('<II', data, position)
        if length < 8 or length % 4 or position + length > 32 + size:
            raise ValueError('invalid Mach-O load command bounds')
        if command == 0x1b:
            if length != 24:
                raise ValueError('unexpected Mach-O LC_UUID size')
            found.append(str(uuidlib.UUID(bytes=data[position + 8:position + 24])).upper())
        position += length
    if position != 32 + size or found != [UUID]:
        raise ValueError('bluetoothd UUID does not match the exact 20H392 bridge identity')


def inputs(a):
    package = repo_path(a.payload)
    metadata = bounded(package / 'root-payload.json', 65536)
    payload = json.loads(metadata)
    if (payload.get('schema') != 1 or
            payload.get('kind') != 'x9-bluetooth-native-root-payload' or
            payload.get('ios_build') != '20H392' or
            payload.get('bluetoothd_uuid') != UUID or
            payload.get('bluetoothd_sha256') != STOCK_SHA or
            payload.get('candidate_ready') is not True or
            payload.get('installed') is not False or
            payload.get('power_interface') != 'kern.x9_bt_power' or
            payload.get('power_lifecycle') != 'closed-transport-only'):
        raise ValueError('requires the complete version-locked offline root payload')
    job_paths = ['/System/Library/LaunchDaemons/' + job[0] + '.plist' for job in JOBS]
    expected = list(BINARIES.values()) + job_paths + [CACHE_PATH]
    entries = payload.get('files')
    originals = payload.get('original_jobs')
    cache_backup = payload.get('original_launchd_cache')
    if (not isinstance(entries, list) or len(entries) != 9 or
            [entry.get('path') for entry in entries] != expected or
            payload.get('install_order') != expected or
            not isinstance(originals, list) or len(originals) != 2 or
            [entry.get('path') for entry in originals] != job_paths or
            not isinstance(cache_backup, dict) or cache_backup.get('path') != CACHE_PATH):
        raise ValueError('only six executables, two Program-only jobs and their append-only service cache are permitted')
    files, stock_jobs = {}, {}
    for index, entry in enumerate(entries):
        data = bounded(member(package, entry['source']), 64 * 1024 * 1024)
        mode = '0755' if index < 6 else '0644'
        if (entry.get('size') != len(data) or entry.get('sha256') != sha(data) or
                entry.get('mode') != mode or entry.get('uid') != 0 or entry.get('gid') != 0):
            raise ValueError('payload bytes/ownership/mode differ from the manifest')
        if index < 6 and entry.get('precondition') != {'absent': True}:
            raise ValueError('new executable must have an absent-target precondition')
        files[entry['path']] = data
    for entry, job in zip(originals, JOBS):
        data = bounded(member(package, entry['source']), 65536)
        if entry.get('size') != len(data) or entry.get('sha256') != sha(data):
            raise ValueError('original job differs from its exact byte hash')
        stock = plistlib.loads(data)
        label, program, args, service, replacement = job
        candidate = next(item for item in entries if item['path'] == entry['path'])
        if (stock.get('Label') != label or stock.get('UserName') != 'mobile' or
                stock.get('ProgramArguments') != [program, *args] or
                stock.get('Program', program) != program or
                service not in stock.get('MachServices', {}) or
                plistlib.loads(files[entry['path']]) != dict(stock, Program=replacement) or
                candidate.get('precondition') != {'sha256': sha(data)}):
            raise ValueError('stock identity or Program-only replacement mismatch')
        stock_jobs[entry['path']] = data
    cache_original = bounded(member(package, cache_backup['source']), CACHE_LIMIT)
    if (cache_backup.get('size') != len(cache_original) or
            cache_backup.get('sha256') != sha(cache_original) or
            entries[-1].get('precondition') != {'sha256': sha(cache_original)}):
        raise ValueError('original launchd cache differs from the exact backup/precondition')
    cache_contract = validate_pair(cache_original, files[CACHE_PATH], JOBS)
    if payload.get('launchd_cache') != cache_contract:
        raise ValueError('launchd cache geometry/append contract differs from the payload')
    stock_jobs[CACHE_PATH] = cache_original
    root_manifest = repo_path(a.root_manifest)
    manifest_bytes = bounded(root_manifest, 8 * 1024 * 1024)
    root = validate(json.loads(manifest_bytes), native=True)
    if root.get('runtime') != 'native-ufs-root' or not root.get('write_policy'):
        raise ValueError('requires the dedicated native mutable-root contract')
    apple_directory = repo_path(a.apple_originals)
    apple = {('/usr/sbin/' + name): bounded(apple_directory / name, 64 * 1024 * 1024)
             for name in ('bluetoothd', 'BlueTool')}
    bluetoothd_identity(apple['/usr/sbin/bluetoothd'])
    candidate_path = repo_path(a.candidate)
    candidate_bytes = bounded(candidate_path / 'manifest.json', 65536)
    candidate = json.loads(candidate_bytes)
    if (candidate.get('schema') != 1 or
            candidate.get('kind') != 'x9-bluetooth-native-complete-candidate' or
            candidate.get('ios_build') != '20H392' or
            candidate.get('candidate_ready') is not True or
            candidate.get('built_complete') is not True or
            candidate.get('runtime_verified') is not False or
            candidate.get('root_installed') is not False):
        raise ValueError('requires a built, not-yet-installed complete native candidate')
    paths = candidate.get('paths', {})
    if (not isinstance(paths, dict) or paths.get('root_payload') != 'root-payload/root-payload.json' or
            (candidate_path / paths['root_payload']).resolve() != package / 'root-payload.json' or
            candidate.get('power_interface') != 'kern.x9_bt_power' or
            candidate.get('power_lifecycle') != 'closed-transport-only' or
            candidate.get('launchd_cache') != cache_contract or
            candidate.get('component_exit_status') != {'full_overlay_kernel': 0, 'native_userland_jobs': 0,
                                                       'root_payload': 0, 'boot_image': 0}):
        raise ValueError('root payload is not the one bound to this complete candidate')
    binding = {'root_manifest_sha256': sha(manifest_bytes),
               'payload_manifest_sha256': sha(metadata),
               'candidate_manifest_sha256': sha(candidate_bytes),
               'apple_originals': {path: sha(data) for path, data in apple.items()},
               'payload_files': {path: sha(data) for path, data in files.items()}}
    return root_manifest, root, payload, files, stock_jobs, apple, binding


def image_path(mount, logical, leaf_symlink=False):
    """Resolve iOS absolute symlinks relative to THIS image, never macOS /."""
    if not logical.startswith('/') or '\0' in logical:
        raise ValueError('expected absolute iOS path')
    todo, parts, links = deque(logical.split('/')[1:]), [], 0
    while todo:
        part = todo.popleft()
        if part in ('', '.'):
            continue
        if part == '..':
            if not parts:
                raise ValueError('image symlink escapes root')
            parts.pop(); continue
        path = mount.joinpath(*parts, part)
        try:
            info = path.lstat()
        except FileNotFoundError:
            if todo:
                raise ValueError('target parent directory is missing')
            parts.append(part); break
        if stat.S_ISLNK(info.st_mode):
            if not todo and not leaf_symlink:
                raise ValueError('target leaf is a symlink')
            links += 1
            if links > 40:
                raise ValueError('image symlink loop')
            target = os.readlink(path)
            if target.startswith('/'):
                parts = []
            todo.extendleft(reversed(target.split('/')))
        else:
            if todo and not stat.S_ISDIR(info.st_mode):
                raise ValueError('target ancestor is not a directory')
            parts.append(part)
    path = mount.joinpath(*parts)
    if not path.is_relative_to(mount) or path == mount:
        raise ValueError('invalid resolved image path')
    return path


class Session:
    def __init__(self, serial, manifest, manifest_path, directory, manifest_hash):
        self.serial, self.m, self.manifest_path, self.directory = serial, manifest, manifest_path, directory
        self.manifest_hash = manifest_hash
        self.adb_binary = shutil.which('adb')
        if not self.adb_binary:
            raise ValueError('adb is not installed')
        self.total, unused = root_device(manifest)
        self.parts = [(0, manifest)]
        if manifest.get('extension_policy'):
            self.parts.append((manifest['bytes'], manifest['extension_policy']))
        self.route = None

    def record(self, value):
        with (self.directory / 'operations.jsonl').open('a') as stream:
            stream.write(json.dumps({'time': now(), **value}) + '\n')
            stream.flush(); os.fsync(stream.fileno())

    def adb(self, command, data=None, timeout=120):
        args = [self.adb_binary, '-s', self.serial]
        args += ['exec-out'] if data is None else ['shell', '-T']
        # Do not start rollback while an interrupt leaves our writer in flight.
        # Defer SIGINT/SIGTERM across the bounded single compare-write; on USB
        # failure/timeout, rollback additionally waits for pinfile to quiesce.
        mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM}) if data is not None else None
        try:
            process = subprocess.run(args + ['su', '-c', command], input=data,
                                     capture_output=True, timeout=timeout)
        finally:
            if mask is not None:
                signal.pthread_sigmask(signal.SIG_SETMASK, mask)
        # No dm table, raw bytes, authentication fields, or serial in console/audit.
        self.record({'operation': 'adb', 'returncode': process.returncode,
                     'input_bytes': 0 if data is None else len(data),
                     'output_bytes': len(process.stdout)})
        if process.returncode:
            raise RuntimeError('adb operation failed; no write is automatically retried')
        return process.stdout

    def quiesce(self):
        absent = 0
        for unused in range(30):
            if self.text('pidof pinfile 2>/dev/null || true').strip():
                absent = 0
            else:
                absent += 1
                if absent == 2:
                    return
            time.sleep(1)
        raise ValueError('pinfile is still in flight; no competing rollback write is allowed')

    def text(self, command, timeout=120):
        return self.adb(command, timeout=timeout).decode()

    def preflight(self, name):
        if sha(bounded(self.manifest_path, 8 * 1024 * 1024)) != self.manifest_hash:
            raise ValueError('frozen private root manifest bytes changed')
        process = subprocess.run([sys.executable, '-I', str(ROOT / 'x9/tools/check_plaintext_root.py'),
                                  '--manifest', str(self.manifest_path), '--serial', self.serial],
                                 capture_output=True, timeout=300)
        (self.directory / name).write_bytes(process.stdout + process.stderr)
        self.record({'operation': 'root-preflight', 'log': name, 'returncode': process.returncode})
        if process.returncode:
            raise ValueError('check_plaintext_root.py refused the live root; see private preflight log')
        if sha(bounded(self.manifest_path, 8 * 1024 * 1024)) != self.manifest_hash:
            raise ValueError('private root manifest changed during preflight')
        if not re.fullmatch(r'\d+', self.text('getprop ro.build.version.sdk').strip()):
            raise ValueError('requires Android running; not a live iOS-mounted root')
        route = resolve_userdata(self.text, self.m)
        if self.route is not None and route != self.route:
            raise ValueError('live userdata route changed')
        self.route = route

    def capture(self, part):
        q = shlex.quote
        path, tool = part['device_file'], str(Path(part['device_file']).parent / 'pinfile')
        info = self.text('stat -c "%i %s %f" ' + q(path)).split()
        if (len(info) != 3 or list(map(int, info[:2])) != [part['inode'], part['bytes']] or
                int(info[2], 16) & 0o170000 != 0o100000):
            raise ValueError('dedicated root file identity changed')
        if ('i' not in self.text('lsattr ' + q(path)).split()[0] or
                ': pinned (GC move failures 0)' not in self.text(q(tool) + ' status ' + q(path))):
            raise ValueError('root file pin/immutable protection changed')
        mapping = json.loads(self.text(q(tool) + ' extents ' + q(path) + ' --json'))
        if (mapping != part['android_extents'] or
                extents(mapping, self.m['partition_blocks'])['extents'] != part['extents']):
            raise ValueError('root file FIEMAP differs from the approved manifest')
        if resolve_userdata(self.text, self.m) != self.route:
            raise ValueError('userdata route changed before raw operation')

    def read(self, part, offset, length):
        if offset < 0 or offset % BLOCK or not length or length % BLOCK or offset + length > part['bytes']:
            raise ValueError('range outside the dedicated root file')
        q = shlex.quote
        tool = str(Path(part['device_file']).parent / 'pinfile')
        result = self.adb(q(tool) + ' rawreadat ' + q(part['device_file']) + ' ' + q(self.route) +
                          f' {offset} {length} 2>/dev/null')
        if len(result) != length:
            raise ValueError('short raw root read')
        return result

    def verify_hashes(self, hashes):
        q = shlex.quote
        for index, (base, part) in enumerate(self.parts):
            self.capture(part)
            tool = str(Path(part['device_file']).parent / 'pinfile')
            report = self.text(q(tool) + ' rawhash ' + q(part['device_file']) + ' ' + q(self.route),
                               timeout=max(300, part['bytes'] // (16 * 1024 * 1024) + 60))
            if not any(line.startswith(hashes[index] + '  ') for line in report.splitlines()):
                raise ValueError('live entire-root baseline/readback hash mismatch')
        self.record({'operation': 'whole-root-verified', 'sha256_by_part': hashes})

    def snapshot(self, output):
        hashes = []
        with output.open('xb') as stream:
            for base, part in self.parts:
                self.capture(part)
                q = shlex.quote
                tool = str(Path(part['device_file']).parent / 'pinfile')
                command = q(tool) + ' rawread ' + q(part['device_file']) + ' ' + q(self.route)
                stderr = self.directory / f'snapshot-part-{len(hashes)}.log'
                with stderr.open('xb') as errors:
                    process = subprocess.Popen([self.adb_binary, '-s', self.serial, 'exec-out', 'su', '-c', command],
                                               stdout=subprocess.PIPE, stderr=errors)
                    timer = threading.Timer(3600, process.kill)
                    timer.start()
                    try:
                        count, digest = 0, hashlib.sha256()
                        while True:
                            data = process.stdout.read(1024 * 1024)
                            if not data:
                                break
                            count += len(data)
                            if count > part['bytes']:
                                raise ValueError('snapshot exceeds dedicated root file')
                            digest.update(data)
                            if data == bytes(len(data)):
                                stream.seek(len(data), 1)  # sparse private backup
                            else:
                                stream.write(data)
                        if process.wait() or count != part['bytes']:
                            raise ValueError('failed or short live root snapshot')
                        hashes.append(digest.hexdigest())
                    finally:
                        timer.cancel()
                        if process.poll() is None:
                            process.kill(); process.wait()
                        process.stdout.close()
            stream.truncate(self.total); stream.flush(); os.fsync(stream.fileno())
        sync_directory(output.parent)
        self.verify_hashes(hashes)
        return hashes

    def write(self, change, expected, data):
        part = self.parts[change['part']][1]
        offset = change['offset']
        if (not data or len(data) > WRITE or len(data) % BLOCK or offset % BLOCK or
                offset < 0 or offset + len(data) > part['bytes']):
            raise ValueError('write exceeds the manifest root file or pinfile limit')
        self.capture(part)
        q = shlex.quote
        tool = str(Path(part['device_file']).parent / 'pinfile')
        reply = self.adb(q(tool) + ' rawwriteat ' + q(part['device_file']) + ' ' + q(self.route) +
                         f' {offset} {expected} --yes', data).decode()
        if f'wrote {len(data)} bytes' not in reply or 'sha256 read back: ' + sha(data) not in reply:
            raise ValueError('pinfile write/fsync/readback did not verify')
        self.record({'operation': 'compare-write', 'part': change['part'], 'offset': offset,
                     'bytes': len(data), 'old_sha256': expected, 'new_sha256': sha(data)})


# Small isolated LOCAL helper, only for files inside a validated offline mount.
# cp -p preserves original job metadata/xattrs; fsync+rename publishes executables
# before jobs. It never runs on Android or modifies an Apple executable.
LOCAL_REPLACE = r'''
import os, stat, sys
root, dest, source, mode = sys.argv[1:]
root = os.path.realpath(root)
if not os.path.realpath(os.path.dirname(dest)).startswith(root + '/'):
    raise ValueError('offline image destination escaped mount')
temp = dest + '.x9-bluetooth-stage'
if os.path.lexists(temp):
    raise ValueError('temporary target already exists')
try:
    import subprocess
    if os.path.lexists(dest):
        if not stat.S_ISREG(os.lstat(dest).st_mode):
            raise ValueError('existing target not regular')
        subprocess.run(['/bin/cp', '-p', dest, temp], check=True)
        fd = os.open(temp, os.O_WRONLY | os.O_TRUNC | os.O_NOFOLLOW)
    else:
        fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as output, open(source, 'rb') as payload:
        while True:
            data = payload.read(1048576)
            if not data: break
            output.write(data)
        output.flush()
        os.fchmod(output.fileno(), int(mode, 8)); os.fchown(output.fileno(), 0, 0)
        os.fsync(output.fileno())
    os.replace(temp, dest)
    fd = os.open(os.path.dirname(dest), os.O_RDONLY)
    try: os.fsync(fd)
    finally: os.close(fd)
finally:
    if os.path.lexists(temp): os.unlink(temp)
'''


def prepare_image(session, directory, files, jobs, apple):
    snapshot, updated = directory / 'snapshot.hfs', directory / 'updated.hfs'
    subprocess.run(['/bin/cp', '-c', str(snapshot), str(updated)], check=True)
    sync_file(updated)
    sync_directory(directory)
    mount = directory / 'mount'; mount.mkdir()
    mounted_device = None
    try:
        # Exactly the repo's native HFS attach/detach route; never mount live UFS.
        attached = subprocess.run(['hdiutil', 'attach', '-owners', 'on', '-nobrowse',
                                   '-mountpoint', str(mount), str(updated), '-plist'],
                                  capture_output=True, check=True)
        entities = plistlib.loads(attached.stdout)['system-entities']
        # Even if the expected mount binding is absent, finally must detach this
        # image's returned root device, not leave a writable stale mount behind.
        returned = [e['dev-entry'] for e in entities if e.get('dev-entry')]
        if returned:
            mounted_device = returned[0]
        devices = [e['dev-entry'] for e in entities if e.get('mount-point') and
                   Path(e['mount-point']).resolve() == mount.resolve()]
        if len(devices) != 1:
            raise ValueError('could not bind the private mounted HFS image')
        mounted_device = devices[0]
        version_bytes = bounded(image_path(mount, SYSTEM_VERSION, True), 65536)
        version = plistlib.loads(version_bytes)
        if not isinstance(version, dict) or version.get('ProductBuildVersion') != '20H392':
            raise ValueError('live root SystemVersion is not 20H392; no root mutation allowed')
        bluetoothd_identity(bounded(image_path(mount, '/usr/sbin/bluetoothd', True), 64 * 1024 * 1024))
        for logical, original in {**apple, **jobs}.items():
            target = image_path(mount, logical, leaf_symlink=logical in apple)
            if bounded(target, 64 * 1024 * 1024) != original:
                raise ValueError('live Apple original/job differs: ' + logical)
        for logical in BINARIES.values():
            target = image_path(mount, logical)
            if target.exists() or target.is_symlink():
                raise ValueError('new native runtime target already exists: ' + logical)
        backups = directory / 'originals'; backups.mkdir()
        for logical, original in {**apple, **jobs, SYSTEM_VERSION: version_bytes}.items():
            target = backups / logical.lstrip('/')
            target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(original)
            sync_file(target)
        for parent in sorted((path for path in backups.rglob('*') if path.is_dir()),
                             key=lambda path: len(path.parts), reverse=True):
            sync_directory(parent)
        sync_directory(backups)
        sync_directory(directory)
        session.record({'operation': 'all-originals-and-absent-targets-verified',
                        'ios_build': '20H392', 'bluetoothd_uuid': UUID,
                        'sha256': {path: sha(data) for path, data in
                                   {**apple, **jobs, SYSTEM_VERSION: version_bytes}.items()}})
        prepared = directory / 'payload'; prepared.mkdir()
        for index, (logical, data) in enumerate(files.items()):
            target = image_path(mount, logical)
            source = prepared / str(index); source.write_bytes(data)
            command = [sys.executable, '-I', '-c', LOCAL_REPLACE, str(mount), str(target), str(source),
                       '0755' if logical in BINARIES.values() else '0644']
            if os.geteuid() != 0:
                command = ['/usr/bin/sudo', '-n', *command]
            subprocess.run(command, check=True, capture_output=True)
            info = target.lstat()
            if (bounded(target, 64 * 1024 * 1024) != data or info.st_uid != 0 or info.st_gid != 0 or
                    stat.S_IMODE(info.st_mode) != (0o755 if logical in BINARIES.values() else 0o644)):
                raise ValueError('offline filesystem file/metadata installation failed')
        for logical, original in {**apple, SYSTEM_VERSION: version_bytes}.items():
            if bounded(image_path(mount, logical, True), 64 * 1024 * 1024) != original:
                raise ValueError('Apple executable bytes changed during offline install')
    finally:
        if mounted_device is not None:
            subprocess.run(['hdiutil', 'detach', mounted_device], check=True, capture_output=True)
    sync_file(updated)
    sync_directory(directory)
    # All changes, including journal replay, are bounded; no catalog mutation by us.
    changes, hashes, changed = [], [], 0
    blocks = directory / 'blocks'; blocks.mkdir()
    with snapshot.open('rb') as old, updated.open('rb') as new:
        for index, (base, part) in enumerate(session.parts):
            digest = hashlib.sha256()
            for offset in range(0, part['bytes'], WRITE):
                length = min(WRITE, part['bytes'] - offset)
                before, after = old.read(length), new.read(length)
                if len(before) != length or len(after) != length:
                    raise ValueError('short offline root image')
                digest.update(after)
                if before == after:
                    continue
                changed += length
                if changed > MAX_DIFF:
                    raise ValueError('native HFS delta exceeds 128 MiB; refusing any live write')
                ident = f'{len(changes):06d}'
                (blocks / (ident + '.old')).write_bytes(before)
                (blocks / (ident + '.new')).write_bytes(after)
                changes.append({'id': ident, 'part': index, 'offset': offset, 'bytes': length,
                                'old_sha256': sha(before), 'new_sha256': sha(after)})
            hashes.append(digest.hexdigest())
    if not changes:
        raise ValueError('unexpected empty native root installation delta')
    for path in blocks.iterdir():
        sync_file(path)
    # File fsync alone does not make newly created .old/.new directory entries
    # durable. Persist this entire dependency chain BEFORE publishing a ledger
    # which can authorize a live write and later reference these backups.
    sync_directory(blocks)
    sync_directory(directory)
    sync_directory(directory.parent)
    session.record({'operation': 'offline-native-hfs-ready', 'changed_bytes': changed,
                    'changed_ranges': len(changes)})
    return changes, hashes


def backup_bytes(directory, change, suffix):
    if not re.fullmatch(r'\d{6}', change['id']) or suffix not in ('old', 'new'):
        raise ValueError('invalid block backup identity')
    data = bounded(directory / 'blocks' / (change['id'] + '.' + suffix), WRITE)
    if len(data) != change['bytes'] or sha(data) != change[suffix + '_sha256']:
        raise ValueError('durable block backup/payload changed; no live write')
    return data


def validate_ledger(session, state):
    changes, attempted = state.get('changes'), state.get('attempted')
    if (not isinstance(changes, list) or not 1 <= len(changes) <= MAX_DIFF // BLOCK or
            not isinstance(attempted, list) or
            any(type(index) is not int or not 0 <= index < len(changes) for index in attempted) or
            attempted != list(range(len(attempted)))):
        raise ValueError('invalid durable write-intent ledger')
    seen, total = set(), 0
    for index, change in enumerate(changes):
        part, offset, length = (change.get(key) for key in ('part', 'offset', 'bytes'))
        if (change.get('id') != f'{index:06d}' or type(part) is not int or
                not 0 <= part < len(session.parts) or type(offset) is not int or
                type(length) is not int or offset < 0 or offset % WRITE or
                not 0 < length <= WRITE or length % BLOCK or
                offset + length > session.parts[part][1]['bytes'] or
                (part, offset) in seen or
                any(not isinstance(change.get(key), str) or
                    not re.fullmatch(r'[0-9a-f]{64}', change[key])
                    for key in ('old_sha256', 'new_sha256'))):
            raise ValueError('write ledger escapes its dedicated root file or contains overlapping ranges')
        seen.add((part, offset)); total += length
        backup_bytes(session.directory, change, 'old')
        backup_bytes(session.directory, change, 'new')
    if total > MAX_DIFF or any(not isinstance(state.get(key), list) or
            len(state[key]) != len(session.parts) or
            any(not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{64}', value)
                for value in state[key]) for key in ('before_sha256', 'after_sha256')):
        raise ValueError('invalid root hash binding or excessive recovery delta')


def rollback(session, state, directory):
    validate_ledger(session, state)
    state['phase'] = 'rolling-back'; save(directory / 'state.json', state)
    failures = []
    try:
        session.quiesce()
    except Exception as error:
        state.update(phase='rollback-incomplete', rollback_failures=[{'quiescence': str(error)}],
                     root_installed=False, runtime_verified=False, audio_verified=False)
        save(directory / 'state.json', state)
        raise RuntimeError('writer not proven quiescent; retain backups and retry explicit recovery') from error
    # Include the in-flight intent: rawwriteat might have written some blocks
    # before a USB disconnect/readback error. Only our exact old/new block states
    # may be restored; never blindly overwrite an unknown concurrent/torn write.
    for index in reversed(state['attempted']):
        change = state['changes'][index]
        try:
            old = backup_bytes(directory, change, 'old')
            new = backup_bytes(directory, change, 'new')
            part = session.parts[change['part']][1]
            session.capture(part)
            current = session.read(part, change['offset'], change['bytes'])
            if current == old:
                continue
            if any(current[k:k + BLOCK] not in (old[k:k + BLOCK], new[k:k + BLOCK])
                   for k in range(0, len(old), BLOCK)):
                raise ValueError('unknown/torn/concurrent block; automatic overwrite refused')
            session.write(change, sha(current), old)
        except Exception as error:
            failures.append({'change': index, 'error': str(error)})
    if not failures:
        try:
            session.verify_hashes(state['before_sha256'])
            session.preflight('preflight-rollback.log')
        except Exception as error:
            failures.append({'verification': str(error)})
    state.update(phase='rollback-incomplete' if failures else 'rolled-back',
                 rollback_failures=failures, root_installed=False, runtime_verified=False, audio_verified=False)
    save(directory / 'state.json', state)
    if failures:
        raise RuntimeError('rollback incomplete; keep Android running and retain private backups/state')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--payload', type=Path, required=True)
    ap.add_argument('--candidate', type=Path, required=True,
                    help='complete candidate directory (its manifest binds this payload)')
    ap.add_argument('--root-manifest', type=Path, required=True)
    ap.add_argument('--apple-originals', type=Path, required=True,
                    help='private 20H392 original directory containing bluetoothd AND BlueTool')
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--apply', action='store_true')
    mode.add_argument('--rollback', action='store_true')
    ap.add_argument('--serial', help='explicit adb transport; never auto-select in write mode')
    ap.add_argument('--state-dir', type=Path, help='NEW private x9/out directory (existing only for rollback)')
    ap.add_argument('--confirm-root-modification', choices=[CONFIRM])
    ap.add_argument('--confirm-android-stays-running', action='store_true')
    a = ap.parse_args()
    os.umask(0o077)
    root_path, manifest, payload, files, jobs, apple, binding = inputs(a)
    if not (a.apply or a.rollback):
        print(json.dumps({'kind': 'x9-bluetooth-root-install-plan', 'device_contacted': False,
                          'writes_performed': False, 'root_bytes': root_device(manifest)[0],
                          'install_order': payload['install_order'], 'binding': binding,
                          'method': 'private live snapshot / native HFS / pinfile compare-write',
                          'maximum_delta_bytes': MAX_DIFF,
                          'minimum_local_free_bytes': root_device(manifest)[0] + 3 * MAX_DIFF + SPACE_MARGIN,
                          'required_confirmation': CONFIRM,
                          'constraints': ['explicit user approval is required before --apply',
                                          'keep Android running until success or verified rollback',
                                          'local sudo -n/root is required for offline file ownership',
                                          'power/USB loss may require explicit --rollback recovery',
                                          'unknown or torn blocks are never blindly overwritten',
                                          'does not boot the device or validate Bluetooth runtime']}, indent=2))
        return
    if (a.confirm_root_modification != CONFIRM or not a.confirm_android_stays_running or
            not a.serial or not a.state_dir):
        raise ValueError('write/recovery requires user approval, BOTH confirmations, serial and private state-dir')
    if sys.platform != 'darwin':
        raise ValueError('this execution route requires macOS native HFS')
    directory = a.state_dir.resolve()
    if not directory.is_relative_to((ROOT / 'x9/out').resolve()) or directory == (ROOT / 'x9/out').resolve():
        raise ValueError('private backups/state must stay under this worktree x9/out')
    if a.state_dir.is_symlink():
        raise ValueError('state directory may not be a symlink')
    (ROOT / 'x9/out').mkdir(parents=True, exist_ok=True)
    # Local-only lock; no device file is allocated or altered for locking.
    root_identity = sha(json.dumps([manifest['device_serial'], manifest['partition_guid_hex'],
                                   manifest['volume_uuid_hex']], separators=(',', ':')).encode())
    lock_path = ROOT / 'x9/out' / ('.bt-root-lock-' + root_identity)
    lock = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if a.apply:
            directory.mkdir(parents=True, exist_ok=False)
            sync_directory(directory.parent)
            if shutil.disk_usage(directory).free < root_device(manifest)[0] + 3 * MAX_DIFF + SPACE_MARGIN:
                raise ValueError('insufficient local space for root backup, APFS delta, both block copies and margin')
            state = {'schema': 1, 'kind': 'x9-bluetooth-root-install-state', 'binding': binding,
                     'adb_serial': a.serial, 'started': now(), 'phase': 'preflight',
                     'attempted': [], 'root_installed': False, 'runtime_verified': False, 'audio_verified': False}
            save(directory / 'state.json', state)
            frozen = bounded(root_path, 8 * 1024 * 1024)
            if sha(frozen) != binding['root_manifest_sha256']:
                raise ValueError('root manifest changed before creating the private frozen preflight input')
            with (directory / 'root-manifest.json').open('xb') as stream:
                stream.write(frozen); stream.flush(); os.fsync(stream.fileno())
            sync_directory(directory)
        else:
            state = json.loads(bounded(directory / 'state.json', 16 * 1024 * 1024))
            if (state.get('binding') != binding or state.get('adb_serial') != a.serial or
                    state.get('schema') != 1 or
                    state.get('kind') != 'x9-bluetooth-root-install-state' or
                    not state.get('changes') or not state.get('attempted')):
                raise ValueError('rollback requires this exact payload/root/device and durable write ledger')
        session = Session(a.serial, manifest, directory / 'root-manifest.json', directory,
                          binding['root_manifest_sha256'])
        session.preflight('preflight-before.log' if a.apply else 'preflight-recovery.log')
        if a.rollback:
            rollback(session, state, directory)
            print('Original entire root restored and verified; backups retained:', directory)
            return
        try:
            state['before_sha256'] = session.snapshot(directory / 'snapshot.hfs')
            state['phase'] = 'snapshot-verified'; save(directory / 'state.json', state)
            changes, after_hashes = prepare_image(session, directory, files, jobs, apple)
            state.update(changes=changes, after_sha256=after_hashes, phase='prepared')
            save(directory / 'state.json', state)
            validate_ledger(session, state)
            # Recheck ALL content, identities, protections and routing after offline
            # mounting and before the first device mutation, not just changed blocks.
            session.preflight('preflight-before-write.log')
            session.verify_hashes(state['before_sha256'])
            state['phase'] = 'staging'; save(directory / 'state.json', state)
            for index, change in enumerate(changes):
                backup_bytes(directory, change, 'old')
                data = backup_bytes(directory, change, 'new')
                state['attempted'].append(index)
                save(directory / 'state.json', state)  # write-ahead intent, before rawwriteat
                session.write(change, change['old_sha256'], data)
            session.verify_hashes(after_hashes)
            session.preflight('preflight-after.log')
            state.update(phase='complete', completed=now(), root_installed=True,
                         runtime_verified=False, audio_verified=False, power_lifecycle='closed-transport-only')
            save(directory / 'state.json', state)
        except BaseException as error:
            state['failure'] = str(error)
            if state.get('attempted'):
                rollback(session, state, directory)
            else:
                state['phase'] = 'failed-without-device-write'; save(directory / 'state.json', state)
            raise
        print('Native Bluetooth root installed; whole-root raw readback verified.')
        print('Private originals, full baseline and receipt retained:', directory)
        print('Device not booted. iOS initialization/scan/pair/BLE/audio remain UNVERIFIED.')
    finally:
        os.close(lock)


if __name__ == '__main__':
    # A normal SIGTERM must unwind into rollback, not silently leave live root
    # halfway staged. SIGKILL/power loss still require the retained recovery ledger.
    def terminate(signum, frame):
        raise KeyboardInterrupt('root staging interrupted; recovery ledger retained')
    signal.signal(signal.SIGTERM, terminate)
    main()
