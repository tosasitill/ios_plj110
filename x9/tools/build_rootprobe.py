#!/usr/bin/env python3
"""Build an owned static PID1 and a bounded raw HFS+ md0 root (host-only).
Does not contain iOS binaries. No phone access or partition operations.
"""
import argparse
from pathlib import Path
import struct
import subprocess

CLANG = '/opt/homebrew/opt/llvm/bin/clang'
LD = '/Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/ld-classic'


def macho_contract(blob):
    if len(blob) < 32:
        raise ValueError('truncated probe Mach-O')
    magic, cpu, subtype, kind, count, size, flags, reserved = struct.unpack_from('<8I', blob)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 2):
        raise ValueError('probe must be a plain arm64 executable')
    off = 32
    thread = signature = False
    for _ in range(count):
        cmd, n = struct.unpack_from('<II', blob, off)
        if n < 8 or off + n > 32 + size or off + n > len(blob):
            raise ValueError('invalid probe load command')
        if cmd in (0xe, 0xc, 0x80000018, 0x8000001f):
            raise ValueError('probe must not depend on dyld or dynamic libraries')
        if cmd == 5:
            if n < 288 or struct.unpack_from('<II', blob, off + 8) != (6, 68):
                raise ValueError('invalid ARM64 thread state')
            if struct.unpack_from('<Q', blob, off + 16 + 32 * 8)[0] & 3:
                raise ValueError('unaligned PID1 entry')
            thread = True
        signature |= cmd == 0x1d
        off += n
    if off != 32 + size or not thread or not signature:
        raise ValueError('probe needs LC_UNIXTHREAD entry and code signature')


def raw_hfs_volume(blob, max_size=64 * 1024 * 1024):
    if len(blob) < 1536 or blob[1024:1026] not in (b'H+', b'HX'):
        raise ValueError('expected partitionless HFS+ volume')
    block, total = struct.unpack_from('>II', blob, 1024 + 40)
    size = block * total
    if block < 512 or block & (block - 1) or not 8 * 1024 * 1024 <= size <= max_size or size > len(blob):
        raise ValueError('invalid or oversized HFS geometry')
    # Exclude any UDIF trailer; only the filesystem goes into md0.
    return blob[:size]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image = out / 'root.hfs'
    if image.exists():
        raise ValueError('refusing to overwrite existing RAM root')
    root = out / 'root-tree'
    executable = root / 'usr/appleinternal/sbin/launchd.x9probe'
    executable.parent.mkdir(parents=True, exist_ok=True)
    (root / 'dev').mkdir(exist_ok=True)
    (root / 'private/tmp').mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parent / 'stub/rootprobe.S'
    obj = out / 'rootprobe.o'
    subprocess.run([CLANG, '-target', 'arm64-apple-macos13.0', '-c', str(source), '-o', str(obj)], check=True)
    subprocess.run([LD, '-static', '-arch', 'arm64', '-platform_version', 'macos', '13.0', '13.0',
                    '-e', '_start', '-no_pie', '-o', str(executable), str(obj)], check=True)
    executable.chmod(0o755)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(executable)], check=True)
    macho_contract(executable.read_bytes())
    dmg = out / 'root-container.dmg'
    subprocess.run(['/usr/bin/hdiutil', 'create', '-size', '64m', '-srcfolder', str(root),
                    '-fs', 'Case-sensitive HFS+', '-layout', 'NONE', '-format', 'UDRW',
                    '-volname', 'X9RAMProbe', '-nospotlight', str(dmg)], check=True)
    image.write_bytes(raw_hfs_volume(dmg.read_bytes()))
    print(f'RAM root built: {image}; {image.stat().st_size} bytes; owned PID1 only', flush=True)


if __name__ == '__main__':
    main()
