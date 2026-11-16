#!/usr/bin/env python3
"""Index MT6653 BT firmware and emit private, gitignored download payload.

The BT container is LITTLE endian, unlike the gen4m Wi-Fi patch descriptor.
Layout and fields: OPPO linux_v2 btmtk_main.h (_Section_Map) and
btmtk_main.c (btmtk_parsing_fw_rom_patch / btmtk_load_fw_by_bin_info).
The board flavor is confirmed by captured Android startup and mt6993.dts.
Generating a payload does not download it or claim calibration/ HCI ready.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NAME = 'BT_RAM_CODE_MT6653_dx5_2g2a_1_hdr.bin'


def index(blob):
    if not 96 <= len(blob) <= 4 * 1024 * 1024:
        raise ValueError('BT patch size outside 96 B..4 MiB')
    if struct.unpack_from('<I', blob, 24)[0] != 0xffffffff:
        raise ValueError('BT patch is not a v2 container')
    count = struct.unpack_from('<I', blob, 44)[0]
    end = 96 + 64 * count
    if not 1 <= count <= 32 or end > len(blob):
        raise ValueError('invalid little-endian section count/map')
    rows, ranges = [], []
    for i in range(count):
        v = struct.unpack_from('<16I', blob, 96 + 64 * i)
        typ, off, size, addr, dl = v[:5]
        if off < end or off > len(blob) or size > len(blob) - off:
            raise ValueError(f'section {i} outside firmware')
        if dl > size or (dl and (typ & 0xffff) != 2):
            raise ValueError(f'section {i} invalid download size/type')
        if dl and addr + dl > 0x100000000:
            raise ValueError(f'section {i} address overflow')
        if size and any(off < b and a < off + size for a, b in ranges):
            raise ValueError(f'section {i} overlapping payload')
        ranges.append((off, off + size))
        rows.append((typ, off, size, addr, dl, v[7], v[8]))
    if not any(r[5] == 2 and r[4] for r in rows):
        raise ValueError('missing BT ILM section (bin type 2)')
    return rows


def render(src, blob=None):
    path = src / NAME
    if blob is None:
        blob = path.read_bytes() if path.is_file() else b''
    rows = index(blob) if blob else []
    out = ['/* Generated BT metadata only; no firmware or device identities. */',
           f'#define X9_BT_FW_PRESENT {int(bool(blob))}',
           f'#define X9_BT_FW_BYTES {len(blob)}u',
           f'#define X9_BT_FW_SECTIONS {len(rows)}u',
           f'static const char x9_bt_fw_sha256[] = "{hashlib.sha256(blob).hexdigest() if blob else ""}";',
           'static const struct x9_bt_section x9_bt_fw_sections[] = {']
    for row in rows:
        out.append('    {' + ', '.join(f'0x{x:08x}u' for x in row) + '},')
    if not rows:
        out.append('    {0},')
    out.append('};')
    return '\n'.join(out) + '\n', rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', type=Path, default=ROOT / '.local/x9/wifi-fw')
    ap.add_argument('--out', type=Path, default=ROOT / 'x9/kernel/generated/x9_bt_firmware.inc')
    ap.add_argument('--payload-out', type=Path, default=ROOT / 'x9/kernel/generated/x9_bt_payload.inc')
    ap.add_argument('--required', action='store_true',
                    help='reject missing/empty real firmware instead of generating a passive stub')
    a = ap.parse_args()
    try:
        path = a.src / NAME
        blob = path.read_bytes() if path.is_file() else b''
        if a.required and not blob:
            raise ValueError('captured production firmware is required for complete initialization')
        text, rows = render(a.src, blob)
        old = a.out.read_text() if a.out.is_file() else None
        a.out.parent.mkdir(parents=True, exist_ok=True)
        if old != text:
            a.out.write_text(text)
        payload = ['/* Private firmware bytes. Generated; never commit. */',
                   f'#define X9_BT_PAYLOAD_BYTES {len(blob)}u',
                   'static const uint8_t x9_bt_payload[] = {']
        for at in range(0, len(blob), 16):
            payload.append('    ' + ','.join(f'0x{b:02x}' for b in blob[at:at + 16]) + ',')
        if not blob:
            payload.append('    0,')
        payload.append('};')
        payload_text = '\n'.join(payload) + '\n'
        a.payload_out.parent.mkdir(parents=True, exist_ok=True)
        if not a.payload_out.is_file() or a.payload_out.read_text() != payload_text:
            a.payload_out.write_text(payload_text)
    except (OSError, ValueError) as e:
        sys.exit(f'bluetooth firmware: {e}')
    print(f'bluetooth firmware: {len(rows)} indexed sections; {len(blob)} private payload bytes; '
          f'{"unchanged" if old == text else "written"}'
          + ('' if rows else ' (absent/stub)'))


if __name__ == '__main__':
    main()
