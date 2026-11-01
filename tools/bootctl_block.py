#!/usr/bin/env python3
"""Build the AOSP bootloader_control block (misc + 2048, 32 bytes).

usage: bootctl_block.py check <hex32>       verify the CRC of a dumped block
       bootctl_block.py set-active a|b <hex32> <out.bin> [tries]
Mirrors libboot_control setActiveBootSlot: the target slot gets priority 15,
7 tries (or [tries], 1..7) and successful=0; any other slot at priority 15
drops to 14. With 1 try a failing slot falls back after a single boot.
"""
import struct
import sys
import zlib

MAGIC = 0x42414342      # "BCAB"


def parse(blob):
    assert len(blob) == 32 and struct.unpack_from("<I", blob, 4)[0] == MAGIC
    slots = []
    for i in range(4):
        b = blob[12 + 2 * i]
        slots.append({"priority": b & 15, "tries": b >> 4 & 7,
                      "successful": b >> 7})
    return slots


def crc_ok(blob):
    return zlib.crc32(blob[:28]) == struct.unpack_from("<I", blob, 28)[0]


def set_active(blob, slot, tries=7):
    assert 1 <= tries <= 7
    out = bytearray(blob)
    idx = "ab".index(slot)
    out[0:4] = b"_" + slot.encode() + b"\0\0"
    for i in range(2):
        b = out[12 + 2 * i]
        if i == idx:
            b = 15 | tries << 4       # priority 15, not successful
        elif b & 15 == 15:
            b = b & ~15 | 14
        out[12 + 2 * i] = b
    struct.pack_into("<I", out, 28, zlib.crc32(bytes(out[:28])))
    return bytes(out)


def main():
    cmd = sys.argv[1]
    if cmd == "check":
        blob = bytes.fromhex(sys.argv[2])
        print("crc ok" if crc_ok(blob) else "CRC MISMATCH", parse(blob)[:2])
    elif cmd == "set-active":
        blob = bytes.fromhex(sys.argv[3])
        assert crc_ok(blob), "input block CRC mismatch"
        tries = int(sys.argv[5]) if len(sys.argv) > 5 else 7
        new = set_active(blob, sys.argv[2], tries)
        open(sys.argv[4], "wb").write(new)
        print(new.hex(" "), parse(new)[:2])


if __name__ == "__main__":
    main()
