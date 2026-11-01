#!/usr/bin/env python3
"""Decode X9 PIXEL serial records without filling in missing or damaged rows.

Usage: python3 -I x9/tools/decode_render_pixels.py USB.log --output DIRECTORY
RGB PNGs show the captured color bytes; separate alpha PNGs and packed BGRA
grids preserve alpha. Large-image grids are nearest samples, not full frames.
"""
import argparse
import json
import re
import struct
import zlib
from pathlib import Path


def png(path, width, height, data, channels):
    def chunk(kind, payload):
        return (struct.pack('>I', len(payload)) + kind + payload
                + struct.pack('>I', zlib.crc32(kind + payload) & 0xffffffff))

    stride = width * channels
    rows = b''.join(b'\0' + data[y * stride:(y + 1) * stride] for y in range(height))
    header = struct.pack('>IIBBBBB', width, height, 8, 2 if channels == 3 else 0, 0, 0, 0)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header)
                     + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('log', type=Path)
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args()
    images = {}
    copies = {}
    draws = {}

    def entry(pid, cb, image):
        return images.setdefault((int(pid), int(cb), int(image)),
                                 {'rows': {}, 'errors': [], 'ended': False})

    for line in args.log.read_text(errors='replace').splitlines():
        m = re.search(r'X9 GRAPH DRAW: id=(\d+) cb=(\d+) encoder=(\d+) '
                      r'vs=(\S+) fs=(\S+) type=(\d+) count=(\d+)', line)
        if m:
            ident, cb, encoder = map(int, m.group(1, 2, 3))
            draws[(encoder >> 32, cb, ident)] = {
                'id': ident, 'encoder': encoder, 'vs': m[4], 'fs': m[5],
                'type': int(m[6]), 'count': int(m[7]),
            }
        m = re.search(r'X9 PIXEL COPY: window=(\d+) cb=(\d+) image=(\d+) '
                      r'encoder=(\d+) draws=(\d+),(\d+) tex=(\d+) role=(\S+) '
                      r'size=(\d+)x(\d+) row=(\d+)', line)
        if m:
            window, cb, image, encoder, first, last, tex = map(int, m.group(1, 2, 3, 4, 5, 6, 7))
            copies[(encoder >> 32, cb, image)] = {
                'window': window, 'encoder': encoder, 'first_draw': first,
                'last_draw': last, 'texture': tex, 'role': m[8],
                'width': int(m[9]), 'height': int(m[10]), 'buffer_row_bytes': int(m[11]),
            }
        m = re.search(r'X9 PIXEL DATA: pid=(\d+) window=(\d+) cb=(\d+) image=(\d+) '
                      r'tex=(\d+) role=(\S+) size=(\d+)x(\d+) grid=(\d+)x(\d+) '
                      r'fnv=([0-9a-fA-F]{8}) order=BGRA8', line)
        if m:
            item = entry(m[1], m[3], m[4])
            if 'grid_width' in item:
                item['errors'].append('duplicate metadata')
            item.update(window=int(m[2]), texture=int(m[5]), role=m[6],
                        width=int(m[7]), height=int(m[8]), grid_width=int(m[9]),
                        grid_height=int(m[10]), full_image_fnv=m[11])
        m = re.search(r'X9 PIXEL ROW: pid=(\d+) cb=(\d+) image=(\d+) '
                      r'y=(\d+) x=(\d+) hex=([0-9a-fA-F]+)', line)
        if m:
            item = entry(m[1], m[2], m[3])
            key = (int(m[4]), int(m[5]))
            try:
                data = bytes.fromhex(m[6])
            except ValueError:
                item['errors'].append(f'invalid hex at {key}')
                continue
            if key in item['rows'] and item['rows'][key] != data:
                item['errors'].append(f'conflicting chunk at {key}')
            item['rows'][key] = data
        m = re.search(r'X9 PIXEL END: pid=(\d+) cb=(\d+) image=(\d+)', line)
        if m:
            entry(*m.group(1, 2, 3))['ended'] = True

    args.output.mkdir(parents=True, exist_ok=True)
    manifest = []
    for key in sorted(images.keys() | copies.keys()):
        item = images.get(key, {'rows': {}, 'errors': [], 'ended': False})
        pid, cb, image = key
        item.update(pid=pid, cb=cb, image=image, copy=copies.get(key))
        rows = item.pop('rows')
        w, h = item.get('grid_width', 0), item.get('grid_height', 0)
        if not (1 <= w <= 128 and 1 <= h <= 128):
            item['errors'].append('missing or invalid grid dimensions')
        elif not item['ended']:
            item['errors'].append('missing END record')
        else:
            packed = bytearray()
            expected = {(y, x) for y in range(h) for x in range(0, w, 8)}
            if rows.keys() != expected:
                item['errors'].append('missing or unexpected chunks')
            for y, x in sorted(expected):
                part = rows.get((y, x), b'')
                if len(part) != min(8, w - x) * 4:
                    item['errors'].append(f'wrong chunk length at {(y, x)}')
                packed.extend(part)
            if not item['errors']:
                stem = f'pid{pid}-cb{cb}-image{image}-tex{item["texture"]}'
                (args.output / f'{stem}.bgra').write_bytes(packed)
                rgb = b''.join(bytes((packed[i + 2], packed[i + 1], packed[i]))
                               for i in range(0, len(packed), 4))
                png(args.output / f'{stem}.png', w, h, rgb, 3)
                png(args.output / f'{stem}-alpha.png', w, h, bytes(packed[3::4]), 1)
                item['color_png'] = f'{stem}.png'
                item['alpha_png'] = f'{stem}-alpha.png'
                item['bgra_grid'] = f'{stem}.bgra'
        if item['copy']:
            c = item['copy']
            item['draws'] = [d for (p, b, _), d in sorted(draws.items())
                             if p == pid and b == cb and d['encoder'] == c['encoder']
                             and c['first_draw'] <= d['id'] <= c['last_draw']]
        item['complete'] = not item['errors']
        # FNV covers the full source image, excluding pitch padding; a sampled
        # grid cannot independently verify that checksum. It is kept as metadata.
        manifest.append(item)
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'images': len(manifest), 'complete': sum(i['complete'] for i in manifest),
                      'output': str(args.output)}))


if __name__ == '__main__':
    main()
