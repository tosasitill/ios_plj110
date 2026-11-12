#!/usr/bin/env python3
"""Native usbmuxd -> original lockdownd Pair and verified TLS session client.

No custom USB mux, daemon replacement, fabricated Trust, automatic retries or
unpair. --list only discovers local USB entries. --query-type does not pair.
--pair is an explicit state-changing operation, needs a private --record, and
leaves pending Trust to the device UI; invoke again only after user action.
--session requires an existing real usbmuxd pair record and pins its device
certificate. No identifiers, certificates, session IDs or keys are printed.
Requires cryptography only for --pair. This does NOT implement DeviceFamily.
"""
import argparse
import datetime
import os
from pathlib import Path
import plistlib
import secrets
import socket
import ssl
import struct
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
LIMIT = 1024 * 1024
LABEL = 'X9NativeUSB'


class ProtocolError(Exception):
    pass


def read_exact(sock, size, deadline):
    data = bytearray()
    while len(data) < size:
        remaining = deadline-time.monotonic()
        if remaining <= 0:
            raise ProtocolError('bounded response deadline expired')
        sock.settimeout(remaining)
        piece = sock.recv(size-len(data))
        if not piece:
            raise ProtocolError('peer closed before a complete response')
        data.extend(piece)
    return bytes(data)


def decode(data):
    value = plistlib.loads(data)
    if not isinstance(value, dict):
        raise ProtocolError('response is not a plist dictionary')
    return value


def check_reply(value, request):
    error = value.get('Error')
    if error:
        # Do not include arbitrary peer-supplied text (which can contain secrets).
        known = {'PairingDialogResponsePending', 'UserDeniedPairing', 'PasswordProtected',
                 'InvalidHostID', 'InvalidPairRecord', 'MissingPairRecord',
                 'PairingProhibitedOverThisConnection', 'SessionInactive',
                 'InvalidResponse', 'ServiceProhibited', 'InvalidService'}
        raise ProtocolError(error if isinstance(error, str) and error in known
                            else f'{request} rejected by original service')
    if value.get('Request') != request:
        raise ProtocolError('response Request does not match')
    return value


class Usbmux:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        try:
            self.sock.connect(path)
        except BaseException:
            self.sock.close()
            raise
        self.tag = 0

    def close(self):
        self.sock.close()

    def request(self, name, **fields):
        self.tag += 1
        deadline = time.monotonic()+15
        self.sock.settimeout(15)
        body = plistlib.dumps(dict(MessageType=name, ClientVersionString=LABEL,
            ProgName=LABEL, kLibUSBMuxVersion=3, **fields), fmt=plistlib.FMT_XML)
        if len(body) > LIMIT-16:
            raise ProtocolError('usbmux request too large')
        # A timed-out send is ambiguous: never reconnect/replay automatically.
        self.sock.sendall(struct.pack('<4I', len(body)+16, 1, 8, self.tag)+body)
        size, version, message, tag = struct.unpack('<4I', read_exact(self.sock, 16, deadline))
        if not 16 <= size <= LIMIT or (version, message, tag) != (1, 8, self.tag):
            raise ProtocolError('invalid usbmux frame or response tag')
        result = decode(read_exact(self.sock, size-16, deadline))
        if 'Number' in result and (type(result['Number']) is not int or result['Number'] != 0):
            raise ProtocolError(f'usbmux {name} rejected')
        return result

    def devices(self):
        entries = self.request('ListDevices').get('DeviceList')
        if not isinstance(entries, list) or len(entries) > 64:
            raise ProtocolError('invalid bounded USB device list')
        return [d for d in entries if isinstance(d, dict)
                and type(d.get('DeviceID')) is int and 1 <= d['DeviceID'] < 2**32
                and isinstance(d.get('Properties'), dict)
                and d['Properties'].get('ConnectionType') == 'USB']

    def select(self, device_id):
        entries = [d for d in self.devices() if d.get('DeviceID') == device_id]
        if len(entries) != 1:
            raise ProtocolError('selected USB device absent or ambiguous; native enumeration required')
        serial = entries[0]['Properties'].get('SerialNumber')
        if not isinstance(serial, str) or not 1 <= len(serial) <= 256:
            raise ProtocolError('invalid pair-record identity from usbmuxd')
        return serial

    def tunnel(self, device_id, port=62078):
        if type(port) is not int or not 1 <= port <= 65535:
            raise ProtocolError('invalid native service port')
        r = self.request('Connect', DeviceID=device_id, PortNumber=socket.htons(port))
        if r.get('MessageType') != 'Result' or r.get('Number') != 0:
            raise ProtocolError('lockdown connection not accepted')
        return self.sock


