#!/usr/bin/env python3
"""Install the seven Bluetooth files without a whole-root read or backup.

A private macFUSE file delegates EVERY image read/write to a bounded cache.
Native HFS performs allocation, catalog and journal updates in LOCAL COW only.
After detach, only the seven catalog owners are fixed, all actual read
dependencies are revalidated, and the final changed blocks are compare-written.
There is no image snapshot, sparse whole-image scaffold, conversion or rawhash.
"""
import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from stage import (ROOT, BLOCK, WRITE, CONFIRM, MAX_DIFF, SYSTEM_VERSION,
                   Session, inputs, bounded, image_path, bluetoothd_identity,
                   sha, now, save, sync_file, sync_directory, backup_bytes)
from package import BINARIES

MAX_READSET = 128 * 1024 * 1024
MAX_DEVICE_READ = 512 * 1024 * 1024  # includes native compare/readback and explicit reads
MAX_RECOVERY_READ = 4 * MAX_DIFF + MAX_READSET + 2 * 65536
MAX_COW_LOG = 512 * 1024 * 1024
MAX_RPC = 1024 * 1024
MAGIC = 0x58423952
REQUEST = struct.Struct('<IHHQII')
RESPONSE = struct.Struct('<IIII')


class TargetSession(Session):
    """Preserve the manifest guards; forbid inherited whole-root operations."""
    def __init__(self, *args):
        super().__init__(*args)
        self.device_read_bytes = 0
        self.recovery_read_bytes = 0
        self.recovering = False

    def charge(self, length):
        used = self.recovery_read_bytes if self.recovering else self.device_read_bytes
        limit = MAX_RECOVERY_READ if self.recovering else MAX_DEVICE_READ
        if length < 0 or used + length > limit:
            raise ValueError('bounded device-read allowance exceeded')
        if self.recovering:
            self.recovery_read_bytes += length
        else:
            self.device_read_bytes += length

    def preflight(self, name):
        if sha(bounded(self.manifest_path, 8 * 1024 * 1024)) != self.manifest_hash:
            raise ValueError('frozen manifest changed')
        self.charge(65536)
        process = subprocess.run([sys.executable, '-I', str(ROOT / 'x9/tools/check_plaintext_root.py'),
                                  '--manifest', str(self.manifest_path), '--serial', self.serial,
                                  '--metadata-only'], capture_output=True, timeout=300)
        (self.directory / name).write_bytes(process.stdout + process.stderr)
        self.record({'operation': 'metadata-only-preflight', 'log': name,
                     'returncode': process.returncode, 'root_read_bytes': 65536,
                     'firmware_payload_read': False})
        if process.returncode:
            raise ValueError('metadata-only root preflight failed; see private log')
        if sha(bounded(self.manifest_path, 8 * 1024 * 1024)) != self.manifest_hash:
            raise ValueError('frozen manifest changed during preflight')
        from userdata_device import resolve_userdata
        if not self.text('getprop ro.build.version.sdk').strip().isdigit():
            raise ValueError('Android must remain running')
        route = resolve_userdata(self.text, self.m)
        if self.route is not None and route != self.route:
            raise ValueError('userdata route changed')
        self.route = route
        for _, part in self.parts:
            self.capture(part)

    def read(self, part, offset, length):
        if not 0 < length <= WRITE:
            raise ValueError('bounded device-read budget exceeded')
        self.charge(length)
        self.capture(part)
        data = super().read(part, offset, length)
        self.record({'operation': 'targeted-read', 'offset': offset, 'bytes': len(data),
                     'part': next(i for i, (_, p) in enumerate(self.parts) if p is part)})
        return data

    def write(self, change, expected, data):
        # pinfile compare-reads OLD and fsync/readbacks NEW internally. Charge
        # both even on failure, in addition to the caller's explicit reads.
        self.charge(2 * len(data))
        return super().write(change, expected, data)

    def read_global(self, offset, length):
        if offset < 0 or offset % BLOCK or length <= 0 or length % BLOCK or offset + length > self.total:
            raise ValueError('global read outside root/extension')
        result = bytearray()
        for base, part in self.parts:
            while length and base <= offset < base + part['bytes']:
                take = min(length, WRITE, base + part['bytes'] - offset)
                result += self.read(part, offset - base, take)
                offset += take
                length -= take
        if length:
            raise ValueError('root/extension mapping gap')
        return bytes(result)

    def snapshot(self, *args):
        raise ValueError('whole-root snapshots are prohibited in targeted mode')

    def verify_hashes(self, *args):
        raise ValueError('whole-root hashing is prohibited in targeted mode')


