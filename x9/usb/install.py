#!/usr/bin/env python3
"""Original USB pairing -> AFC -> installation_proxy -> authoritative Lookup.

Requires real native USB enumeration and an existing pair record. Never pairs,
unpairs, resets USB, replays Install, removes staged files or lists other apps.
A private receipt is persisted before every ambiguous mutation. An interrupted
Install can only be checked with --verify, not silently repeated.
"""
import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import secrets
import stat
import struct
import sys
import time
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pairing import (Usbmux, Lockdown, ProtocolError, LIMIT, decode, read_exact,
                     private_record_path, save_private, verified_session)

MAX_IPA = 1024 * 1024 * 1024
MAGIC = b'CFA6LPAA'


def inspect_ipa(path):
    if path.is_symlink() or not path.is_file() or not 1 <= path.stat().st_size <= MAX_IPA:
        raise ProtocolError('IPA must be a bounded regular file, not a symlink')
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    with zipfile.ZipFile(path) as ipa:
        infos = ipa.infolist()
        if len(infos) > 50000 or len({i.filename for i in infos}) != len(infos):
            raise ProtocolError('too many or duplicate IPA entries')
        expanded = 0
        for info in infos:
            name = PurePosixPath(info.filename)
            expanded += info.file_size
            if (name.is_absolute() or '..' in name.parts or '\\' in info.filename
                    or '\0' in info.filename or info.flag_bits & 1 or expanded > 4 * MAX_IPA):
                raise ProtocolError('unsafe/oversized/encrypted IPA archive')
            mode = info.external_attr >> 16
            if stat.S_ISLNK(mode):
                raise ProtocolError('IPA symlinks are not accepted by this bounded installer')
            if stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR):
                raise ProtocolError('unsupported IPA file type')
        main = [i for i in infos if re.fullmatch(r'Payload/[^/]+\.app/Info\.plist', i.filename)]
        if len(main) != 1 or main[0].file_size > LIMIT:
            raise ProtocolError('IPA needs exactly one bounded main Info.plist')
        info = decode(ipa.read(main[0]))
        bundle, version, executable = (info.get(k) for k in ('CFBundleIdentifier', 'CFBundleVersion', 'CFBundleExecutable'))
        if (not isinstance(bundle, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,254}', bundle)
                or not isinstance(version, str) or not 1 <= len(version) <= 128
                or not isinstance(executable, str) or not re.fullmatch(r'[^/\\\x00]{1,255}', executable)
                or executable in ('.', '..')):
            raise ProtocolError('invalid IPA bundle identity/version/executable')
        program = main[0].filename.removesuffix('Info.plist') + executable
        entry = ipa.getinfo(program)
        if entry.file_size < 32:
            raise ProtocolError('main executable is not Mach-O')
        with ipa.open(entry) as stream:
            header = stream.read(32)
            magic, cpu, _, kind, ncmds, nbytes, _, _ = struct.unpack('<8I', header)
            if magic != 0xfeedfacf or cpu != 0x100000c or kind != 2 or ncmds > 16384 or nbytes > 4 * LIMIT:
                raise ProtocolError('installer currently accepts thin arm64 iOS executables only')
            commands = stream.read(nbytes)
            at = 0
            for _ in range(ncmds):
                if at + 8 > len(commands):
                    raise ProtocolError('truncated Mach-O commands')
                cmd, size = struct.unpack_from('<2I', commands, at)
                if size < 8 or size % 4 or at + size > len(commands):
                    raise ProtocolError('invalid Mach-O command size')
                if cmd in (0x21, 0x2c) and (size < 20 or struct.unpack_from('<I', commands, at + 16)[0]):
                    raise ProtocolError('FairPlay-encrypted IPA is not supported')
                at += size
            if at != nbytes or len(commands) != nbytes:
                raise ProtocolError('invalid Mach-O command range')
    return dict(bundle=bundle, version=version, sha256=digest, size=path.stat().st_size)


