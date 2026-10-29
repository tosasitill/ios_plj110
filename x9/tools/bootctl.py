#!/usr/bin/env python3
"""Prepare an A/B one-try control block OFFLINE, from a caller-supplied snapshot.

Does not read or write the phone. Keeps the other slot marked successful and
refuses a snapshot with no established successful fallback. Never invent an
AOSP boot_control block for a device whose existing layout is different.
"""
import argparse
from pathlib import Path
import struct
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from shared import load
control = load('x9_shared_bootctl', 'x676c/tools/bootctl_block.py')


def prepare(blob, slot):
    if slot not in ('a', 'b'):
        raise ValueError('slot must be a or b')
    if len(blob) != 32 or struct.unpack_from('<I', blob, 4)[0] != control.MAGIC:
        raise ValueError('not a 32-byte AOSP bootloader_control snapshot')
    if blob[8] != 1 or blob[9] & 7 != 2 or not control.crc_ok(blob):
        raise ValueError('unsupported version/slot count or invalid boot_control CRC')
    target = 'ab'.index(slot)
    fallback = control.parse(blob)[1 - target]
    if not fallback['priority'] or not fallback['successful'] or blob[13 + 2 * (1 - target)] & 1:
        raise ValueError('the other slot must already be a successful, non-corrupt fallback')
    prepared = bytearray(control.set_active(blob, slot, tries=1))
    # AOSP setActiveBootSlot clears a stale verity_corrupted flag for the new
    # candidate; retain the other slot and all reserved bits unchanged.
    prepared[13 + 2 * target] &= ~1
    struct.pack_into('<I', prepared, 28, zlib.crc32(prepared[:28]))
    return bytes(prepared)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('snapshot', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--slot', choices=['a', 'b'], default='a')
    a = ap.parse_args()
    blob = prepare(a.snapshot.read_bytes(), a.slot)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(blob)
    print(f'{a.out}: slot {a.slot}, 1 try; offline artifact only')


if __name__ == '__main__':
    main()
