#!/usr/bin/env python3
"""Strict CRC decoding for X9 kernel milestones and pan48 cascade selectors."""
import argparse
import binascii
import json
from pathlib import Path
import re

JOURNAL = re.compile(rb'(?<![A-Za-z0-9])(x9m:([0-9]{2}):([0-9a-f]{8})):([0-9a-f]{8})(?![0-9a-f])')
CASCADE = re.compile(rb'(?<![A-Za-z0-9])(q480([01])(0[0-5]):([0-9a-f]{8})):([0-9a-f]{8})(?![0-9a-f])')


def analyze(files):
    samples, rejected = [], []
    for name, data in files:
        for line_no, line in enumerate(data.split(b'\n'), 1):
            for kind, pattern in [('milestone', JOURNAL), ('cascade', CASCADE)]:
                for match in pattern.finditer(line):
                    prefix, *fields, crc = match.groups()
                    item = {'kind': kind, 'file': str(name), 'LF_line': line_no,
                            'record': match.group().decode('ascii')}
                    if binascii.crc32(prefix) != int(crc, 16):
                        rejected.append(dict(item, reason='CRC mismatch'))
                        continue
                    if kind == 'milestone':
                        stage, flags = fields
                        flags = int(flags, 16)
                        item.update(stage=int(stage), flags=f'0x{flags:08x}',
                                    console_mapped=bool(flags & 1),
                                    early_disabled=bool(flags & 2), board_x9=bool(flags & 4))
                    else:
                        phase, index, value = fields
                        i = int(index, 16)
                        item.update(phase='post_frames' if phase == b'1' else 'pre_frames',
                                    blender=i + 1, address=f'0x{0x329c0010 + i * 0x10000:08x}',
                                    value=f'0x{int(value, 16):08x}')
                    samples.append(item)
    return {'samples': samples, 'rejected': rejected,
            'integrity_note': 'No repair or voting. CRC validates captured fields, not scanout or completion.'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('logs', nargs='+', type=Path)
    a = p.parse_args()
    result = analyze((str(f), f.read_bytes()) for f in a.logs)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result['samples'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
