#!/usr/bin/env python3
"""Strict pan46 CRC records; no repair, voting or legacy tag reassignment."""
import argparse
import binascii
import json
from pathlib import Path
import re
import sys

ADDRESSES = (
    0x3e300ee4, 0x3e300f34, 0x32900aac, 0x32900ab8,
    0x32900ab0, 0x32900abc, 0x3e300a3c, 0x3e300a4c,
    0x3e300a44, 0x3e300a54, 0x3e700a3c, 0x3e700a4c,
    0x3e730274, 0x3e730244, 0x32900bbc, 0x32960100,
    0x3e320020, 0x3e320024, 0x3e320034, 0x3e320038,
    0x3e720020, 0x3e720024, 0x3e720034, 0x3e720038,
    0x32920020, 0x32920024, 0x32920034, 0x32920038,
    0x3e320008, 0x3e720008, 0x32920008, 0x3e73000c,
    0x32960240, 0x32a30108, 0x3e7f0008,
    None, None, None,
)
NAMES = (
    'main.PQ_OUT_CB3', 'main.PQ_IN_CB0', 'OVL.VALID1', 'OVL.READY1',
    'OVL.VALID2', 'OVL.READY2', 'main.VALID1', 'main.READY1',
    'main.VALID3', 'main.READY3', 'side.VALID0', 'side.READY0',
    'DSI.FSM', 'DSI.position', 'OVL.DLO12.status', 'EXDMA3.FIFO',
    'main.mutex.EN', 'main.mutex.ownership', 'main.mutex.MOD0', 'main.mutex.MOD1',
    'side.mutex.EN', 'side.mutex.ownership', 'side.mutex.MOD0', 'side.mutex.MOD1',
    'OVL.mutex.EN', 'OVL.mutex.ownership', 'OVL.mutex.MOD0', 'OVL.mutex.MOD1',
    'main.mutex.CFG', 'side.mutex.CFG', 'OVL.mutex.CFG', 'DSI.INTSTA',
    'EXDMA3.flow', 'OUTPROC0.flow', 'DSC0.INTSTA',
    'diagnostic.last_status', 'diagnostic.last_wait_ms', 'diagnostic.good_streak',
)
STAGES = {
    0: 'candidate baseline; common fresh-status instrumentation',
    1: 'release software ownership; cumulative',
    2: 'per-frame mutex CFG/EN sequence; cumulative',
    3: 'OVL MOD0 membership update; cumulative; inspect the recorded MOD0 value',
    4: 'strip software BIT31 from OVL/side MOD1; cumulative',
}
PATTERN = re.compile(
    rb'(?<![A-Za-z0-9])(q46([0-4])([01])([0-9a-fA-F]{2}):([0-9a-fA-F]{8}))'
    rb':([0-9a-fA-F]{8})(?![0-9a-fA-F])')


def analyze(files):
    accepted, rejected = {}, []
    for filename, data in files:
        for number, line in enumerate(data.split(b'\n'), 1):
            for match in PATTERN.finditer(line):
                prefix, stage, phase, index, value, stored_crc = match.groups()
                stage, phase, index = int(stage), int(phase), int(index, 16)
                location = {'file': str(filename), 'LF_line': number,
                            'record': match.group().decode('ascii')}
                if binascii.crc32(prefix) != int(stored_crc, 16):
                    rejected.append(dict(location, reason='CRC mismatch'))
                    continue
                if index >= len(ADDRESSES):
                    rejected.append(dict(location, reason='unknown index'))
                    continue
                by_value = accepted.setdefault((stage, phase, index), {})
                by_value.setdefault(int(value, 16), []).append(location)
    samples = []
    for (stage, phase, index), by_value in sorted(accepted.items()):
        samples.append({
            'stage': stage, 'stage_meaning': STAGES[stage],
            'phase': 'post_frames' if phase else 'pre_frames', 'index': index,
            'address': None if ADDRESSES[index] is None else f'0x{ADDRESSES[index]:08x}',
            'register': NAMES[index],
            'status': 'CONFLICT' if len(by_value) > 1 else 'consistent',
            'values': [{'value': f'0x{value:08x}', 'copies': len(locations),
                        'evidence': locations} for value, locations in sorted(by_value.items())],
        })
    return {'samples': samples, 'rejected': rejected,
            'integrity_note': 'CRC validates the recorded stage/phase/index/value, not glass '
                              'visibility or register semantics. No repair or majority voting. '
                              'Stages are cumulative, not independent reset experiments. '
                              'Metadata fields are stored results, not MMIO addresses.'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args(argv)
    result = analyze((str(p), p.read_bytes()) for p in args.logs)
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print('stage\tphase\tindex\taddress\tregister\tvalues\tstatus\tcopies')
        for sample in result['samples']:
            print('\t'.join((str(sample['stage']), sample['phase'], f"{sample['index']:02x}",
                             sample['address'] or 'metadata', sample['register'],
                             ','.join(v['value'] for v in sample['values']), sample['status'],
                             ','.join(str(v['copies']) for v in sample['values']))))
    conflicts = sum(s['status'] == 'CONFLICT' for s in result['samples'])
    print(f"usable fields={len(result['samples'])}, conflicts={conflicts}, "
          f"rejected records={len(result['rejected'])}; no reconstruction", file=sys.stderr)
    return 2 if conflicts or not result['samples'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
