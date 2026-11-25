#!/usr/bin/env python3
"""Original lockdownd over the device's TCP listener; AFC/installation_proxy.

The 20H392 com.apple.mobile.lockdown launchd job owns IPv4/IPv6 sockets on
port 62078 (System/Library/LaunchDaemons/com.apple.mobile.lockdown.plist), so
the legacy network lockdown protocol needs neither usbmuxd nor an Apple USB
device mux. This tool is the host half of the candidate ECM path
(x9_usb=1/2 -> ue0 -> 192.168.7.0/24); neither that link nor the device-side
lockdown service map has been hardware-verified. It never replays Pair or
Install, never pairs automatically and prints no identifiers, keys or
peer-supplied text.
"""
import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import re
import secrets
import socket
import struct
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pairing import (Lockdown, ProtocolError, LIMIT, decode, read_exact,
                     certificate_bytes, new_pair_record, private_record_path,
                     save_private, verified_session)
from install import AFC, PlistService, inspect_ipa, verify_application

DEFAULT_HOST = '192.168.7.2'
DEFAULT_PORT = 62078
LABEL = 'X9NetLockdown'
INSTALL_SERVICES = ('com.apple.afc', 'com.apple.mobile.installation_proxy')
KNOWN_ERRORS = {'PairingDialogResponsePending', 'UserDeniedPairing', 'PasswordProtected',
                'InvalidHostID', 'InvalidPairRecord', 'MissingPairRecord',
                'PairingProhibitedOverThisConnection', 'SessionInactive',
                'InvalidResponse', 'ServiceProhibited', 'InvalidService'}


def bounded_error(request, error):
    """Map a peer error to a fixed category; never echo peer-supplied text.

    lockdownd's StartService denials carry device-side strings (activation,
    USB-only, developer mode, instance limit). Those literals are matched here
    only to give the operator a class of failure; the text itself is dropped
    because it may embed identifiers.
    """
    if not isinstance(error, str):
        return f'{request} rejected by original service'
    if error in KNOWN_ERRORS:
        return error
    if request == 'StartService':
        if 'only allowed to run over USB' in error:
            return 'StartService: ServiceUsbOnly'
        if 'not activated' in error:
            return 'StartService: ServiceRequiresActivation'
        if 'developer mode' in error.lower():
            return 'StartService: ServiceRequiresDeveloperMode'
        if 'Too many instances' in error:
            return 'StartService: ServiceInstanceLimit'
        if 'look up service' in error:
            return 'StartService: ServiceUnknown'
    return f'{request} rejected by original service'


class NetLockdown(Lockdown):
    """Lockdown message framing over a plain TCP socket, same wire format."""

    def request(self, name, **fields):
        deadline = time.monotonic() + 15
        self.sock.settimeout(15)
        payload = plistlib.dumps(dict(Label=LABEL, Request=name, **fields), fmt=plistlib.FMT_XML)
        if len(payload) > LIMIT:
            raise ProtocolError('lockdown request too large')
        self.sock.sendall(struct.pack('!I', len(payload)) + payload)
        length, = struct.unpack('!I', read_exact(self.sock, 4, deadline))
        if not 1 <= length <= LIMIT:
            raise ProtocolError('invalid bounded lockdown length')
        value = decode(read_exact(self.sock, length, deadline))
        if value.get('Error'):
            raise ProtocolError(bounded_error(name, value['Error']))
        if value.get('Request') != name:
            raise ProtocolError('response Request does not match')
        return value


def connect(host, port, timeout=10):
    if not isinstance(host, str) or not host or len(host) > 253 or any(c.isspace() for c in host):
        raise ProtocolError('invalid device host')
    if type(port) is not int or not 1 <= port <= 65535:
        raise ProtocolError('invalid device lockdown port')
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(15)
    return sock


def device_identity(lockdown):
    udid = lockdown.request('GetValue', Key='UniqueDeviceID').get('Value')
    if not isinstance(udid, str) or not 1 <= len(udid) <= 256:
        raise ProtocolError('original lockdownd returned no bounded device identity')
    return hashlib.sha256(udid.encode('utf-8')).hexdigest()


def require_live_identity(lockdown, expected):
    """Bind a TCP peer to a saved record: ue0 has no USB serial to rely on."""
    if device_identity(lockdown) != expected:
        raise ProtocolError('connected device does not match the pair record')


def load_record(path):
    record = private_record_path(path)
    if not record.exists():
        raise ProtocolError('--record must be an accepted private pair record')
    saved = decode(record.read_bytes())
    pair = saved.get('PairRecord')
    if saved.get('State') != 'device-accepted' or not isinstance(pair, dict):
        raise ProtocolError('pair record is not device-accepted; finish --pair first')
    identity = saved.get('DeviceIdentitySHA256')
    if not isinstance(identity, str) or len(identity) != 64:
        raise ProtocolError('pair record has no device identity digest')
    return saved, pair, identity


