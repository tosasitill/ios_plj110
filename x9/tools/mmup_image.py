#!/usr/bin/env python3
"""Android vcp_b.img 'tinysys-mmup-RV33_A' section -> x9/kernel/generated/x9_mmup_image.inc.

LK loads this image into a heap buffer and TF-A makes MMUP copy buf[0x2000:size]
into its own SRAM at reset release. XNU reuses that DRAM before MMUP starts, so
the kernel restores the bytes itself. Vendor data stays in .local, never git.
Absent source -> empty stub (kernel then does nothing).
"""
import argparse
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'x9/kernel/generated/x9_mmup_image.inc'
NAME = b'tinysys-mmup-RV33_A'
MAGIC = bytes.fromhex('88168858')
SKIP = 0x2000  # first 0x2000 bytes are written to SRAM by TF-A itself


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--required', action='store_true')
    a = ap.parse_args()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    src = a.src.expanduser()
    if not src.is_file():
        if a.required:
            sys.exit(f'mmup_image: missing {src}')
        OUT.write_text('/* generated: no MMUP image (stub) */\n'
                       'static const unsigned char x9_mmup_image[1] = {0};\n'
                       'static const unsigned int x9_mmup_image_bytes = 0;\n')
        print('mmup_image: stub (no source)')
        return
    d = src.read_bytes()
    pos = -1
    for off in range(0, len(d) - 0x200, 4):
        if d[off:off + 4] == MAGIC and d[off + 8:off + 8 + len(NAME) + 1] == NAME + b'\0':
            pos = off
            break
    if pos < 0:
        sys.exit(f'mmup_image: section {NAME.decode()} not found in {src}')
    size = int.from_bytes(d[pos + 4:pos + 8], 'little')
    body = d[pos + 0x200:pos + 0x200 + size]
    if size <= SKIP or size > 0x80000 or len(body) != size or body[:4] != bytes.fromhex('b5a00100'):
        sys.exit(f'mmup_image: unexpected section size/header ({size:#x})')
    img = body[SKIP:]
    rows = ',\n'.join(' ' + ','.join(f'0x{b:02x}' for b in img[i:i + 16])
                      for i in range(0, len(img), 16))
    tmp = OUT.with_suffix('.tmp')
    tmp.write_text('/* generated from vendor vcp_b.img; do not commit */\n'
                   f'static const unsigned char x9_mmup_image[{len(img)}] = {{\n{rows}\n}};\n'
                   f'static const unsigned int x9_mmup_image_bytes = {len(img)};\n')
    tmp.replace(OUT)
    print(f'mmup_image: {len(img)} bytes (SRAM {SKIP:#x}..{size:#x}) sha256 '
          f'{hashlib.sha256(img).hexdigest()[:16]}')


if __name__ == '__main__':
    main()
