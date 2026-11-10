#!/usr/bin/env python3
"""Select the LK-loaded MT6993 GNSS payload's identity, not a signature verdict.

Saved LK gps_post_allocation(5a03c) calls load_fw_image(b6a24) into EMI
region1 (CONN_RO), strips its 512B outer header, and cleans the destination.
It does NOT strip the alternative Linux loader's 48B per-file header.
Generate length/SHA256 only: XNU can qualify a retained LK image without
embedding another multi-megabyte copy or overwriting firmware/NV/HIF RAM.
Certificates, this digest and image presence do not grant boot/EMI authority.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RO_BYTES = 0x3c0000


def payload(blob):
    if len(blob) < 512:
        raise ValueError('GNSS input must be the complete MTK container, not a raw payload')
    magic, low = struct.unpack_from('<II', blob)
    name = blob[8:40]
    extension, header, version = struct.unpack_from('<III', blob, 48)
    alignment, high = struct.unpack_from('<II', blob, 68)
    size = low | (high << 32)
    if (magic != 0x58881688 or extension != 0x58891689 or header != 512 or
            version != 1 or alignment != 16 or name != b'connsys_gnss'.ljust(32, b'\0')):
        raise ValueError('unsupported GNSS outer image/header; no Linux48B or Wi-Fi parser fallback')
    if size < 48 or size > RO_BYTES or size & 3 or size > len(blob) - header:
        raise ValueError('GNSS payload exceeds its container or CONN_RO interval')
    return blob[header:header + size]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', type=Path, default=ROOT / '.local/x9/gps-fw/connsys_gnss.bin')
    ap.add_argument('--out', type=Path, default=ROOT / 'x9/kernel/generated/x9_gps_firmware.inc')
    ap.add_argument('--metadata', type=Path)
    ap.add_argument('--required', action='store_true')
    a = ap.parse_args()
    try:
        if not a.src.is_file():
            if a.required:
                raise ValueError('required GNSS container is absent; no stub written')
            data, container_hash = b'', ''
        else:
            # The captured partition is 8MiB; bound input before reading.
            if not 512 <= a.src.stat().st_size <= 0x800000:
                raise ValueError('GNSS container size outside the supported partition bound')
            blob = a.src.read_bytes()
            data, container_hash = payload(blob), hashlib.sha256(blob).hexdigest()
        digest = hashlib.sha256(data).digest() if data else bytes(32)
        text = ('/* Generated GNSS build-input identity; not authentication or a power/boot lease. */\n'
                f'#define X9_GPS_FW_PRESENT {int(bool(data))}\n'
                f'#define X9_GPS_FW_BYTES {len(data)}u\n'
                'static const unsigned char x9_gps_fw_sha256[32] = {' +
                ','.join(f'0x{v:02x}' for v in digest) + '};\n')
        if not a.out.is_file() or a.out.read_text() != text:
            a.out.parent.mkdir(parents=True, exist_ok=True)
            a.out.write_text(text)
        if a.metadata:
            record = dict(present=bool(data), image='connsys_gnss', outer_header_bytes=512,
                          payload_bytes=len(data), conn_ro_bytes=RO_BYTES,
                          container_sha256=container_hash,
                          payload_sha256=digest.hex() if data else '',
                          authenticated=False, selected_runtime=False,
                          scope='explicit build-input identity; retained-image qualification only')
            a.metadata.parent.mkdir(parents=True, exist_ok=True)
            a.metadata.write_text(json.dumps(record, indent=2) + '\n')
    except (OSError, ValueError, struct.error) as e:
        sys.exit(f'gps_firmware: {e}')
    print(f'GPS firmware identity: {len(data)}B payload, {"selected input" if data else "absent"}; no firmware bytes embedded')


if __name__ == '__main__':
    main()
