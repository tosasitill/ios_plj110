#!/usr/bin/env python3
"""Board TFA98xx container -> x9/kernel/generated/x9_tfa_container.inc.

The container is vendor data captured from the device (.local, never git).
Absent source -> empty stub (the kernel bring-up then refuses before any
hardware access). A present but malformed file is an error, not a stub.
"""
import argparse
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'x9/kernel/generated/x9_tfa_container.inc'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--required', action='store_true')
    a = ap.parse_args()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    src = a.src.expanduser()
    if not src.is_file():
        if a.required:
            sys.exit(f'tfa_container: missing {src}')
        OUT.write_text('/* generated: no TFA container (stub) */\n'
                       'static const unsigned char x9_tfa_container[1] = {0};\n'
                       'static const unsigned int x9_tfa_container_bytes = 0;\n')
        print('tfa_container: stub (no source)')
        return
    data = src.read_bytes()
    if (len(data) < 54 or len(data) > 1 << 20 or data[:2] != b'PM' or data[3:6] != b'_01'
            or int.from_bytes(data[6:10], 'little') != len(data)
            or int.from_bytes(data[40:42], 'little') != 2):
        sys.exit(f'tfa_container: {src} is not a 2-device PMx_01 container')
    if (zlib.crc32(data[14:]) & 0xffffffff) != int.from_bytes(data[10:14], 'little'):
        sys.exit(f'tfa_container: {src} CRC mismatch')
    rows = ',\n'.join(' ' + ','.join(f'0x{b:02x}' for b in data[i:i + 16])
                      for i in range(0, len(data), 16))
    tmp = OUT.with_suffix('.tmp')
    tmp.write_text('/* generated from the board container; do not commit */\n'
                   f'static const unsigned char x9_tfa_container[{len(data)}] = {{\n{rows}\n}};\n'
                   f'static const unsigned int x9_tfa_container_bytes = {len(data)};\n')
    tmp.replace(OUT)
    print(f'tfa_container: {len(data)} bytes from {src}')


if __name__ == '__main__':
    main()
