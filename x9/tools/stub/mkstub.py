#!/usr/bin/env python3
"""Build a plain arm64 Mach-O stand-in to exercise the real X9 loader path."""
import argparse
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from shared import load
from toolchain import CFLAGS, ROOT, abs64_relocs, command, run
wrapper = load('x9_shared_stub', 'x676c/tools/stub/mkstub.py')
SHIM = ROOT / 'x9/shim'


def build(out, console_base=0, console_size=0, pstore_mirror=False):
    out = Path(out).resolve()
    work = out.parent / 'stub-build'
    work.mkdir(parents=True, exist_ok=True)
    objects = []
    for src in [HERE / 'entry.S', HERE / 'stub.c', SHIM / 'hw.c', SHIM / 'plog.c']:
        obj = work / (src.stem + '.o')
        run(command('cc') + CFLAGS + ['-I', SHIM, f'-DX9_CONSOLE_BASE={console_base}UL',
            f'-DX9_CONSOLE_SIZE={console_size}UL',
            '-DX9_PSTORE_MIRROR=' + ('1' if pstore_mirror else '0'),
            '-c', src, '-o', obj])
        if src.suffix == '.c' and abs64_relocs(obj):
            raise ValueError(f'absolute relocation in stub: {src.name}')
        objects.append(obj)
    elf = work / 'stub.elf'
    run(command('ld.lld') + ['-T', HERE / 'link.ld', '--build-id=none', '-static'] + objects + ['-o', elf])
    binary = work / 'stub.bin'
    run(command('objcopy') + ['-O', 'binary', elf, binary])
    blob = wrapper.macho(binary.read_bytes())
    out.write_bytes(blob)
    print(f'{out}: {len(blob)} bytes; stub only, not XNU')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out', type=Path)
    ap.add_argument('--console-base', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--console-size', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--pstore-mirror', action='store_true',
                    help='mirror the log into the ramoops console zone')
    a = ap.parse_args()
    build(a.out, a.console_base, a.console_size, a.pstore_mirror)


if __name__ == '__main__':
    main()
