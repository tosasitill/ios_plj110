#!/usr/bin/env python3
"""Decode intact pan50 inherited-LK records; never repair damaged evidence."""
import argparse
import binascii
import json
from pathlib import Path
import re

PATTERN = re.compile(rb'(?<![A-Za-z0-9])(lk50:([0-3]):([0-9a-f]{16}):([0-9a-f]{8})):([0-9a-f]{8})(?![0-9a-f])')
PHASES = ('inherited', 'before_submit', 'after_submit', 'final_or_decision')
DECISIONS = {1: 'matching_mutex_count', 2: 'scanout_count', 3: 'layers_valid',
             4: 'mutex_valid', 5: 'mutex_id', 6: 'mutex_bank_mask', 7: 'submissions'}


def analyze(files):
    accepted, rejected = [], []
    for filename, data in files:
        for m in PATTERN.finditer(data):
            prefix, phase, address, value, crc = m.groups()
            row = {'file': str(filename), 'byte_offset': m.start(), 'record': m.group().decode()}
            if binascii.crc32(prefix) != int(crc, 16):
                rejected.append(dict(row, reason='CRC mismatch'))
                continue
            key = int(address, 16)
            row.update(phase=PHASES[int(phase)], address=f'0x{key:016x}', value=f'0x{int(value,16):08x}')
            if key < 0x10000:
                row['decision'] = DECISIONS.get(key, 'unknown')
            accepted.append(row)
    return {'samples': accepted, 'rejected': rejected,
            'integrity_note': 'Only intact CRC records. Copies remain separate. No field reconstruction. '
                              'Frame submissions or inherited status flags do not prove frame completion or visible scanout.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    args = parser.parse_args()
    result = analyze((str(p), p.read_bytes()) for p in args.logs)
    print(json.dumps(result, indent=2))
    return 0 if result['samples'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
