#!/usr/bin/env python3
"""Decode original btsnoop/H4 controller facts, without exposing peer data.

Does not replay packets, contact a controller or infer missing features.
Only successful, matching command completions are published. Unknown events,
pairing keys, addresses, names and connection traffic are omitted from output.
"""
import argparse
import json
import struct
from pathlib import Path


def facts(data):
    if len(data) < 16 or data[:8] != b'btsnoop\0':
        raise ValueError('not an original btsnoop capture')
    if struct.unpack_from('>II', data, 8) != (1, 1002):
        raise ValueError('expected btsnoop v1 with H4 datalink 1002')
    reads = {0x1001: 8, 0x1002: 64, 0x1003: 8, 0x1004: 10,
             0x1005: 7, 0x2003: 8, 0x203b: 1, 0x2060: 6}
    out = {'records': 0, 'dropped': 0, 'truncated': 0, 'reset_epochs': 0,
           'read_errors': [], 'controller': {}, 'extended_features': {}}
    pending = {}
    at = 16
    while at < len(data):
        if len(data) - at < 24:
            raise ValueError('partial btsnoop record header; recapture after file closes')
        full, size, flags, dropped, _ = struct.unpack_from('>IIIIQ', data, at)
        at += 24
        if size > full or size > 65540 or at + size > len(data):
            raise ValueError('invalid/partial btsnoop record payload')
        packet = data[at:at + size]
        at += size
        out['records'] += 1
        out['dropped'] = max(out['dropped'], dropped)
        if full != size:
            out['truncated'] += 1
            continue
        if not packet:
            raise ValueError('empty H4 packet')
        if packet[0] == 1:
            if len(packet) < 4 or len(packet) != packet[3] + 4 or flags & 1:
                raise ValueError('malformed/outbound-direction HCI command')
            opcode = int.from_bytes(packet[1:3], 'little')
            if opcode == 0x0c03:
                # Never combine capabilities from different reset epochs.
                pending.clear()
                out['controller'].clear()
                out['extended_features'].clear()
                out['read_errors'].clear()
                out['reset_epochs'] += 1
            if opcode in reads:
                if opcode in pending:
                    raise ValueError('overlapping controller read without completion')
                pending[opcode] = packet[4:]
            continue
        if packet[:2] != b'\x04\x0e':
            continue
        if len(packet) < 7 or len(packet) != packet[2] + 3 or not flags & 1:
            raise ValueError('malformed/inbound-direction Command Complete')
        opcode = int.from_bytes(packet[4:6], 'little')
        if opcode not in pending:
            continue
        request = pending.pop(opcode)
        status, value = packet[6], packet[7:]
        if status:
            out['read_errors'].append({'opcode': f'{opcode:04x}', 'status': status})
            continue
        if len(value) != reads[opcode]:
            raise ValueError(f'wrong successful controller reply length: {opcode:04x}')
        ctrl = out['controller']
        if opcode == 0x1001:
            ctrl['hci_version'] = value[0]
            ctrl['hci_revision'] = int.from_bytes(value[1:3], 'little')
            ctrl['lmp_version'] = value[3]
            ctrl['manufacturer'] = int.from_bytes(value[4:6], 'little')
            ctrl['lmp_subversion'] = int.from_bytes(value[6:8], 'little')
        elif opcode == 0x1002:
            ctrl['supported_commands_le_bytes'] = value.hex()
        elif opcode in (0x1003, 0x2003):
            key = 'local_features' if opcode == 0x1003 else 'le_features'
            ctrl[key + '_le_bytes'] = value.hex()
            ctrl[key + '_uint64'] = f'0x{int.from_bytes(value, "little"):016x}'
        elif opcode == 0x1004:
            if len(request) != 1 or value[0] != request[0] or value[0] > value[1]:
                raise ValueError('extended feature reply does not match requested page')
            out['extended_features'][str(value[0])] = {
                'max_page': value[1], 'le_bytes': value[2:].hex()}
        elif opcode == 0x203b:
            if value[0] > 0xf0:
                raise ValueError('invalid advertising set count')
            ctrl['advertising_sets'] = value[0]
        elif opcode == 0x1005:
            ctrl['br_edr_buffers'] = {'acl_bytes': int.from_bytes(value[:2], 'little'),
                'sco_bytes': value[2], 'acl_packets': int.from_bytes(value[3:5], 'little'),
                'sco_packets': int.from_bytes(value[5:7], 'little')}
        elif opcode == 0x2060:
            ctrl['le_iso_buffers'] = {'acl_bytes': int.from_bytes(value[:2], 'little'),
                'acl_packets': value[2], 'iso_bytes': int.from_bytes(value[3:5], 'little'),
                'iso_packets': value[5]}
    out['unfinished_reads'] = [f'{opcode:04x}' for opcode in pending]
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    args = parser.parse_args()
    print(json.dumps(facts(args.original.read_bytes()), indent=2))


if __name__ == '__main__':
    main()
