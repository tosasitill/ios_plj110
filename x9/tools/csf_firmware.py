#!/usr/bin/env python3
"""Decode stock CSF 0.3 firmware and generate bounded kernel build inputs.
Format source: OPPO a16w csf/mali_kbase_csf_firmware.c and csf_defs.h.
Does not execute firmware, access a device or run a test suite.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

MAX_BYTES = 4 << 20
MAX_ENTRIES = 128
MAX_REGIONS = 32
SUPPORTED_FLAGS = 0xC000003F


def decode(blob):
    if not 20 <= len(blob) <= MAX_BYTES:
        raise ValueError('firmware length outside supported bounds')
    if struct.unpack_from('<I', blob)[0] != 0xC3F13A6E or blob[4:6] != b'\x03\x00':
        raise ValueError('expected stock CSF firmware header 0.3')
    end = struct.unpack_from('<I', blob, 16)[0]
    if end < 20 or end > len(blob) or end % 4:
        raise ValueError('entry table outside firmware')
    result = dict(bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest(),
                  header_version='0.3', version_hash=hex(struct.unpack_from('<I', blob, 8)[0]),
                  entries_end=end, entries=[], regions=[])
    at = 20
    while at < end:
        if end-at < 4 or len(result['entries']) == MAX_ENTRIES:
            raise ValueError('truncated or oversized entry table')
        header = struct.unpack_from('<I', blob, at)[0]
        typ, length = header & 255, header >> 8 & 255
        if length < 4 or length % 4 or length > end-at:
            raise ValueError('invalid entry length')
        row = dict(offset=at, type=typ, size=length, optional=bool(header >> 31),
                   updatable=bool(header & (1 << 30)))
        if typ == 0:
            if length < 28 or len(result['regions']) == MAX_REGIONS:
                raise ValueError('invalid memory interface entry')
            flags, va, stop, start, finish = struct.unpack_from('<5I', blob, at+4)
            if flags & ~SUPPORTED_FLAGS or not flags & 3:
                raise ValueError('unsupported memory permissions')
            if va >= stop or (va | stop) & 4095 or stop-va > (4 << 20):
                raise ValueError('invalid GPU virtual range')
            if finish < start or finish > len(blob) or finish-start > stop-va:
                raise ValueError('invalid firmware data range')
            name_end = blob.find(b'\0', at+24, at+length)
            if name_end < 0 or at+length-name_end-1 < 4:
                raise ValueError('missing name terminator or execution address')
            name = blob[at+24:name_end].decode('utf-8', errors='strict')
            region = dict(flags=flags, virtual_start=va, virtual_end=stop,
                          data_start=start, data_end=finish, name=name,
                          virtual_exe_start=struct.unpack_from('<I', blob, name_end+1)[0],
                          protected=bool(flags & 32), shared=bool(flags & (1 << 30)),
                          zero=bool(flags & (1 << 31)), executable=bool(flags & 4),
                          gpu_pages=(stop-va)//4096, host_pages=(stop-va+16383)//16384)
            if any(va < r['virtual_end'] and r['virtual_start'] < stop for r in result['regions']):
                raise ValueError('overlapping firmware virtual regions')
            result['regions'].append(region)
            row['region'] = len(result['regions'])-1
        elif typ == 6:
            if length < 12:
                raise ValueError('invalid build info record')
            start, size = struct.unpack_from('<2I', blob, at+4)
            if start > len(blob) or size > len(blob)-start:
                raise ValueError('build info outside firmware')
            row['build_info'] = blob[start:start+size].rstrip(b'\0').decode('utf-8', errors='replace')
        elif typ not in (1, 3, 4, 7, 8, 9) and not row['optional']:
            raise ValueError(f'unknown required entry type {typ}')
        result['entries'].append(row)
        at += length
    if at != end:
        raise ValueError('entry table has trailing bytes')
    result['mapped_bytes'] = sum(r['virtual_end']-r['virtual_start'] for r in result['regions'] if not r['protected'])
    if not result['mapped_bytes'] or result['mapped_bytes'] > (16 << 20):
        raise ValueError('firmware image exceeds preparation budget')
    shared = [r for r in result['regions'] if r['shared'] and not r['protected']]
    if len(shared) != 1 or shared[0]['virtual_start'] != 0x04000000:
        raise ValueError('unsupported CSF shared interface layout')
    return result


def generate(source, output_dir):
    blob = Path(source).read_bytes()
    result = decode(blob)
    dest = Path(output_dir)
    dest.mkdir(parents=True, exist_ok=True)
    (dest/'firmware-layout.json').write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n')
    with (dest/'x9_csf_firmware.inc').open('w') as f:
        f.write('/* Generated from the frozen, unmodified stock firmware build input. */\n')
        f.write('static const char csf_firmware_sha256[] = "'+result['sha256']+'";\n')
        f.write('static const unsigned char csf_firmware_bytes[] = {\n')
        for start in range(0, len(blob), 24):
            f.write(' '+','.join('0x%02x' % v for v in blob[start:start+24])+',\n')
        f.write('};\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    data = generate(args.firmware, args.out)
    print(json.dumps({k: data[k] for k in ('bytes', 'sha256', 'header_version', 'version_hash', 'mapped_bytes')}))