class Lockdown:
    def __init__(self, sock):
        self.sock = sock

    def request(self, name, **fields):
        deadline = time.monotonic()+15
        self.sock.settimeout(15)
        payload = plistlib.dumps(dict(Label=LABEL, Request=name, **fields), fmt=plistlib.FMT_XML)
        if len(payload) > LIMIT:
            raise ProtocolError('lockdown request too large')
        self.sock.sendall(struct.pack('!I', len(payload))+payload)
        length, = struct.unpack('!I', read_exact(self.sock, 4, deadline))
        if not 1 <= length <= LIMIT:
            raise ProtocolError('invalid bounded lockdown length')
        return check_reply(decode(read_exact(self.sock, length, deadline)), name)

    def query_type(self):
        if self.request('QueryType').get('Type') != 'com.apple.mobile.lockdown':
            raise ProtocolError('peer is not original mobile lockdownd')


def certificate_bytes(record, name):
    value = record.get(name)
    if not isinstance(value, bytes) or not 1 <= len(value) <= 65536:
        raise ProtocolError(f'missing/invalid {name}')
    return value


def new_pair_record(device_public_key, buid):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    if not isinstance(device_public_key, bytes) or not 1 <= len(device_public_key) <= 65536:
        raise ProtocolError('invalid DevicePublicKey')
    device_key = serialization.load_pem_public_key(device_public_key)
    if not isinstance(device_key, rsa.RSAPublicKey) or device_key.key_size < 2048:
        raise ProtocolError('unqualified device key type/size; no crypto downgrade')
    root_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    host_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    now = datetime.datetime.now(datetime.timezone.utc)
    root_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'X9 Pairing Root')])

    def cert(public, common_name, ca):
        subject = root_name if ca else x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name)])
        return (x509.CertificateBuilder().subject_name(subject).issuer_name(root_name)
            .public_key(public).serial_number(x509.random_serial_number())
            .not_valid_before(now-datetime.timedelta(minutes=5))
            .not_valid_after(now+datetime.timedelta(days=3650))
            .add_extension(x509.BasicConstraints(ca=ca, path_length=0 if ca else None), critical=True)
            .add_extension(x509.KeyUsage(digital_signature=not ca, content_commitment=False,
                key_encipherment=not ca, data_encipherment=False, key_agreement=False,
                key_cert_sign=ca, crl_sign=ca, encipher_only=False, decipher_only=False), critical=True)
            .sign(root_key, hashes.SHA256()).public_bytes(serialization.Encoding.PEM))

    def private(key):
        return key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                                 serialization.NoEncryption())
    return dict(HostID=str(uuid.uuid4()).upper(), SystemBUID=buid,
        RootCertificate=cert(root_key.public_key(), '', True),
        HostCertificate=cert(host_key.public_key(), 'X9 USB Host', False),
        DeviceCertificate=cert(device_key, 'X9 USB Device', False),
        HostPrivateKey=private(host_key), RootPrivateKey=private(root_key))


def private_record_path(path):
    if path.is_symlink():
        raise ProtocolError('record may not be a symlink')
    path = path.resolve()
    if not path.is_relative_to((ROOT/'.local').resolve()):
        raise ProtocolError('pair record must be in project-private .local, never tracked source')
    if path.exists():
        st = path.stat()
        if not path.is_file() or st.st_uid != os.getuid() or st.st_mode & 0o077 or st.st_size > LIMIT:
            raise ProtocolError('existing record must be owned regular mode0600 and bounded')
    return path


