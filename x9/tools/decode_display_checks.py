#!/usr/bin/env python3
"""Decode pan45 kA0..kP1 value/complement records without repairing corruption.

These tags use +off as an encoding, NOT as a register offset. Records with
damaged tags/delimiters are not guessed. Complement validation detects many
errors, but cannot certify against identical errors in both halves.
"""
import argparse
import json
from pathlib import Path
import re
import sys

ADDRESSES = (
    0x3e300ee4, 0x3e300f34, 0x32900aac, 0x32900ab8,
    0x32900ab0, 0x32900abc, 0x3e300a3c, 0x3e300a4c,
    0x3e300a44, 0x3e300a54, 0x3e700a3c, 0x3e700a4c,
    0x3e730274, 0x3e730244, 0x32900bbc, 0x32960100,
)
NAMES = (
    'main.PQ_OUT_CB3', 'main.PQ_IN_CB0', 'OVL.VALID1', 'OVL.READY1',
    'OVL.VALID2', 'OVL.READY2', 'main.VALID1', 'main.READY1',
    'main.VALID3', 'main.READY3', 'side.VALID0', 'side.READY0',
    'DSI.FSM', 'DSI.position', 'OVL.DLO12.counter', 'EXDMA3.FIFO',
)
PATTERN = re.compile(
    r'(?<![A-Za-z0-9])(k[A-P][01])\+off=0x([0-9a-fA-F]{16})(?![0-9a-fA-F])')


def analyze(files):
    """files is an iterable of (source name, bytes); LF line numbers preserved."""
    accepted = {}
    rejected = []
    for filename, data in files:
        for number, line in enumerate(data.decode('ascii', errors='replace').split('\n'), 1):
            for match in PATTERN.finditer(line):
                tag, payload = match.groups()
                value, inverse = int(payload[:8], 16), int(payload[8:], 16)
                location = {'file': str(filename), 'LF_line': number, 'payload': payload}
                if (value ^ inverse) != 0xffffffff:
                    rejected.append(dict(location, tag=tag, reason='complement mismatch'))
                    continue
                by_value = accepted.setdefault(tag, {})
                by_value.setdefault(value, []).append(location)
    samples = []
    for tag, by_value in sorted(accepted.items()):
        index = ord(tag[1]) - ord('A')
        samples.append({
            'tag': tag, 'phase': 'pre_frames' if tag[2] == '0' else 'post_frames',
            'address': f'0x{ADDRESSES[index]:08x}', 'register': NAMES[index],
            'status': 'consistent' if len(by_value) == 1 else 'CONFLICT',
            'values': [{'value': f'0x{value:08x}', 'copies': len(locations),
                        'evidence': locations} for value, locations in sorted(by_value.items())],
        })
    return {'samples': samples, 'rejected': rejected,
            'integrity_note': 'No repair. Matching complements do not rule out equal corruption; '
                              'duplicate snapshots may also differ because hardware advances.'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--json', action='store_true', help='include evidence and rejected records')
    args = parser.parse_args(argv)
    result = analyze((str(path), path.read_bytes()) for path in args.logs)
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print('tag\tphase\taddress\tregister\tvalue(s)\tstatus\tcopies')
        for sample in result['samples']:
            values = sample['values']
            print('\t'.join((sample['tag'], sample['phase'], sample['address'],
                             sample['register'], ','.join(v['value'] for v in values),
                             sample['status'], ','.join(str(v['copies']) for v in values))))
    conflicts = sum(s['status'] == 'CONFLICT' for s in result['samples'])
    print(f"usable tags={len(result['samples'])}, conflicts={conflicts}, "
          f"rejected complete records={len(result['rejected'])}; no reconstruction", file=sys.stderr)
    return 2 if conflicts or not result['samples'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
