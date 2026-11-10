#!/usr/bin/env python3
"""Prepare an OFFLINE 20H392 arm64 D22-only dual-SIM-personality experiment.

Never installs anything or edits the source. Output must be a new regular file.
This changes only hardware enum 7 (D22) in the dual-slot capability predicate.
It does NOT return a fabricated CT capability: ActivationModel still requires
BSP DsdsAllowed, and SIM identity, registration, DDS and data remain separate.
Use only after validating the opt-in X9 dual-slot UIM/MIPC candidate on-device.
No NR gate changes are included; do not mix this with the separate NR candidate.
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
SITE = 0x1F036CF98
# Original: idx=hw-12; allowed=(uint8(idx)<45) & (mask >> idx).
BEFORE = bytes.fromhex(
    "093300512a1d00125fb50071ea279f1aebc091d2ebc0a3f24be0c3f2"
    "6925c99a4901090a69060939"
)
# New: idx=hw-7; allowed=(uint32(idx)<50) & (((mask<<5)|1) >> idx).
# Only hw=7 is added. Bits 1..4 are zero; hw=12..56 retain the old bits.
# Hardware is an int8 load: for all negative values both ranges reject it.
# Removing the now-unneeded AND #0xff leaves room for the fourth MOVK.
# x9/x10/x11 and NZCV are overwritten by the following original predicates.
AFTER = bytes.fromhex(
    "091f00513fc90071ea279f1a2b1c98d22b1eb8f26b08dcf26b00e0f2"
    "6925c99a4901090a69060939"
)
PATCH_BYTES = 28  # last three instructions, including STRB +0x241, stay intact


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
        # Do not silently compose this candidate with the separate NR patch.
        nr = offset(0x1F036D1B0, 8)
        if data[nr:nr + 8] != bytes.fromhex("2900805269820439"):
            raise ValueError("NR ctor site differs; use the pristine source component")

        metadata = {
            "purpose": "offline D22 dual-slot personality gate, NOT verified dual-SIM or data support",
            "image_uuid": UUID, "site_vm": hex(SITE), "file_offset": hex(where),
            "source": str(source.resolve()), "output": str(output.absolute()),
            "original_bytes": BEFORE[:PATCH_BYTES].hex(), "replacement_bytes": AFTER[:PATCH_BYTES].hex(),
            "hardware_configs_affected": [7], "D22_hardware_config": 7,
            "personality_field": "0x241", "BSP_DsdsAllowed_TLV": "0x53",
            "NR_gate_modified": False, "signature_regenerated": False, "installed_or_booted": False,
        }
        # Exclusive create also rejects a racing symlink or preexisting output.
        fd = os.open(output, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            with os.fdopen(fd, "w+b") as out:
                f.seek(0)
                shutil.copyfileobj(f, out)
                out.seek(where)
                out.write(AFTER[:PATCH_BYTES])
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