def save_private(path, data):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    tmp = path.parent / ('.pair-'+secrets.token_hex(12))
    try:
        fd = os.open(tmp, os.O_WRONLY|os.O_CREAT|os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as f:
            f.write(plistlib.dumps(data, fmt=plistlib.FMT_BINARY)); f.flush(); os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


def verified_session(lockdown, pair, operation=None):
    for k in ('HostID', 'SystemBUID'):
        if not isinstance(pair.get(k), str) or not 1 <= len(pair[k]) <= 256:
            raise ProtocolError(f'pair record missing {k}')
    # Prepare crypto before StartSession; do not leave accepted session without TLS setup.
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False  # USB identity, not a DNS certificate; pin below.
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_verify_locations(cadata=certificate_bytes(pair, 'RootCertificate').decode('ascii'))
    expected = ssl.PEM_cert_to_DER_cert(certificate_bytes(pair, 'DeviceCertificate').decode('ascii'))
    with tempfile.TemporaryDirectory(prefix='x9-pair-tls-') as directory:
        cert, key = Path(directory)/'host.pem', Path(directory)/'host-key.pem'
        for p, data in [(cert, certificate_bytes(pair, 'HostCertificate')),
                        (key, certificate_bytes(pair, 'HostPrivateKey'))]:
            fd = os.open(p, os.O_WRONLY|os.O_CREAT|os.O_EXCL, 0o600)
            with os.fdopen(fd, 'wb') as f: f.write(data)
        ctx.load_cert_chain(cert, key)
        r = lockdown.request('StartSession', HostID=pair['HostID'], SystemBUID=pair['SystemBUID'])
        session = r.get('SessionID')
        if not isinstance(session, str) or not 1 <= len(session) <= 256 or type(r.get('EnableSessionSSL')) is not bool:
            raise ProtocolError('invalid session response; connection will close, no replay')
        try:
            if r['EnableSessionSSL']:
                lockdown.sock = ctx.wrap_socket(lockdown.sock)
                if lockdown.sock.getpeercert(binary_form=True) != expected:
                    raise ProtocolError('device certificate pin mismatch')
            lockdown.query_type()  # Verify a complete response over the actual session transport.
            if operation is not None:
                # Callers may start original native services within this real
                # session. Reuse its verified/pinned TLS context, never downgrade.
                operation(lockdown, ctx, expected)
            lockdown.request('StopSession', SessionID=session)
            return r['EnableSessionSSL']
        finally:
            lockdown.sock.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    action = ap.add_mutually_exclusive_group(required=True)
    action.add_argument('--list', action='store_true')
    action.add_argument('--query-type', action='store_true')
    action.add_argument('--pair', action='store_true')
    action.add_argument('--session', action='store_true')
    ap.add_argument('--device-id', type=int, help='explicit ID from --list; no automatic device selection')
    ap.add_argument('--record', type=Path, help='required by --pair: private mode0600 pending/accepted record')
    ap.add_argument('--usbmux-socket', default='/var/run/usbmuxd')
    a = ap.parse_args()
    if not a.list and (a.device_id is None or not 1 <= a.device_id < 2**32):
        ap.error('select one explicit --device-id')
    if a.pair and a.record is None:
        ap.error('--pair requires --record under .local; original device Trust may be pending')
    record_path = private_record_path(a.record) if a.pair else None
    mux = Usbmux(a.usbmux_socket)
    lockdown = None
    try:
        if a.list:
            devices = mux.devices()
            for d in devices:
                print(f'USB device-id={d.get("DeviceID")} (identity omitted)')
            print(f'{len(devices)} native USB entries; this is NOT pairing/session success')
            return 0
        serial = mux.select(a.device_id)
        pair = None
        saved_state = None
        if a.session:
            raw = mux.request('ReadPairRecord', PairRecordID=serial).get('PairRecordData')
            if not isinstance(raw, bytes) or len(raw) > LIMIT:
                raise ProtocolError('no existing bounded real pair record')
            pair = decode(raw)
        if a.pair:
            buid = mux.request('ReadBUID').get('BUID')
            if not isinstance(buid, str) or not 1 <= len(buid) <= 256:
                raise ProtocolError('original usbmuxd did not provide BUID; not inventing one')
            if record_path.exists():
                saved = decode(record_path.read_bytes())
                if saved.get('PairRecordID') != serial:
                    raise ProtocolError('private pending record belongs to a different device')
                pair = saved.get('PairRecord')
                saved_state = saved.get('State')
                if saved_state not in {'prepared', 'trust-pending', 'device-accepted', 'saved'}:
                    raise ProtocolError('previous Pair/save outcome is uncertain; preserve record, do not replay')
                if saved_state == 'saved':
                    raise ProtocolError('record already saved; use --session, not another Pair')
                if not isinstance(pair, dict) or pair.get('SystemBUID') != buid:
                    raise ProtocolError('pending record and original host BUID disagree')
        lockdown = Lockdown(mux.tunnel(a.device_id))
        lockdown.query_type()
        if a.query_type:
            print('PASS: native usbmux tunnel -> original lockdownd QueryType; NOT paired')
        elif a.session:
            tls = verified_session(lockdown, pair)
            print(f'PASS: original StartSession -> {"verified/pinned TLS" if tls else "non-SSL session"} '
                  '-> QueryType -> StopSession')
        elif a.pair:
            if pair is None:
                key = lockdown.request('GetValue', Key='DevicePublicKey').get('Value')
                pair = new_pair_record(key, buid)
                save_private(record_path, dict(PairRecordID=serial, PairRecord=pair, State='prepared'))
            public = {k:pair[k] for k in ['HostID', 'SystemBUID', 'HostCertificate',
                                         'RootCertificate', 'DeviceCertificate']}
            if saved_state != 'device-accepted':
                # Record ambiguous ownership BEFORE sending. A timeout must not turn
                # into a replay next invocation. Only a real pending response permits
                # explicit continuation after the user accepts original Trust UI.
                save_private(record_path, dict(PairRecordID=serial, PairRecord=pair, State='pair-in-flight'))
                try:
                    r = lockdown.request('Pair', PairRecord=public, ProtocolVersion='2',
                                         PairingOptions={'ExtendedPairingErrors':True})
                except ProtocolError as e:
                    if str(e) == 'PairingDialogResponsePending':
                        save_private(record_path, dict(PairRecordID=serial, PairRecord=pair, State='trust-pending'))
                    raise
                if 'EscrowBag' in r:
                    pair['EscrowBag'] = certificate_bytes(r, 'EscrowBag')
                save_private(record_path, dict(PairRecordID=serial, PairRecord=pair, State='device-accepted'))
            control = Usbmux(a.usbmux_socket)
            try:
                if control.select(a.device_id) != serial:
                    raise ProtocolError('device changed before saving accepted record')
                save_private(record_path, dict(PairRecordID=serial, PairRecord=pair, State='host-save-in-flight'))
                control.request('SavePairRecord', PairRecordID=serial, DeviceID=a.device_id,
                    PairRecordData=plistlib.dumps(pair, fmt=plistlib.FMT_XML))
            finally:
                control.close()
            save_private(record_path, dict(PairRecordID=serial, PairRecord=pair, State='saved'))
            print('Original Pair accepted and native host record saved; verify separately with --session')
        return 0
    finally:
        if lockdown is not None: lockdown.sock.close()
        mux.close()


if __name__ == '__main__':
    try:
        sys.exit(main())
    except ProtocolError as e:
        print(f'USB pairing: {e}; stopped without replay', file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f'USB pairing: {type(e).__name__}; stopped without replay or secret output', file=sys.stderr)
        sys.exit(1)
