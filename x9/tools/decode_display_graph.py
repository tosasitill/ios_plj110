#!/usr/bin/env python3
"""Strict q49 CRC records with separate MT6993 OVL handshake groups."""
import argparse
import binascii
import json
from pathlib import Path
import re

REGISTERS = (
    ('OVL.VALID0', 0x32900aa8), ('OVL.READY0', 0x32900ab4),
    ('OVL.VALID1', 0x32900aac), ('OVL.READY1', 0x32900ab8),
    ('OVL.VALID2', 0x32900ab0), ('OVL.READY2', 0x32900abc),
) + tuple((f'BL{i + 1}.L0_EN', 0x329c0040 + i * 0x10000) for i in range(6)) + tuple(
    (f'BL{i + 1}.DBG_STATUS5', 0x329c07b4 + i * 0x10000) for i in range(6)) + (
    ('OVL.mutex.MOD0', 0x32920034), ('OVL.mutex.MOD1', 0x32920038),
    ('side.mutex.MOD1', 0x3e720038), ('DSI.FSM', 0x3e730274),
    ('OVL.DLO12.status', 0x32900bbc),
)
SIGNALS = json.loads((Path(__file__).parent / 'ovl_signals_mt6993.json').read_text())
PATTERN = re.compile(rb'(?<![A-Za-z0-9])(q49([0-4])([01])([0-9a-f]{2}):([0-9a-f]{8})):([0-9a-f]{8})(?![0-9a-f])')


def set_signals(group, value):
    return [{'bit': bit, 'name': SIGNALS[str(group)].get(str(bit), 'unknown')}
            for bit in range(32) if value & (1 << bit)]


def analyze(files):
    samples, rejected = [], []
    for filename, data in files:
        for number, line in enumerate(data.split(b'\n'), 1):
            for match in PATTERN.finditer(line):
                prefix, stage, phase, index, value, crc = match.groups()
                item = {'file': str(filename), 'LF_line': number,
                        'record': match.group().decode('ascii')}
                if binascii.crc32(prefix) != int(crc, 16):
                    rejected.append(dict(item, reason='CRC mismatch'))
                    continue
                index, value = int(index, 16), int(value, 16)
                if index >= len(REGISTERS):
                    rejected.append(dict(item, reason='unknown register index'))
                    continue
                name, address = REGISTERS[index]
                item.update(stage=int(stage), phase='post_frames' if phase == b'1' else 'pre_frames',
                            register=name, address=f'0x{address:08x}', value=f'0x{value:08x}')
                if index < 6:
                    item['set_signals'] = set_signals(index // 2, value)
                samples.append(item)
    return {'samples': samples, 'rejected': rejected,
            'integrity_note': 'No reconstruction. Different handshake groups use different bit names. '
                              'Samples are instantaneous; CRC does not establish frame completion.'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('logs', nargs='+', type=Path)
    a = p.parse_args()
    result = analyze((str(f), f.read_bytes()) for f in a.logs)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result['samples'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
