#!/usr/bin/env python3
"""Pack plain arm64 SWEET XNU using the existing SWEETXNU v1 format."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from shared import payload


KERNEL_TAG = b'HelioDarwin-X9-0186\n'
STUB_TAG = b'X9 STUB: direct kernel entry\n'


def build(kernel, adt, cmdline, ramdisk=None, *, loader_stub=False):
    if len(cmdline.encode()) >= payload.BOOT_LINE_LENGTH or '\0' in cmdline:
        raise ValueError('boot arguments must fit 607 bytes and contain no NUL')
    data = Path(kernel).read_bytes()
    tag = STUB_TAG if loader_stub else KERNEL_TAG
    if tag not in data:
        raise ValueError('not an X9 loader stub' if loader_stub else
                         'kernel lacks X9 board isolation: rebuild SWEET with patch 0186')
    macho = payload.parse_macho(data)
    if macho['cputype'] != 0x100000c or macho['cpusub'] != 0:
        raise ValueError('use a plain arm64 SWEET kernel, not arm64e/PAC')
    if macho['entry'] is None:
        raise ValueError('kernel has no LC_UNIXTHREAD entry')
    blob, info = payload.build(str(kernel), str(adt), cmdline, str(ramdisk) if ramdisk else None)
    info['kernel_sha256'] = hashlib.sha256(data).hexdigest()
    info['kernel_kind'] = 'loader-test-stub' if loader_stub else 'SWEET-XNU-candidate'
    info['x9_board_isolation_tag'] = not loader_stub
    info['payload_sha256'] = hashlib.sha256(blob).hexdigest()
    return blob, info


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('kernel', type=Path)
    ap.add_argument('adt', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--cmdline', default='debug=0xc serial=3 cpus=1 keepsyms=1 -v')
    ap.add_argument('--ramdisk', type=Path, help='optional md0 image, not the Android vendor ramdisk')
    a = ap.parse_args()
    blob, info = build(a.kernel, a.adt, a.cmdline, a.ramdisk)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(blob)
    a.out.with_suffix('.json').write_text(json.dumps(info, indent=2) + '\n')
    print(f'{a.out}: {len(blob)} bytes, entry {info["entry"]}')


if __name__ == '__main__':
    main()