def service_socket(lockdown, ctx, pin, host, name):
    reply = lockdown.request('StartService', Service=name)
    port, tls = reply.get('Port'), reply.get('EnableServiceSSL', False)
    if type(port) is not int or not 1 <= port <= 65535 or type(tls) is not bool:
        raise ProtocolError('invalid original StartService port/TLS flag')
    sock = connect(host, port)
    if tls:
        try:
            sock = ctx.wrap_socket(sock)
        except BaseException:
            sock.close()
            raise
        if sock.getpeercert(binary_form=True) != pin:
            sock.close()
            raise ProtocolError('native service device certificate mismatch')
    return sock


def pair(host, port, record_path):
    record = private_record_path(record_path)
    sock = connect(host, port)
    try:
        lockdown = NetLockdown(sock)
        lockdown.query_type()
        identity = device_identity(lockdown)
        pair, state = None, None
        if record.exists():
            saved = decode(record.read_bytes())
            if saved.get('DeviceIdentitySHA256') != identity:
                raise ProtocolError('private record belongs to a different device')
            pair, state = saved.get('PairRecord'), saved.get('State')
            if state not in {'prepared', 'trust-pending', 'device-accepted'}:
                raise ProtocolError('previous Pair outcome is uncertain; preserve record, do not replay')
            if state == 'device-accepted':
                print('Pair was already accepted; use --session/--probe/--install')
                return 0
            if not isinstance(pair, dict):
                raise ProtocolError('private record has no pair record')
        if pair is None:
            key = lockdown.request('GetValue', Key='DevicePublicKey').get('Value')
            pair = new_pair_record(key, str(uuid.uuid4()).upper())
            save_private(record, dict(DeviceIdentitySHA256=identity, PairRecord=pair, State='prepared'))
        public = {k: pair[k] for k in ('HostID', 'SystemBUID', 'HostCertificate',
                                       'RootCertificate', 'DeviceCertificate')}
        # Ambiguous ownership is recorded before sending: a timeout must not
        # turn into an automatic second Pair.
        save_private(record, dict(DeviceIdentitySHA256=identity, PairRecord=pair, State='pair-in-flight'))
        try:
            reply = lockdown.request('Pair', PairRecord=public, ProtocolVersion='2',
                                     PairingOptions={'ExtendedPairingErrors': True})
        except ProtocolError as e:
            if str(e) == 'PairingDialogResponsePending':
                save_private(record, dict(DeviceIdentitySHA256=identity, PairRecord=pair, State='trust-pending'))
            raise
        if 'EscrowBag' in reply:
            pair['EscrowBag'] = certificate_bytes(reply, 'EscrowBag')
        save_private(record, dict(DeviceIdentitySHA256=identity, PairRecord=pair, State='device-accepted'))
        print('Original lockdownd accepted the pair record over TCP; verify with --session')
        return 0
    finally:
        sock.close()


def session(host, port, record_path):
    _, pair, identity = load_record(record_path)
    sock = connect(host, port)
    state = {}

    def operate(lockdown, ctx, pin):
        require_live_identity(lockdown, identity)
        state['value'] = lockdown.request('GetValue', Key='ProductVersion').get('Value')

    try:
        tls = verified_session(NetLockdown(sock), pair, operate)
    finally:
        sock.close()
    version = state.get('value')
    print(f'PASS: StartSession -> {"pinned TLS" if tls else "non-SSL"} -> QueryType -> '
          f'GetValue(ProductVersion={version if isinstance(version, str) else "?"}) -> StopSession')
    return 0


def probe(host, port, record_path):
    _, pair, identity = load_record(record_path)
    sock = connect(host, port)
    results = {}

    def operate(lockdown, ctx, pin):
        require_live_identity(lockdown, identity)
        for name in INSTALL_SERVICES:
            try:
                service = service_socket(lockdown, ctx, pin, host, name)
            except ProtocolError as e:
                results[name] = str(e)
                continue
            service.close()
            results[name] = 'started+reachable'

    try:
        verified_session(NetLockdown(sock), pair, operate)
    finally:
        sock.close()
    for name in INSTALL_SERVICES:
        print(f'{name}: {results.get(name, "not attempted")}')
    ok = all(v == 'started+reachable' for v in results.values()) and len(results) == len(INSTALL_SERVICES)
    print('PASS: both install services started over TCP' if ok else
          'INCOMPLETE: at least one install service did not start; see categories above')
    return 0 if ok else 1


def receipt_expected(path):
    record = private_record_path(path)
    if not record.exists():
        raise ProtocolError('--verify requires an existing private receipt')
    prior = decode(record.read_bytes())
    expected = prior.get('IPA')
    if (not isinstance(expected, dict) or not isinstance(expected.get('bundle'), str)
            or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,254}', expected['bundle'])
            or not isinstance(expected.get('version'), str) or not 1 <= len(expected['version']) <= 128):
        raise ProtocolError('receipt has no valid expected IPA identity')
    return record, prior, expected


