#!/usr/bin/env python3
"""Send an HFS+ root to the X9 shim's RAM receiver. Never flash/reboot Android.

Requires pyusb and libusb; lz4 is optional (independent compressed blocks).
The phone must already be running the receiver boot image, flashed manually.
Without --boot, stop after verification and leave the receiver waiting.
"""
import argparse
from dataclasses import dataclass
from pathlib import Path
import struct
import sys
import time
import zlib

MAGIC = b'X9RAMRX1'
BEGIN, DATA, COMMIT, BOOT, ABORT, STATUS = range(0x40, 0x46)
BLOCK = 32768


@dataclass
class Status:
    state: int
    error: int
    received: int
    total: int
    capacity: int
    crc: int
    block: int

    @classmethod
    def unpack(cls, blob):
        if len(blob) != 48:
            raise ValueError('short receiver status')
        magic, *values = struct.unpack('<8sIIQQQII', blob)
        if magic != MAGIC or values[-1] != BLOCK:
            raise ValueError('device is not the X9 RAM receiver')
        return cls(*values)


def frame(offset, raw, compress=None):
    if not raw or len(raw) > BLOCK:
        raise ValueError('invalid raw frame size')
    codec, data = 0, raw
    if not raw.strip(b'\0'):
        codec, data = 2, b''
    elif compress:
        packed = compress(raw)
        if len(packed) < len(raw):
            codec, data = 1, packed
    return struct.pack('<QIIII', offset, len(raw), codec, zlib.crc32(raw), len(data)) + data


def inspect_root(path):
    size = path.stat().st_size
    with path.open('rb') as f:
        header = f.read(1536)
        if len(header) < 1536 or header[1024:1026] not in (b'H+', b'HX'):
            raise ValueError('send a raw HFS+ image; this XNU build has no APFS driver')
        block, count = struct.unpack_from('>II', header, 1064)
        if block < 512 or block & (block-1) or block*count != size:
            raise ValueError('raw HFS+ geometry must match the file (no DMG/UDIF trailer)')
        crc = zlib.crc32(header)
        while data := f.read(4*1024*1024):
            crc = zlib.crc32(data, crc)
    return size, crc


def status(device):
    return Status.unpack(bytes(device.ctrl_transfer(0xc0, STATUS, 0, 0, 48, timeout=5000)))


def command(device, request, data=b''):
    result = device.ctrl_transfer(0x40, request, 0, 0, data, timeout=10000)
    if result != len(data):
        raise IOError('short USB control write')


def upload(device, path, compress=None, boot=False, output=print):
    total, crc = inspect_root(path)
    initial = status(device)
    if total > initial.capacity:
        raise ValueError(f'root {total} exceeds receiver capacity {initial.capacity}')
    output(f'RAM-only upload: {total/1048576:.2f} MiB; capacity {initial.capacity/1048576:.2f} MiB')
    command(device, BEGIN, struct.pack('<8sQII', MAGIC, total, crc, 1))
    state = status(device)
    if state.state != 1 or state.total != total or state.received or state.error:
        raise IOError('receiver rejected BEGIN')
    started = time.monotonic()
    next_progress = 0
    sent = 0
    with path.open('rb') as source:
        while raw := source.read(BLOCK):
            packet = frame(sent, raw, compress)
            # Each block is acknowledged in both directions. On a lost ACK,
            # query the authoritative offset; do not blindly replay a write.
            for attempt in range(3):
                try:
                    command(device, DATA, packet)
                except Exception:
                    state = status(device)
                    if state.state == 1 and state.received == sent+len(raw) and not state.error:
                        break
                    if state.state != 1 or state.received != sent or attempt == 2:
                        raise
                    continue
                state = status(device)
                if state.state != 1 or state.received != sent+len(raw) or state.error:
                    raise IOError(f'frame rejected: {state}')
                break
            sent += len(raw)
            if sent >= next_progress:
                elapsed = max(time.monotonic()-started, 0.001)
                output(f'{sent/1048576:.1f}/{total/1048576:.1f} MiB; {sent/elapsed/1048576:.2f} MiB/s')
                next_progress = sent+16*1024*1024
    command(device, COMMIT)
    state = status(device)
    if state.state != 2 or state.received != total or state.total != total or state.crc != crc or state.error:
        raise IOError(f'final RAM verification failed: {state}')
    output(f'RAM verified: {total} bytes, CRC32 {crc:08x}')
    if boot:
        command(device, BOOT)
        output('BOOT acknowledged; shim will hand the resident root to XNU')
    else:
        output('Receiver remains ready. Use --boot-ready to hand off to XNU.')
    return state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', nargs='?', type=Path)
    parser.add_argument('--serial', default='X9RAMRX1')
    parser.add_argument('--status', action='store_true')
    parser.add_argument('--boot', action='store_true', help='boot after this upload is verified')
    parser.add_argument('--boot-ready', action='store_true', help='boot a previously verified resident root')
    parser.add_argument('--abort', action='store_true', help='discard the current RAM session metadata')
    parser.add_argument('--raw', action='store_true', help='do not compress nonzero blocks')
    args = parser.parse_args()
    if not (args.root or args.status or args.boot_ready or args.abort):
        parser.error('provide a raw HFS+ root or a receiver action')
    if args.boot and not args.root:
        parser.error('--boot requires a root file; use --boot-ready for a verified root')
    import usb.core
    devices = list(usb.core.find(find_all=True, idVendor=0x1209, idProduct=0x0001))
    matches = []
    for device in devices:
        try:
            if device.serial_number == args.serial:
                status(device)
                matches.append(device)
        except (ValueError, usb.core.USBError):
            continue
    if len(matches) != 1:
        raise RuntimeError(f'expected one X9 receiver, found {len(matches)}; phone must be running receiver boot.img')
    device = matches[0]
    # SET_CONFIGURATION only changes the USB receiver, never phone partitions.
    device.set_configuration(1)
    if args.abort:
        command(device, ABORT)
    if args.status:
        print(status(device))
    if args.boot_ready:
        state = status(device)
        if state.state != 2 or state.error:
            raise RuntimeError('no fully verified resident root is ready')
        command(device, BOOT)
    if args.root:
        compress = None
        if not args.raw:
            try:
                import lz4.block
                compress = lambda data: lz4.block.compress(data, store_size=False)
            except ImportError:
                print('lz4 unavailable; using raw and zero blocks', file=sys.stderr)
        upload(device, args.root, compress, args.boot)


if __name__ == '__main__':
    main()
