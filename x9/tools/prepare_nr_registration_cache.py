#!/usr/bin/env python3
"""Prepare an OFFLINE 20H392 arm64 NR-registration-gate experiment.

Never installs anything or edits the source. Output must be a new regular file.
This is NOT a complete SA enablement patch: policy, NR signal and data calls
still require implementation/measurement. It preserves the D22 hardware enum.
The affected ctor case is shared by hardware configs 5..10 (D22 uses 7).
Code signatures are not regenerated; loading on the target is unverified.
"""
import argparse
import json
import mmap
import os
from pathlib import Path
import shutil
import struct
import uuid


IMAGE = 0x1F031E000  # libCommCenterBase.dylib, unslid 20H392
UUID = "64cd5d55-eec1-3ee0-a697-cc0ecd0ee021"
SITE = 0x1F036D1B0
BEFORE = bytes.fromhex("290080526982043969ba073969fa0739e9000014")
# mov w9,#0x101; strh w9,[x19,#0x120]
# Preserve +0x120=1, add +0x121=1. The two following STRB still write 1.
AFTER = bytes.fromhex("2920805269420279") + BEFORE[8:]


def prepare(source, output):
    if not source.is_file() or output.exists() or output.is_symlink():
        raise ValueError("source must be a file; output must not exist")
    with source.open("rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if data[:16].rstrip(b" \0") != b"dyld_v1   arm64":
            raise ValueError("expected a dyld arm64 cache component, not arm64e")
        table, count = struct.unpack_from("<II", data, 16)
        if not 0 < count <= 64 or table + count * 32 > len(data):
            raise ValueError("invalid cache mappings")
        mappings = [struct.unpack_from("<QQQ", data, table + i * 32) for i in range(count)]

        def offset(va, length):
            for base, size, off in mappings:
                if base <= va and va + length <= base + size and off + va - base + length <= len(data):
                    return off + va - base
            raise ValueError(f"VM address {va:#x} not present in this component")

        header = offset(IMAGE, 32)
        if struct.unpack_from("<I", data, header)[0] != 0xFEEDFACF:
            raise ValueError("expected Mach-O header at libCommCenterBase base")
        ncmds, cmdsize = struct.unpack_from("<II", data, header + 16)
        if not 0 < ncmds <= 128 or cmdsize > 65536:
            raise ValueError("invalid Mach-O commands")
        offset(IMAGE, 32 + cmdsize)
        pos, end, image_uuid = header + 32, header + 32 + cmdsize, None
        for _ in range(ncmds):
            if pos + 8 > end:
                raise ValueError("truncated load command")
            cmd, size = struct.unpack_from("<II", data, pos)
            if size < 8 or pos + size > end:
                raise ValueError("invalid load command size")
            if cmd == 0x1B and size == 24:
                image_uuid = str(uuid.UUID(bytes=data[pos + 8:pos + 24]))
            pos += size
        if image_uuid != UUID:
            raise ValueError(f"unsupported image UUID {image_uuid}")
        where = offset(SITE, len(BEFORE))
        if data[where:where + len(BEFORE)] != BEFORE:
            raise ValueError("ctor instructions differ; refuse stale/already patched input")
        # Guard the consumer as well: ldrb w27,[x0,#0x121].
        consumer = offset(0x1F191CE0C, 4)
        if data[consumer:consumer + 4] != bytes.fromhex("1b854439"):
            raise ValueError("NR consumer instruction differs")

        metadata = {
            "purpose": "offline NR registration gate experiment, NOT full SA support",
            "image_uuid": UUID, "site_vm": hex(SITE), "file_offset": hex(where),
            "source": str(source.resolve()), "output": str(output.absolute()),
            "original_bytes": BEFORE[:8].hex(), "replacement_bytes": AFTER[:8].hex(),
            "hardware_configs_affected": [5, 6, 7, 8, 9, 10], "D22_hardware_config": 7,
            "signature_regenerated": False, "installed_or_booted": False,
        }
        # Exclusive create also rejects a racing symlink or preexisting output.
        fd = os.open(output, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            with os.fdopen(fd, "w+b") as out:
                f.seek(0)
                shutil.copyfileobj(f, out)
                out.seek(where)
                out.write(AFTER[:8])
                out.flush()
                os.fsync(out.fileno())
        except BaseException:
            output.unlink(missing_ok=True)
            raise
    print(json.dumps(metadata, indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", required=True, type=Path, help="original dyld_shared_cache_arm64.31")
    ap.add_argument("--output", required=True, type=Path, help="new offline copy; never a device/root path")
    args = ap.parse_args()
    prepare(args.source, args.output)


if __name__ == "__main__":
    main()