class PlistService:
    def __init__(self, sock):
        self.sock = sock

    def send(self, value):
        data = plistlib.dumps(value, fmt=plistlib.FMT_XML)
        if len(data) > LIMIT:
            raise ProtocolError('service request too large')
        self.sock.settimeout(15)
        self.sock.sendall(struct.pack('!I', len(data)) + data)

    def receive(self, deadline):
        size, = struct.unpack('!I', read_exact(self.sock, 4, deadline))
        if not 1 <= size <= LIMIT:
            raise ProtocolError('invalid installation_proxy frame length')
        value = decode(read_exact(self.sock, size, deadline))
        if value.get('Error') or value.get('ErrorDescription'):
            raise ProtocolError('original installation_proxy rejected operation (details omitted)')
        return value

    def lookup(self, bundle):
        self.send(dict(Command='Lookup', ClientOptions={
            'BundleIDs': [bundle], 'ApplicationType': 'User',
            'ReturnAttributes': ['CFBundleIdentifier', 'CFBundleVersion', 'Path', 'ApplicationType']}))
        deadline = time.monotonic() + 30
        result = {}
        saw_result = False
        for _ in range(256):
            reply = self.receive(deadline)
            if 'LookupResult' in reply:
                found = reply['LookupResult']
                if not isinstance(found, dict) or set(found) - {bundle}:
                    raise ProtocolError('unexpected unbounded Lookup result')
                if bundle in found and (not isinstance(found[bundle], dict)
                        or found[bundle].get('CFBundleIdentifier') != bundle):
                    raise ProtocolError('malformed Lookup application identity')
                saw_result = True
                result.update(found)
            if reply.get('Status') == 'Complete':
                if not saw_result:
                    raise ProtocolError('Lookup Complete without LookupResult is not evidence of absence')
                return result.get(bundle)
        raise ProtocolError('Lookup response count exceeded')

    def install(self, remote):
        self.send(dict(Command='Install', PackagePath=remote,
                       ClientOptions={'ApplicationType': 'User'}))
        deadline = time.monotonic() + 180
        for _ in range(4096):
            if self.receive(deadline).get('Status') == 'Complete':
                return
        raise ProtocolError('installation progress count exceeded')


class AFC:
    def __init__(self, sock):
        self.sock, self.sequence = sock, 0

    def request(self, operation, arguments=b'', payload=b'', allowed=(), response=1):
        self.sequence += 1
        deadline = time.monotonic() + 30
        self.sock.settimeout(30)
        head = struct.pack('<8s4Q', MAGIC, 40 + len(arguments) + len(payload),
                           40 + len(arguments), self.sequence, operation)
        self.sock.sendall(head + arguments + payload)
        magic, size, header, sequence, op = struct.unpack('<8s4Q', read_exact(self.sock, 40, deadline))
        if magic != MAGIC or not 40 <= header <= size <= LIMIT or sequence != self.sequence:
            raise ProtocolError('invalid bounded AFC response')
        body = read_exact(self.sock, size - 40, deadline)
        if op == 1:
            if len(body) != 8:
                raise ProtocolError('malformed AFC status')
            status, = struct.unpack('<Q', body)
            if status != 0 and status not in allowed:
                raise ProtocolError(f'AFC operation {operation} failed with status {status}; not replayed')
            if response != 1:
                raise ProtocolError('AFC status does not contain the expected operation data')
            return op, body
        if op != response:
            raise ProtocolError('unexpected AFC response operation')
        return op, body

    def upload(self, path, remote, expected):
        # Parent may already exist. A new unpredictable child MUST be created;
        # no overwrite or cleanup of an earlier staging directory is attempted.
        self.request(9, b'PublicStaging\0', allowed=(16,))
        directory = remote.rsplit('/', 1)[0]
        self.request(9, directory.encode('ascii') + b'\0')
        op, body = self.request(13, struct.pack('<Q', 3) + remote.encode('ascii') + b'\0', response=14)
        if op != 14 or len(body) != 8:
            raise ProtocolError('AFC did not return an open handle')
        handle = body
        digest, total = hashlib.sha256(), 0
        with path.open('rb') as source:
            for data in iter(lambda: source.read(64 * 1024), b''):
                if total + len(data) > expected['size']:
                    raise ProtocolError('IPA changed during transfer; not installed')
                self.request(16, handle, data)
                digest.update(data)
                total += len(data)
        if total != expected['size'] or digest.hexdigest() != expected['sha256']:
            raise ProtocolError('IPA changed during transfer; not installed')
        self.request(20, handle)
        op, body = self.request(10, remote.encode('ascii') + b'\0', response=2)
        if op != 2 or not body.endswith(b'\0'):
            raise ProtocolError('invalid AFC file info')
        parts = body.rstrip(b'\0').split(b'\0')
        if len(parts) % 2:
            raise ProtocolError('invalid AFC info pairs')
        info = dict(zip(parts[::2], parts[1::2]))
        if info.get(b'st_size') != str(total).encode('ascii') or info.get(b'st_ifmt') != b'S_IFREG':
            raise ProtocolError('AFC staged file size/type mismatch')


def service(lockdown, ctx, pin, a, serial, name):
    reply = lockdown.request('StartService', Service=name)
    port, tls = reply.get('Port'), reply.get('EnableServiceSSL', False)
    if type(port) is not int or not 1 <= port <= 65535 or type(tls) is not bool:
        raise ProtocolError('invalid original StartService port/TLS flag')
    mux = Usbmux(a.usbmux_socket)
    sock = mux.sock
    try:
        if mux.select(a.device_id) != serial:
            raise ProtocolError('USB identity changed before service connection')
        sock = mux.tunnel(a.device_id, port)
        if tls:
            sock = ctx.wrap_socket(sock)
            if sock.getpeercert(binary_form=True) != pin:
                raise ProtocolError('native service device certificate mismatch')
        return sock
    except BaseException:
        sock.close()
        raise


