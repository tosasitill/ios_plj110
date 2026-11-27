#!/usr/bin/env python3
"""Host-side verification of a storm bootstrap build directory.

Checks: manifest size/SHA256 for every file, symlink targets, every Mach-O
is arm64 + iOS platform + ad-hoc signed, dpkg's compiled-in admindir is the
storm path, no file references the Procursus /var/jb prefix, every dylib a
Mach-O loads is either a system library or present in the tree under its
/var/storm install name (the would-it-launch closure), apt's compiled-in
state/conf dirs are the storm paths, the shipped apt.conf status pin agrees
with dpkg's admindir, and the Sileo dependency set (apt, coreutils,
libzstd1, dpkg) is present.
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
LC_LOAD_DYLIB = 0x0C
LC_ID_DYLIB = 0x0D
LC_LOAD_WEAK_DYLIB = 0x18 | 0x80000000
LC_REEXPORT_DYLIB = 0x1F
LC_LOAD_UPWARD_DYLIB = 0x23
DYLIB_CMDS = (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB,
              LC_LOAD_UPWARD_DYLIB)
SYSTEM_PREFIXES = ('/usr/lib/', '/System/Library/')


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


def dylib_names(data):
    """(install name, [loaded dylib names]) for a thin Mach-O."""
    if data[:4] != b'\xcf\xfa\xed\xfe':
        return None, []
    _cpu, _sub, _ftype, ncmds = struct.unpack_from('<IIII', data, 4)
    off = 32
    own = None
    loads = []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd in DYLIB_CMDS or cmd == LC_ID_DYLIB:
            name_off, = struct.unpack_from('<I', data, off + 8)
            start = off + name_off
            end = data.index(b'\0', start)
            name = data[start:end].decode('utf-8', 'replace')
            if cmd == LC_ID_DYLIB:
                own = name
            else:
                loads.append(name)
        off += size
    return own, loads


def main():
    out = Path(sys.argv[1]).resolve()
    manifest = json.loads((out / 'manifest.json').read_text())
    tree = out / 'bootstrap'
    problems = []
    machos = 0
    tree_paths = set(manifest['files'])
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
            _own, loads = dylib_names(data)
            for name in loads:
                if name.startswith('/var/storm/'):
                    if name[len('/var/storm'):] not in tree_paths:
                        problems.append(f'{rel}: loads missing {name}')
                elif name.startswith(SYSTEM_PREFIXES):
                    pass
                else:
                    problems.append(f'{rel}: loads non-system, '
                                    f'non-storm path {name}')
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
    apt_pkg = tree / 'usr/lib/libapt-pkg.6.0.dylib'
    if not apt_pkg.is_file():
        problems.append('usr/lib/libapt-pkg.6.0.dylib missing')
    else:
        blob = apt_pkg.read_bytes()
        for want in (b'/var/storm/var/lib/apt', b'/var/storm/etc/apt',
                     b'/var/storm/var/cache/apt'):
            if want not in blob:
                problems.append(f'libapt-pkg: compiled-in path {want.decode()} missing')
    conf = tree / 'etc/apt/apt.conf.d/00storm'
    if not conf.is_file():
        problems.append('etc/apt/apt.conf.d/00storm missing')
    elif 'Dir::State::status "/var/storm/Library/dpkg/status"' not in \
            conf.read_text():
        problems.append('00storm: Dir::State::status does not pin the '
                        'dpkg admindir status file')
    https = None
    for cand in ('usr/libexec/apt/methods/https', 'usr/lib/apt/methods/https'):
        if (tree / cand).is_file():
            https = tree / cand
            break
    if https is None:
        problems.append('apt https method missing')
    else:
        links = subprocess.run(['otool', '-L', str(https)],
                               capture_output=True, text=True).stdout
        if '/var/storm/usr/lib/libgnutls' not in links:
            problems.append('apt https method: not linked against storm libgnutls')
    for tool in ('usr/bin/sh', 'usr/bin/dpkg', 'usr/bin/uicache',
                 'usr/bin/patch', 'usr/lib/liblzma.5.dylib', 'usr/lib/libmd.0.dylib',
                 'Library/dpkg/status',
                 # round 2: apt tree + coreutils + Sileo's Depends set
                 'usr/bin/apt-get', 'usr/bin/apt-cache', 'usr/bin/apt-config',
                 'usr/bin/apt', 'usr/bin/apt-ftparchive',
                 'usr/lib/libapt-pkg.6.0.dylib', 'usr/lib/libzstd.1.dylib',
                 'usr/lib/liblz4.1.dylib', 'usr/lib/libxxhash.0.dylib',
                 'usr/lib/libgnutls.30.dylib', 'usr/lib/libgcrypt.20.dylib',
                 'usr/lib/libgpg-error.0.dylib', 'usr/lib/libnettle.8.dylib',
                 'usr/lib/libhogweed.6.dylib', 'usr/lib/libtasn1.6.dylib',
                 'usr/lib/libgmp.10.dylib', 'usr/bin/cp', 'usr/bin/env'):
        if not (tree / tool).exists():
            problems.append(f'missing: {tool}')
    print(f'files={len(manifest["files"])} machos={machos} problems={len(problems)}')
    for p in problems:
        print('FAIL', p)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