class RangeStore:
    """Only actually requested original 4K blocks, plus bounded local writes."""
    def __init__(self, session, directory):
        self.session, self.directory = session, directory
        self.size = session.total
        self.base, self.cow = {}, {}
        self.failure = None
        self.frozen = False
        self.originals = (directory / 'read-cache.blocks').open('xb')
        self.local_writes = (directory / 'local-cow.log').open('xb')
        self.cow_bytes = 0
        self.lock = threading.RLock()

    def check(self, offset, length):
        if self.failure:
            raise IOError('sticky source/COW failure: ' + self.failure)
        if offset < 0 or length < 0 or offset + length > self.size:
            raise ValueError('image request outside the fixed-size root')

    def hydrate(self, offset, length):
        first, end = offset // BLOCK, (offset + length + BLOCK - 1) // BLOCK
        at = first
        while at < end:
            if at in self.base:
                at += 1
                continue
            if self.frozen:
                raise ValueError('new read dependency after freeze')
            stop = at + 1
            while stop < end and stop not in self.base and stop - at < WRITE // BLOCK:
                stop += 1
            if (len(self.base) + stop - at) * BLOCK > MAX_READSET:
                raise ValueError('128 MiB necessary-block readset budget exceeded')
            data = self.session.read_global(at * BLOCK, (stop - at) * BLOCK)
            for index in range(at, stop):
                block = data[(index - at) * BLOCK:(index - at + 1) * BLOCK]
                if len(block) != BLOCK:
                    raise IOError('short original block; no zero substitution')
                self.base[index] = (self.originals.tell(), block)
                self.originals.write(block)
            at = stop

    def read(self, offset, length):
        with self.lock:
            self.check(offset, length)
            if not length:
                return b''
            self.hydrate(offset, length)
            first, end = offset // BLOCK, (offset + length + BLOCK - 1) // BLOCK
            data = b''.join(self.cow.get(index, self.base[index][1]) for index in range(first, end))
            return data[offset % BLOCK:offset % BLOCK + length]

    def write(self, offset, data):
        with self.lock:
            self.check(offset, len(data))
            if self.frozen:
                raise ValueError('local write after freeze')
            if not data:
                return
            self.hydrate(offset, len(data))  # every write has a REAL beforeimage
            if self.cow_bytes + len(data) + 12 > MAX_COW_LOG:
                raise ValueError('local COW traffic exceeds 512 MiB')
            first, end = offset // BLOCK, (offset + len(data) + BLOCK - 1) // BLOCK
            for index in range(first, end):
                start = index * BLOCK
                lo, hi = max(start, offset), min(start + BLOCK, offset + len(data))
                block = bytearray(self.cow.get(index, self.base[index][1]))
                block[lo - start:hi - start] = data[lo - offset:hi - offset]
                self.cow[index] = bytes(block)
            if len(self.cow) * BLOCK > MAX_DIFF:
                raise ValueError('local COW footprint exceeds 128 MiB')
            self.local_writes.write(struct.pack('<QI', offset, len(data)))
            self.local_writes.write(data)
            self.cow_bytes += len(data) + 12

    def sync(self):
        with self.lock:
            for stream in (self.originals, self.local_writes):
                stream.flush()
                os.fsync(stream.fileno())

    def close(self):
        self.sync()
        self.originals.close()
        self.local_writes.close()

    def freeze(self):
        with self.lock:
            self.check(0, 0)
            self.sync()
            self.frozen = True
            reads = [{'block': index, 'cache_offset': pos, 'sha256': sha(data)}
                     for index, (pos, data) in sorted(self.base.items())]
            save(self.directory / 'readset.json', reads)
            blocks = self.directory / 'blocks'
            blocks.mkdir()
            changes = []
            different = sorted(index for index, data in self.cow.items() if data != self.base[index][1])
            at = 0
            while at < len(different):
                first = different[at]
                part_index = next(i for i, (base, part) in enumerate(self.session.parts)
                                  if base <= first * BLOCK < base + part['bytes'])
                base, part = self.session.parts[part_index]
                end = at + 1
                while (end < len(different) and different[end] == different[end - 1] + 1 and
                       end - at < WRITE // BLOCK and different[end] * BLOCK < base + part['bytes']):
                    end += 1
                ids = different[at:end]
                old = b''.join(self.base[index][1] for index in ids)
                new = b''.join(self.cow[index] for index in ids)
                ident = '%06d' % len(changes)
                for suffix, data in (('old', old), ('new', new)):
                    path = blocks / (ident + '.' + suffix)
                    path.write_bytes(data)
                    sync_file(path)
                changes.append({'id': ident, 'part': part_index, 'offset': first * BLOCK - base,
                                'bytes': len(old), 'old_sha256': sha(old), 'new_sha256': sha(new)})
                at = end
            if not changes:
                raise ValueError('unexpected empty seven-file installation delta')
            for path in (blocks, self.directory, self.directory.parent):
                sync_directory(path)
            return changes, reads


class RPCServer:
    def __init__(self, store, path):
        self.store, self.path = store, path
        self.stop = threading.Event()
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(str(path))
        os.chmod(path, 0o600)
        self.server.listen(8)
        self.server.settimeout(1)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    @staticmethod
    def receive(connection, count):
        data = bytearray()
        while len(data) < count:
            piece = connection.recv(count - len(data))
            if not piece:
                raise IOError('truncated local RPC')
            data += piece
        return bytes(data)

    def run(self):
        while not self.stop.is_set():
            try:
                connection, _ = self.server.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with connection:
                connection.settimeout(120)
                try:
                    # Private 0700 directory/socket and the helper's getpeereid
                    # binding isolate this endpoint; no TCP/network listener.
                    magic, version, op, offset, length, reserved = REQUEST.unpack(self.receive(connection, REQUEST.size))
                    if magic != MAGIC or version != 1 or reserved or length > MAX_RPC or op not in (1, 2, 3):
                        raise ValueError('invalid local RPC header')
                    if op == 3:
                        if offset or length:
                            raise ValueError('invalid FSYNC request')
                        self.store.check(0, 0)
                        self.store.sync()
                        output, confirmed = b'', 0
                    elif op == 1:
                        output = self.store.read(offset, length)
                        confirmed = len(output)
                    else:
                        self.store.write(offset, self.receive(connection, length))
                        output, confirmed = b'', length
                    connection.sendall(RESPONSE.pack(MAGIC, 0, confirmed, 0) + output)
                except Exception as error:
                    self.store.failure = str(error)
                    try:
                        connection.sendall(RESPONSE.pack(MAGIC, errno.EIO, 0, 0))
                    except OSError:
                        pass

    def close(self):
        self.stop.set()
        self.thread.join(timeout=125)
        self.server.close()
        if self.thread.is_alive():
            raise ValueError('local image RPC did not quiesce')


LOCAL_COPY = r'''
import os, sys
dest, source, mode = sys.argv[1:]
temp = dest + '.x9-bt-targeted'
if os.path.lexists(temp): raise ValueError('temporary path already exists')
try:
    import stat, subprocess
    if os.path.lexists(dest):
        if not stat.S_ISREG(os.lstat(dest).st_mode):
            raise ValueError('existing target is not a regular file')
        subprocess.run(['/bin/cp', '-p', dest, temp], check=True)
        fd = os.open(temp, os.O_WRONLY|os.O_TRUNC|os.O_NOFOLLOW)
    else:
        fd = os.open(temp, os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as out, open(source, 'rb') as incoming:
        while True:
            data = incoming.read(1048576)
            if not data: break
            out.write(data)
        out.flush(); os.fchmod(out.fileno(), int(mode,8)); os.fsync(out.fileno())
    os.replace(temp, dest)
    fd = os.open(os.path.dirname(dest), os.O_RDONLY)
    try: os.fsync(fd)
    finally: os.close(fd)
finally:
    if os.path.lexists(temp): os.unlink(temp)
'''


def build_helper(directory):
    config = json.loads((ROOT / 'x9/build/local.json').read_text())
    developer = Path(config['developer_dir'])
    clang = developer / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    sdk = developer / 'Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
    output = directory / 'range_fs'
    with (directory / 'helper-build.log').open('xb') as log:
        subprocess.run([clang, '-std=c11', '-Wall', '-Wextra', '-Werror', '-D_FILE_OFFSET_BITS=64',
                        '-isysroot', sdk, '-I/usr/local/include/fuse', ROOT / 'x9/bluetooth/range_fs.c',
                        '-L/usr/local/lib', '-Wl,-rpath,/usr/local/lib', '-lfuse', '-pthread',
                        '-o', output], stdout=log, stderr=subprocess.STDOUT, check=True)
    return output


def attached_devices(image):
    """Use DiskImages' authoritative binding even after partial attach."""
    result = subprocess.run(['hdiutil', 'info', '-plist'], capture_output=True,
                            check=True, timeout=30)
    images = plistlib.loads(result.stdout).get('images')
    if not isinstance(images, list):
        raise ValueError('invalid native DiskImages registry')
    matches = [item for item in images if item.get('image-path') and
               Path(item['image-path']).resolve() == image.resolve()]
    devices = []
    for item in matches:
        entries = [e['dev-entry'] for e in item.get('system-entities', []) if e.get('dev-entry')]
        if not entries or any(not re.fullmatch(r'/dev/disk[0-9]+(?:s[0-9]+)*', e) for e in entries):
            raise ValueError('attached image has no valid native device binding')
        devices.append(entries[0])
    return devices


def file_metadata(path):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode):
        raise ValueError('metadata source must be a regular leaf')
    attrs, size = {}, 0
    names = os.listxattr(path, follow_symlinks=False)
    if len(names) > 128:
        raise ValueError('too many target extended attributes')
    for name in names:
        value = os.getxattr(path, name, follow_symlinks=False)
        size += len(value)
        if size > 16 * 1024 * 1024:
            raise ValueError('target extended attributes exceed bounded budget')
        attrs[name] = value
    if info.st_flags & 0x20 or 'com.apple.decmpfs' in attrs:
        raise ValueError('compressed existing target requires a different proven copy contract')
    return info.st_flags, attrs


