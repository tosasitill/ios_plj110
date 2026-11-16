#!/usr/bin/env python3
"""Index an original MTK tinysys-sspm partition, without booting or modifying it.

Outer MTK header and inner segment traversal match the captured BL2 loader.
Optional extraction keeps input/derived files separate under x9/out/. This
does NOT prove a running LK handoff, choose a baud profile or emit a boot image.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def segments(data):
    if len(data) < 512 or struct.unpack_from('<I', data)[0] != 0x58881688:
        raise ValueError('missing outer MediaTek partition header')
    size = struct.unpack_from('<I', data, 4)[0]
    name = data[8:40].split(b'\0', 1)[0]
    if name != b'tinysys-sspm' or size > len(data) - 512:
        raise ValueError('wrong partition name or out-of-bounds image')
    if struct.unpack_from('<II', data, 48) != (0x58891689, 512):
        raise ValueError('unsupported outer header extension')
    at, limit = 512, 512 + size
    items, ids = [], set()
    while at + 24 <= limit and struct.unpack_from('<I', data, at)[0] == 0x58901690:
        _, header, length, alignment, identity, flags = struct.unpack_from('<6I', data, at)
        if (header != 24 or alignment == 0 or alignment & (alignment - 1) or
                alignment > 4096 or identity in ids or not length):
            raise ValueError('invalid segment header/alignment/duplicate ID')
        start = at + header
        end = start + length
        next_at = start + ((length + alignment - 1) & -alignment)
        if end > limit or next_at > limit:
            raise ValueError('segment exceeds outer image')
        payload = data[start:end]
        items.append({'id': identity, 'header_offset': at, 'data_offset': start,
                      'bytes': length, 'alignment': alignment, 'flags': flags,
                      'sha256': hashlib.sha256(payload).hexdigest(),
                      'head_words_le': [f'{v:08x}' for v in
                          struct.unpack_from('<6I', payload)] if length >= 24 else []})
        ids.add(identity)
        at = next_at
    if not {0, 1, 99} <= ids:
        raise ValueError('BL2-required segments 0/1/99 are missing')
    return items


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('--extract', type=Path, help='new directory under x9/out/')
    args = parser.parse_args()
    data = args.original.read_bytes()
    items = segments(data)
    if args.extract:
        directory = args.extract.resolve()
        directory.relative_to((ROOT / 'x9/out').resolve())
        if directory.exists():
            parser.error('extraction directory already exists; do not overwrite derived files')
        directory.mkdir(parents=True)
        for item in items:
            start = item['data_offset']
            (directory / f'segment-{item["id"]}.bin').write_bytes(data[start:start + item['bytes']])
    print(json.dumps({'partition_bytes': len(data), 'segments': items,
                      'runtime_handoff_verified': False}, indent=2))


if __name__ == '__main__':
    main()
