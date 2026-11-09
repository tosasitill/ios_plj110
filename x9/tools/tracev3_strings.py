#!/usr/bin/env python3
"""Dump printable strings from the LZ4 chunksets of iOS .tracev3 files.

Not a unified-log decoder: format strings live in uuidtext/dsc and are not
joined here. Inline arguments (%s, %@ descriptions, oversize strings) are
stored in the chunk data, so this is enough to search for reasons that a
process logged as string arguments. Read-only on local copies.

  tracev3_strings.py FILE.tracev3 [...] [--min 6] > strings.txt
"""
import argparse, re, struct, sys

import lz4.block


def chunksets(data):
    pos = 0
    while pos + 16 <= len(data):
        tag, sub, size = struct.unpack_from('<IIQ', data, pos)
        body = data[pos + 16:pos + 16 + size]
        if tag == 0x600d:
            yield pos, body
        pos += 16 + size
        pos = (pos + 7) & ~7


def decompress(body):
    out, p = bytearray(), 0
    while p + 4 <= len(body):
        magic = body[p:p + 4]
        if magic == b'bv4$':
            break
        if magic == b'bv41':
            raw, comp = struct.unpack_from('<II', body, p + 4)
            out += lz4.block.decompress(body[p + 12:p + 12 + comp], uncompressed_size=raw)
            p += 12 + comp
        elif magic == b'bv4-':
            raw, = struct.unpack_from('<I', body, p + 4)
            out += body[p + 8:p + 8 + raw]
            p += 8 + raw
        else:
            break
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+')
    ap.add_argument('--min', type=int, default=6)
    a = ap.parse_args()
    rx = re.compile(rb'[\x20-\x7e]{%d,}' % a.min)
    for f in a.files:
        data = open(f, 'rb').read()
        for off, body in chunksets(data):
            try:
                plain = decompress(body)
            except Exception as e:  # keep going on a damaged chunkset
                print('%s@%#x: decompress failed: %s' % (f, off, e), file=sys.stderr)
                continue
            for m in rx.finditer(plain):
                print('%s@%#x+%#x\t%s' % (f.rsplit('/', 1)[-1], off, m.start(), m.group().decode()))


if __name__ == '__main__':
    main()