def cleanup_native(image, mount, fuse_mount, process, server, log, private, directory):
    # Native detach failure MUST keep this actual backing service alive. A
    # foreground hold is intentional; only this exact local image is involved.
    previous = {s: signal.signal(s, signal.SIG_IGN) for s in (signal.SIGINT, signal.SIGTERM)}
    tried_hfs = tried_fuse = announced = False
    try:
        while True:
            try:
                devices = attached_devices(image)
                if devices:
                    if not tried_hfs:
                        tried_hfs = True
                        for device in devices:
                            result = subprocess.run(['hdiutil', 'detach', device], capture_output=True, timeout=300)
                            with (directory / 'native-cleanup.log').open('ab') as out:
                                out.write(result.stdout + result.stderr)
                        continue
                    raise ValueError('local HFS image remains attached')
                if os.path.ismount(mount):
                    raise ValueError('local HFS mount remains active without a confirmed image binding')
                if os.path.ismount(fuse_mount):
                    if not tried_fuse:
                        tried_fuse = True
                        subprocess.run(['/sbin/umount', str(fuse_mount)], capture_output=True, timeout=120)
                        continue
                    raise ValueError('local FUSE mount remains active')
                if process is not None and process.poll() is None:
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        # Recheck after the wait: startup may have mounted late.
                        if attached_devices(image) or os.path.ismount(mount) or os.path.ismount(fuse_mount):
                            continue
                        process.terminate()
                        process.wait(timeout=15)
                    continue  # helper is now exited; recheck ALL consumers
                # Helper cannot create a new mount after exit. A second native
                # registry check closes the late-attach/late-mount window.
                if attached_devices(image) or os.path.ismount(mount) or os.path.ismount(fuse_mount):
                    continue
                break
            except Exception as error:
                if not announced:
                    print(str(error) + '; keeping LOCAL COW/RPC alive; no device writes.', flush=True)
                    print('Detach only this local image:', str(image), flush=True)
                    print('Local FUSE mount:', str(fuse_mount), flush=True)
                    announced = True
                time.sleep(5)
        # Every teardown is after helper exit and proven no image/FS consumer.
        try:
            if server is not None:
                server.close()
            if log is not None:
                log.close()
            shutil.rmtree(private)
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)
        if process is not None and process.returncode:
            raise ValueError('local macFUSE helper failed; no device writes allowed')
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def native_install(session, store, directory, files, jobs, apple):
    helper = build_helper(directory)
    private = Path(tempfile.mkdtemp(prefix='x9bt-', dir='/private/tmp')).resolve()
    os.chmod(private, 0o700)
    fuse_mount = private / 'image'
    fuse_mount.mkdir(mode=0o700)
    mount = directory / 'mount'
    mount.mkdir(mode=0o700)
    image = fuse_mount / 'root.hfs'
    server = log = process = None
    try:
        server = RPCServer(store, private / 'rpc.sock')
        log = (directory / 'fuse.log').open('xb')
        process = subprocess.Popen([helper, private / 'rpc.sock', str(session.total), fuse_mount],
                                   stdout=log, stderr=subprocess.STDOUT)
        for _ in range(150):
            if os.path.ismount(fuse_mount):
                break
            if process.poll() is not None:
                raise ValueError('macFUSE helper exited; see private fuse.log')
            time.sleep(0.1)
        else:
            raise ValueError('macFUSE mount did not become ready')
        attached = subprocess.run(['hdiutil', 'attach', '-nomount',
                                   '-noverify', '-noautofsck',
                                   '-imagekey', 'diskimage-class=CRawDiskImage',
                                   str(image), '-plist'],
                                  capture_output=True, timeout=300)
        (directory / 'hfs-attach.log').write_bytes(attached.stdout + attached.stderr)
        if attached.returncode:
            raise ValueError('native HFS attach failed; see private log')
        entities = plistlib.loads(attached.stdout)['system-entities']
        matches = [entry['dev-entry'] for entry in entities if
                   entry.get('content-hint') in ('Apple_HFS', 'Apple_HFSX') and entry.get('dev-entry')]
        if len(matches) != 1:
            raise ValueError('native HFS volume binding mismatch')
        devices = attached_devices(image)
        if len(devices) != 1:
            raise ValueError('native HFS image registration mismatch')
        mounted = subprocess.run(['diskutil', 'mount', 'nobrowse', '-mountOptions', 'noatime,noowners',
                                  '-mountPoint', str(mount), matches[0]], capture_output=True, timeout=300)
        (directory / 'hfs-mount.log').write_bytes(mounted.stdout + mounted.stderr)
        if mounted.returncode or not os.path.ismount(mount):
            raise ValueError('native HFS mount failed; see private log')
        info = subprocess.run(['diskutil', 'info', '-plist', matches[0]], capture_output=True,
                              check=True, timeout=30)
        if plistlib.loads(info.stdout).get('MountPoint') != str(mount):
            raise ValueError('native mounted volume path mismatch')
        version = bounded(image_path(mount, SYSTEM_VERSION, True), 65536)
        if plistlib.loads(version).get('ProductBuildVersion') != '20H392':
            raise ValueError('live root is not 20H392')
        bluetoothd_identity(bounded(image_path(mount, '/usr/sbin/bluetoothd', True), 64 * 1024 * 1024))
        metadata = {}
        for path, data in {**apple, **jobs}.items():
            if bounded(image_path(mount, path, path in apple), 64 * 1024 * 1024) != data:
                raise ValueError('live original differs: ' + path)
            if path in jobs:
                metadata[path] = file_metadata(image_path(mount, path))
        for path in BINARIES.values():
            target = image_path(mount, path)
            if os.path.lexists(target):
                raise ValueError('new runtime path is not absent: ' + path)
        prepared = directory / 'payload'
        prepared.mkdir()
        for index, (path, data) in enumerate(files.items()):
            target = image_path(mount, path)
            source = prepared / str(index)
            source.write_bytes(data)
            mode = 0o755 if path in BINARIES.values() else 0o644
            subprocess.run([sys.executable, '-I', '-c', LOCAL_COPY, str(target), str(source), '%04o' % mode],
                           capture_output=True, check=True)
            if bounded(target, 64 * 1024 * 1024) != data or stat.S_IMODE(target.stat().st_mode) != mode:
                raise ValueError('native file bytes/mode installation failed: ' + path)
            if path in metadata and file_metadata(target) != metadata[path]:
                raise ValueError('native replacement changed existing xattrs/ACL/flags: ' + path)
        for path, data in {**apple, SYSTEM_VERSION: version}.items():
            if bounded(image_path(mount, path, True), 64 * 1024 * 1024) != data:
                raise ValueError('native update changed an Apple original')
        subprocess.run(['hdiutil', 'detach', devices[0]], capture_output=True, check=True, timeout=300)
        if store.failure:
            raise ValueError('image backend failed: ' + store.failure)
        return version
    finally:
        cleanup_native(image, mount, fuse_mount, process, server, log, private, directory)
        if store.failure:
            raise ValueError('local image IO failed; no device writes allowed')


