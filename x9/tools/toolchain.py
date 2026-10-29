"""Small native LLVM cross-build helper; Zig is an optional fallback."""
from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CFLAGS = [
    '--target=aarch64-none-elf', '-march=armv8-a', '-O2',
    '-ffreestanding', '-nostdlib', '-fno-pic', '-fno-pie', '-fno-builtin',
    '-mgeneral-regs-only', '-mstrict-align', '-fno-stack-protector',
    '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-Wall', '-Wextra', '-Werror',
]


def command(tool):
    native = {'cc': 'clang', 'ld.lld': 'ld.lld', 'objcopy': 'llvm-objcopy', 'nm': 'llvm-nm'}
    override = os.environ.get('X9_' + {'cc': 'CC', 'ld.lld': 'LD', 'objcopy': 'OBJCOPY', 'nm': 'NM'}[tool])
    executable = override or shutil.which(native[tool])
    if executable:
        return [executable]
    import importlib.util
    if importlib.util.find_spec('ziglang'):
        return [sys.executable, '-I', '-m', 'ziglang', tool]
    raise RuntimeError(f'{native[tool]} missing: install LLVM or ziglang')


def run(cmd):
    print(' '.join(map(str, cmd)), flush=True)
    subprocess.run(list(map(str, cmd)), check=True, cwd=ROOT)


def symbols(elf):
    text = subprocess.check_output(command('nm') + ['-n', str(elf)], text=True, cwd=ROOT)
    result = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1].upper() != 'U':
            result[parts[2]] = int(parts[0], 16)
    return result


def abs64_relocs(obj):
    import struct
    b = Path(obj).read_bytes()
    shoff, = struct.unpack_from('<Q', b, 0x28)
    entry_size, count = struct.unpack_from('<HH', b, 0x3a)
    result = 0
    for i in range(count):
        section = shoff + i * entry_size
        if struct.unpack_from('<I', b, section + 4)[0] != 4:  # SHT_RELA
            continue
        target, = struct.unpack_from('<I', b, section + 0x2c)
        flags, = struct.unpack_from('<Q', b, shoff + target * entry_size + 8)
        if not flags & 2:  # SHF_ALLOC
            continue
        off, size = struct.unpack_from('<QQ', b, section + 0x18)
        for pos in range(off, off + size, 24):
            info, = struct.unpack_from('<Q', b, pos + 8)
            result += (info & 0xffffffff) == 257
    return result
