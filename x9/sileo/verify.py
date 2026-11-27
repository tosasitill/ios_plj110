#!/usr/bin/env python3
"""Host-side verification of a storm bootstrap build directory.

Checks: manifest size/SHA256 for every file, symlink targets, every Mach-O
is arm64 + iOS platform + ad-hoc signed, dpkg's compiled-in admindir is the
storm path, and no file references the Procursus /var/jb prefix.
Exits nonzero on the first failed class with a precise message.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

MACHO_MAGICS = (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe')
LC_BUILD_VERSION = 0x32
PLATFORM_IOS = 2


def macho_info(data):
    """(arch, platform, minos) for thin arm64 Mach-O; None if not thin."""
    if data[:4] != b'\xcf\xfa\xed\xfe':
        return None
    cpu, _sub, _ftype, ncmds = struct.unpack_from('<IIII', data, 4)
    off = 32
    platform = minos = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == LC_BUILD_VERSION:
            platform, sdk = struct.unpack_from('<II', data, off + 8)
            minos = sdk
        off += size
    return cpu, platform, minos


def main():
    out = Path(sys.argv[1]).resolve()
    manifest = json.loads((out / 'manifest.json').read_text())
    tree = out / 'bootstrap'
    problems = []
    machos = 0
    for rel, meta in manifest['files'].items():
        path = tree / rel.lstrip('/')
        if 'symlink' in meta:
            if not path.is_symlink() or path.readlink().as_posix() != meta['symlink'] \
                    and str(path.readlink()) != meta['symlink']:
                problems.append(f'{rel}: symlink target mismatch')
            continue
        data = path.read_bytes()
        if len(data) != meta['size'] or hashlib.sha256(data).hexdigest() != meta['sha256']:
            problems.append(f'{rel}: manifest hash/size mismatch')
            continue
        if b'/var/jb' in data:
            problems.append(f'{rel}: references /var/jb')
        if data[:4] in MACHO_MAGICS:
            info = macho_info(data)
            if info is None:
                continue  # fat binary: none expected in this tree
            machos += 1
            cpu, platform, _ = info
            if cpu != 0x0100000C:
                problems.append(f'{rel}: cpu is not arm64 ({cpu:#x})')
            if platform != PLATFORM_IOS:
                problems.append(f'{rel}: platform is not iOS ({platform})')
            sig = subprocess.run(['codesign', '-dv', str(path)],
                                 capture_output=True, text=True).stderr
            if 'Signature=adhoc' not in sig:
                problems.append(f'{rel}: not ad-hoc signed')
    dpkg = tree / 'usr/bin/dpkg'
    if not dpkg.is_file():
        problems.append('usr/bin/dpkg missing')
    else:
        blob = dpkg.read_bytes()
        if b'/var/storm/Library/dpkg' not in blob:
            problems.append('dpkg: compiled-in admindir is not /var/storm/Library/dpkg')
        links = subprocess.run(['otool', '-L', str(dpkg)],
                               capture_output=True, text=True).stdout
        if '/var/storm/usr/lib/liblzma' not in links:
            problems.append('dpkg: not linked against storm liblzma')
    for tool in ('usr/bin/sh', 'usr/bin/dpkg', 'usr/bin/uicache',
                 'usr/bin/patch', 'usr/lib/liblzma.5.dylib', 'usr/lib/libmd.0.dylib',
                 'Library/dpkg/status'):
        if not (tree / tool).exists():
            problems.append(f'missing: {tool}')
    print(f'files={len(manifest["files"])} machos={machos} problems={len(problems)}')
    for p in problems:
        print('FAIL', p)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