def install(host, port, record_path, ipa, receipt_path, replace_existing):
    expected = inspect_ipa(ipa)
    saved, pair, identity = load_record(record_path)
    receipt = private_record_path(receipt_path)
    if receipt.exists():
        raise ProtocolError('receipt exists; use --verify, never silently replay installation')
    sock = connect(host, port)

    def operate(lockdown, ctx, pin):
        require_live_identity(lockdown, identity)
        proxy = PlistService(service_socket(lockdown, ctx, pin, host,
                                            'com.apple.mobile.installation_proxy'))
        try:
            before = proxy.lookup(expected['bundle'])
            if before is not None and not replace_existing:
                raise ProtocolError('this app is already installed; explicit --replace-existing required')
            remote = 'PublicStaging/x9-' + secrets.token_hex(16) + '/install.ipa'
            state = dict(IPA=expected, Transport='tcp', DeviceHost=host,
                         DeviceIdentitySHA256=identity, RemotePath=remote,
                         State='upload-in-flight')
            receipt.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            fd = os.open(receipt, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'wb') as stream:
                stream.write(plistlib.dumps(state, fmt=plistlib.FMT_BINARY))
                stream.flush()
                os.fsync(stream.fileno())
            afc = AFC(service_socket(lockdown, ctx, pin, host, 'com.apple.afc'))
            try:
                afc.upload(ipa, remote, expected)
            finally:
                afc.sock.close()
            state['State'] = 'install-in-flight'
            save_private(receipt, state)
            proxy.install(remote)  # Exactly once. A timeout keeps an ambiguous receipt.
            state['State'] = 'service-complete'
            save_private(receipt, state)
            verify_application(proxy.lookup(expected['bundle']), expected)
            state['State'] = 'installed-lookup-verified'
            save_private(receipt, state)
            print('PASS: TCP AFC upload -> original Install Complete -> native Lookup bundle/version/path')
            print('App launch/persistence and staged-file cleanup remain separate; no automatic cleanup')
        finally:
            proxy.sock.close()

    try:
        verified_session(NetLockdown(sock), pair, operate)
    finally:
        sock.close()
    return 0


def verify(host, port, record_path, receipt_path):
    _, pair, identity = load_record(record_path)
    _, prior, expected = receipt_expected(receipt_path)
    if prior.get('DeviceIdentitySHA256') != identity:
        raise ProtocolError('receipt belongs to a different device than the pair record')
    sock = connect(host, port)

    def operate(lockdown, ctx, pin):
        require_live_identity(lockdown, identity)
        proxy = PlistService(service_socket(lockdown, ctx, pin, host,
                                            'com.apple.mobile.installation_proxy'))
        try:
            verify_application(proxy.lookup(expected['bundle']), expected)
        finally:
            proxy.sock.close()
        print('PASS: authoritative native Lookup confirms this bundle/version; launch still unverified')

    try:
        verified_session(NetLockdown(sock), pair, operate)
    finally:
        sock.close()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    actions = ap.add_mutually_exclusive_group(required=True)
    actions.add_argument('--pair', action='store_true', help='Pair over TCP; Trust stays on the device UI')
    actions.add_argument('--session', action='store_true', help='verified StartSession/StopSession only')
    actions.add_argument('--probe', action='store_true', help='start the AFC and installation_proxy services, close them')
    actions.add_argument('--install', type=Path, metavar='IPA')
    actions.add_argument('--verify', action='store_true', help='only re-check an existing receipt')
    ap.add_argument('--host', default=DEFAULT_HOST, help=f'device address on ue0 (default {DEFAULT_HOST})')
    ap.add_argument('--port', type=int, default=DEFAULT_PORT, help=f'lockdownd TCP port (default {DEFAULT_PORT})')
    ap.add_argument('--record', type=Path, required=True, help='private pair record under .local')
    ap.add_argument('--receipt', type=Path, help='new private receipt (--install) or existing one (--verify)')
    ap.add_argument('--replace-existing', action='store_true', help='explicitly permit updating this one app')
    a = ap.parse_args()
    if a.pair:
        return pair(a.host, a.port, a.record)
    if a.session:
        return session(a.host, a.port, a.record)
    if a.probe:
        return probe(a.host, a.port, a.record)
    if a.receipt is None:
        ap.error('--install/--verify require --receipt')
    if a.install:
        return install(a.host, a.port, a.record, a.install, a.receipt, a.replace_existing)
    return verify(a.host, a.port, a.record, a.receipt)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except ProtocolError as e:
        print(f'network lockdown: {e}; stopped without replay', file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f'network lockdown: {type(e).__name__}; stopped without replay or secret output', file=sys.stderr)
        sys.exit(1)