def validate_target_ledger(session, state):
    changes, attempted = state.get('changes'), state.get('attempted')
    if (state.get('schema') != 2 or state.get('kind') != 'x9-bluetooth-targeted-root-install' or
            not isinstance(changes, list) or not 1 <= len(changes) <= MAX_DIFF // BLOCK or
            not isinstance(attempted, list) or any(type(i) is not int for i in attempted) or
            attempted != list(range(len(attempted))) or len(attempted) > len(changes)):
        raise ValueError('invalid targeted write ledger')
    if state.get('changes_sha256') != sha(json.dumps(changes, sort_keys=True, separators=(',', ':')).encode()):
        raise ValueError('frozen change ledger identity differs')
    previous, total = (-1, -1), 0
    for index, change in enumerate(changes):
        if not isinstance(change, dict):
            raise ValueError('invalid change record')
        part, off, length = (change.get(key) for key in ('part', 'offset', 'bytes'))
        if (change.get('id') != '%06d' % index or type(part) is not int or not 0 <= part < len(session.parts) or
                type(off) is not int or off < 0 or off % BLOCK or type(length) is not int or
                not 0 < length <= WRITE or length % BLOCK or off + length > session.parts[part][1]['bytes'] or
                (part, off) < previous):
            raise ValueError('write ledger overlap or out-of-scope range')
        previous = part, off + length
        total += length
        backup_bytes(session.directory, change, 'old')
        backup_bytes(session.directory, change, 'new')
    if total > MAX_DIFF:
        raise ValueError('targeted delta exceeds 128 MiB')