def verify_application(entry, expected):
    if not isinstance(entry, dict) or entry.get('CFBundleIdentifier') != expected['bundle']:
        raise ProtocolError('native Lookup does not show the expected installed application')
    if entry.get('CFBundleVersion') != expected['version']:
        raise ProtocolError('native Lookup version does not match this IPA')
    path = entry.get('Path')
    if not isinstance(path, str) or not path.startswith(('/private/var/containers/Bundle/', '/var/containers/Bundle/')):
        raise ProtocolError('native Lookup does not report an installed user bundle path')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    actions = ap.add_mutually_exclusive_group(required=True)
    actions.add_argument('--install', type=Path, metavar='IPA')
    actions.add_argument('--verify', action='store_true', help='only check an existing receipt; never replay Install')
    ap.add_argument('--receipt', type=Path, required=True, help='new private .local receipt for --install')
    ap.add_argument('--device-id', type=int, required=True)
    ap.add_argument('--replace-existing', action='store_true', help='explicitly permit updating this one app')
    ap.add_argument('--usbmux-socket', default='/var/run/usbmuxd')
    a = ap.parse_args()
    if not 1 <= a.device_id < 2**32:
        ap.error('select explicit device-id from pairing.py --list')
    receipt = private_record_path(a.receipt)
    if a.install:
        if receipt.exists():
            raise ProtocolError('receipt exists; use --verify, never silently replay installation')
        expected = inspect_ipa(a.install)
    else:
        if not receipt.exists():
            raise ProtocolError('--verify requires an existing private receipt')
        prior = decode(receipt.read_bytes())
        expected = prior.get('IPA')
        if (not isinstance(expected, dict) or not isinstance(expected.get('bundle'), str)
                or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,254}', expected['bundle'])
                or not isinstance(expected.get('version'), str)):
            raise ProtocolError('receipt has no valid expected IPA identity')
    mux = Usbmux(a.usbmux_socket)
    lockdown = None
    try:
        serial = mux.select(a.device_id)
        identity = hashlib.sha256(serial.encode('utf-8')).hexdigest()
        if a.verify and prior.get('DeviceIdentitySHA256') != identity:
            raise ProtocolError('receipt belongs to a different USB device')
        raw = mux.request('ReadPairRecord', PairRecordID=serial).get('PairRecordData')
        if not isinstance(raw, bytes) or not 1 <= len(raw) <= LIMIT:
            raise ProtocolError('real host pair record is required')
        pair = decode(raw)
        lockdown = Lockdown(mux.tunnel(a.device_id))
        lockdown.query_type()

        def operate(ld, ctx, pin):
            proxy_sock = service(ld, ctx, pin, a, serial, 'com.apple.mobile.installation_proxy')
            try:
                proxy = PlistService(proxy_sock)
                before = proxy.lookup(expected['bundle'])
                if a.verify:
                    verify_application(before, expected)
                    print('PASS: authoritative native Lookup confirms this bundle/version; launch still unverified')
                    return
                if before is not None and not a.replace_existing:
                    raise ProtocolError('this app is already installed; explicit --replace-existing required')
                remote = 'PublicStaging/x9-' + secrets.token_hex(16) + '/install.ipa'
                state = dict(IPA=expected, DeviceIdentitySHA256=identity,
                             RemotePath=remote, State='upload-in-flight')
                # Atomically claim the new receipt before the first file write.
                # Two concurrent invocations with the same receipt must not both
                # pass an exists() precheck and replace each other's in-flight state.
                receipt.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                fd = os.open(receipt, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                with os.fdopen(fd, 'wb') as stream:
                    stream.write(plistlib.dumps(state, fmt=plistlib.FMT_BINARY))
                    stream.flush()
                    os.fsync(stream.fileno())
                afc_sock = service(ld, ctx, pin, a, serial, 'com.apple.afc')
                try:
                    AFC(afc_sock).upload(a.install, remote, expected)
                finally:
                    afc_sock.close()
                state['State'] = 'install-in-flight'
                save_private(receipt, state)
                proxy.install(remote)  # Exactly once. Timeout retains ambiguous receipt.
                state['State'] = 'service-complete'
                save_private(receipt, state)
                verify_application(proxy.lookup(expected['bundle']), expected)
                state['State'] = 'installed-lookup-verified'
                save_private(receipt, state)
                print('PASS: AFC upload -> original Install Complete -> native Lookup bundle/version/path')
                print('App launch/persistence and staged-file cleanup remain separate; no automatic cleanup')
            finally:
                proxy_sock.close()

        verified_session(lockdown, pair, operate)
        return 0
    finally:
        if lockdown:
            lockdown.sock.close()
        mux.close()


if __name__ == '__main__':
    try:
        sys.exit(main())
    except ProtocolError as e:
        print(f'USB install: {e}; stopped without replay', file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f'USB install: {type(e).__name__}; stopped without replay or secret output', file=sys.stderr)
        sys.exit(1)