def frozen_reads(session, directory, state):
    data = bounded(directory / 'readset.json', 8 * 1024 * 1024)
    if sha(data) != state.get('readset_sha256'):
        raise ValueError('frozen readset identity changed')
    reads = json.loads(data)
    if not isinstance(reads, list) or not 0 < len(reads) <= MAX_READSET // BLOCK or len(reads) != state.get('readset_blocks'):
        raise ValueError('invalid readset size')
    previous, offsets = -1, set()
    size = len(reads) * BLOCK
    fd = os.open(directory / 'read-cache.blocks', os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as cache:
        info = os.fstat(cache.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size != size or size != state.get('necessary_cache_bytes'):
            raise ValueError('frozen necessary-block cache geometry changed')
        index_to_offset = {}
        for item in reads:
            if not isinstance(item, dict):
                raise ValueError('invalid readset record')
            index, at = item.get('block'), item.get('cache_offset')
            digest = item.get('sha256')
            if (type(index) is not int or index <= previous or not 0 <= index * BLOCK < session.total or
                    type(at) is not int or at < 0 or at % BLOCK or at + BLOCK > size or at in offsets or
                    not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest)):
                raise ValueError('invalid frozen necessary-block readset')
            cache.seek(at)
            if sha(cache.read(BLOCK)) != digest:
                raise ValueError('necessary-block cache changed')
            offsets.add(at)
            index_to_offset[index] = at
            previous = index
        for change in state['changes']:
            old = backup_bytes(directory, change, 'old')
            base = session.parts[change['part']][0] + change['offset']
            for at in range(0, len(old), BLOCK):
                block = (base + at) // BLOCK
                if block not in index_to_offset:
                    raise ValueError('changed block is not bound to readset')
                cache.seek(index_to_offset[block])
                if cache.read(BLOCK) != old[at:at + BLOCK]:
                    raise ValueError('changed OLD differs from frozen original block')
    return reads


def validate_dependencies(session, directory, reads):
    at = 0
    fd = os.open(directory / 'read-cache.blocks', os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as cache:
        while at < len(reads):
            end = at + 1
            while (end < len(reads) and reads[end]['block'] == reads[end - 1]['block'] + 1 and
                   end - at < WRITE // BLOCK):
                end += 1
            first = reads[at]['block']
            current = session.read_global(first * BLOCK, (end - at) * BLOCK)
            for index, item in enumerate(reads[at:end]):
                cache.seek(item['cache_offset'])
                old = cache.read(BLOCK)
                if len(old) != BLOCK or sha(old) != item['sha256'] or current[index * BLOCK:(index + 1) * BLOCK] != old:
                    raise ValueError('necessary-block cache/live dependency changed')
            at = end
    session.record({'operation': 'readset-revalidated', 'blocks': len(reads), 'bytes': len(reads) * BLOCK})


def rollback_targeted(session, state, directory):
    session.recovering = True
    validate_target_ledger(session, state)
    reads = frozen_reads(session, directory, state)
    state['phase'] = 'rolling-back'
    save(directory / 'state.json', state)
    failures = []
    try:
        session.quiesce()
    except Exception as error:
        failures.append({'quiescence': str(error)})
    if not failures:
        for index in reversed(state['attempted']):
            change = state['changes'][index]
            try:
                old, new = (backup_bytes(directory, change, suffix) for suffix in ('old', 'new'))
                part = session.parts[change['part']][1]
                current = session.read(part, change['offset'], change['bytes'])
                if any(current[pos:pos + BLOCK] not in (old[pos:pos + BLOCK], new[pos:pos + BLOCK])
                       for pos in range(0, len(old), BLOCK)):
                    raise ValueError('unknown/torn/concurrent block; blind rollback refused')
                if current != old:
                    session.write(change, sha(current), old)
                if session.read(part, change['offset'], change['bytes']) != old:
                    raise ValueError('targeted rollback readback mismatch')
            except Exception as error:
                failures.append({'change': index, 'error': str(error)})
    if not failures:
        try:
            validate_dependencies(session, directory, reads)
            session.preflight('preflight-rollback.log')
        except Exception as error:
            failures.append({'preflight': str(error)})
    state.update(phase='rollback-incomplete' if failures else 'rolled-back', rollback_failures=failures,
                 recovery_read_bytes_upper_bound=session.recovery_read_bytes,
                 root_installed=False, runtime_verified=False, audio_verified=False)
    save(directory / 'state.json', state)
    if failures:
        raise ValueError('targeted rollback incomplete; preserve touched-block backups/state')


class LiveView:
    def __init__(self, session):
        self.session, self.size = session, session.total
        self.physical_read_bytes = 0

    def read(self, offset, length):
        if offset < 0 or length < 0 or offset + length > self.size:
            raise ValueError('readonly live view request outside root')
        if not length:
            return b''
        first, end = offset & ~(BLOCK - 1), (offset + length + BLOCK - 1) & ~(BLOCK - 1)
        if self.physical_read_bytes + end - first > MAX_READSET:
            raise ValueError('final live path verification exceeds its reserved 128 MiB allowance')
        self.physical_read_bytes += end - first
        data = self.session.read_global(first, end - first)
        return data[offset - first:offset - first + length]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate', 'payload', 'root-manifest', 'apple-originals', 'state-dir'):
        ap.add_argument('--' + name, type=Path, required=True)
    ap.add_argument('--serial', required=True)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument('--apply', action='store_true')
    mode.add_argument('--rollback', action='store_true')
    ap.add_argument('--confirm-root-modification', choices=[CONFIRM], required=True)
    ap.add_argument('--confirm-android-stays-running', action='store_true', required=True)
    a = ap.parse_args()
    os.umask(0o077)
    root_path, manifest, payload, files, jobs, apple, binding = inputs(a)
    directory = a.state_dir.resolve()
    if sys.platform != 'darwin' or a.state_dir.is_symlink() or not directory.is_relative_to((ROOT / 'x9/out').resolve()) or directory == ROOT / 'x9/out':
        raise ValueError('requires private worktree x9/out state and macOS native HFS/macFUSE')
    root_id = sha(json.dumps([manifest['device_serial'], manifest['partition_guid_hex'], manifest['volume_uuid_hex']],
                             separators=(',', ':')).encode())
    common = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--path-format=absolute', '--git-common-dir'],
                            capture_output=True, text=True, check=True).stdout.strip()
    lock_dir = Path(common).resolve().parent / 'x9/out'
    lock_dir.mkdir(parents=True, exist_ok=True)
    lock = os.open(lock_dir / ('.bt-root-lock-' + root_id), os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    store = None
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if a.apply:
            directory.mkdir(mode=0o700, parents=True, exist_ok=False)
            if shutil.disk_usage(directory).free < MAX_READSET + MAX_COW_LOG + 3 * MAX_DIFF + 32 * 1024 * 1024:
                raise ValueError('insufficient space for necessary-block cache, COW and touched-block recovery')
            state = {'schema': 2, 'kind': 'x9-bluetooth-targeted-root-install', 'binding': binding,
                     'adb_serial': a.serial, 'started': now(), 'phase': 'preflight', 'attempted': [],
                     'whole_root_read': False, 'whole_root_backup': False, 'root_installed': False,
                     'runtime_verified': False, 'audio_verified': False}
            frozen = bounded(root_path, 8 * 1024 * 1024)
            if sha(frozen) != binding['root_manifest_sha256']:
                raise ValueError('root manifest changed')
            (directory / 'root-manifest.json').write_bytes(frozen)
            sync_file(directory / 'root-manifest.json')
            save(directory / 'state.json', state)
        else:
            state = json.loads(bounded(directory / 'state.json', 16 * 1024 * 1024))
            if state.get('binding') != binding or state.get('adb_serial') != a.serial:
                raise ValueError('recovery root/device/package binding mismatch')
            if directory.stat().st_mode & 0o077:
                raise ValueError('recovery directory must remain private')
        session = TargetSession(a.serial, manifest, directory / 'root-manifest.json', directory,
                                binding['root_manifest_sha256'])
        session.recovering = a.rollback
        session.preflight('preflight-before.log' if a.apply else 'preflight-recovery.log')
        if a.rollback:
            rollback_targeted(session, state, directory)
            print('Touched blocks restored and verified; no whole-root read or backup.')
            return
        try:
            from hfs_target import TargetHFS
            store = RangeStore(session, directory)
            version = native_install(session, store, directory, files, jobs, apple)
            fs = TargetHFS(store)
            journal = fs.require_clean_journal()
            fs.patch_owners(files)
            targets = fs.verify_targets(files)
            for path, data in {**apple, SYSTEM_VERSION: version}.items():
                if fs.read_file(path) != data:
                    raise ValueError('detached Apple raw file verification failed before device writes: ' + path)
            changes, reads = store.freeze()
            state.update(changes=changes, readset_blocks=len(reads), phase='prepared',
                         changed_bytes=sum(change['bytes'] for change in changes),
                         necessary_cache_bytes=len(reads) * BLOCK, journal=journal, targets=targets,
                         changes_sha256=sha(json.dumps(changes, sort_keys=True, separators=(',', ':')).encode()),
                         readset_sha256=sha(bounded(directory / 'readset.json', 8 * 1024 * 1024)))
            save(directory / 'state.json', state)
            validate_target_ledger(session, state)
            reads = frozen_reads(session, directory, state)
            session.preflight('preflight-before-write.log')
            validate_dependencies(session, directory, reads)
            # Reserve forward compare/readback + authoritative final path reads
            # BEFORE writing, while the independent recovery allowance is intact.
            if session.device_read_bytes + 3 * state['changed_bytes'] + MAX_READSET + 65536 > MAX_DEVICE_READ:
                raise ValueError('insufficient bounded forward read allowance; no device writes')
            state['phase'] = 'staging'
            save(directory / 'state.json', state)
            for index, change in enumerate(changes):
                backup_bytes(directory, change, 'old')
                data = backup_bytes(directory, change, 'new')
                state['attempted'].append(index)
                save(directory / 'state.json', state)
                session.write(change, change['old_sha256'], data)
            for change in changes:
                current = session.read(session.parts[change['part']][1], change['offset'], change['bytes'])
                if current != backup_bytes(directory, change, 'new'):
                    raise ValueError('final touched-block readback mismatch')
            live = TargetHFS(LiveView(session))
            final_journal = live.require_clean_journal()
            final_targets = live.verify_targets(files)
            for path, data in {**apple, SYSTEM_VERSION: version}.items():
                if live.read_file(path) != data:
                    raise ValueError('live Apple original changed: ' + path)
            session.preflight('preflight-after.log')
            state.update(phase='complete', completed=now(), root_installed=True,
                         device_read_bytes_upper_bound=session.device_read_bytes,
                         final_targets=final_targets, final_journal=final_journal,
                         recovery_read_bytes_upper_bound=session.recovery_read_bytes,
                         runtime_verified=False, audio_verified=False)
            save(directory / 'state.json', state)
        except BaseException as error:
            state['failure'] = str(error)
            state['device_read_bytes_upper_bound'] = session.device_read_bytes
            if state.get('attempted'):
                rollback_targeted(session, state, directory)
            else:
                state.update(phase='failed-without-device-write')
                save(directory / 'state.json', state)
            raise
        print('Seven Bluetooth files installed and verified at live HFS paths.')
        print('Necessary original blocks read: %d; changed-block backups: %d bytes.' %
              (state['necessary_cache_bytes'], state['changed_bytes']))
        print('No whole-root read/backup, boot or flash; runtime/audio remain unverified.')
        print('Private receipt:', directory)
    finally:
        if store is not None:
            store.close()
        os.close(lock)


if __name__ == '__main__':
    def terminate(signum, frame):
        raise KeyboardInterrupt('targeted staging interrupted')
    signal.signal(signal.SIGTERM, terminate)
    main()
