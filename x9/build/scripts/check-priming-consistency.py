#!/usr/bin/env python3
"""Fail when Metal pipeline priming (M3) is packaged inconsistently.

The rev287 incident: the plugin bundle shipped X9GeneralMetal.primelist
but the X9GeneralMetal binary contained no priming code at all. The plugin
build compiles the frontend from .local/x9/MaliMetal/X9/General -- a
symlink into the main checkout's x9/metal/General -- while the primelist
had been applied to that directory on its own, after the priming sources
were reverted there. The build succeeded, the package staged, the device
booted, and priming silently did not exist: not one x9-prime line in the
whole session log, because there was no code to emit one.

The dangerous direction is inventory-without-code: a dead feature that
looks shipped. That is a hard failure here, for both the frontend source
directory (checked before compiling) and the built bundle (checked after
linking). The reverse -- code compiled in, no inventory packaged -- was
the deliberate rev286 stepping stone and stays legal, but the bundle
check reports it loudly; on device it logs "x9-prime: no inventory".

Usage:
  check-priming-consistency.py --source DIR   # frontend source dir
  check-priming-consistency.py --bundle DIR   # built .bundle dir
Exit 0 = consistent, 1 = inventory packaged without priming code.
"""
import argparse
import sys
from pathlib import Path

PRIMELIST = 'X9GeneralMetal.primelist'
PRIMING_INC = 'X9MetalPriming.inc'
FRONTEND_MM = 'NativeBridgeMetalFrontend.mm'
BINARY = 'X9GeneralMetal'
# Present in the linked (and -S stripped) binary only when X9MetalPriming.inc
# was compiled in; verified against the shipped rev286 (present) and rev287
# (absent) frontend binaries.
MARKER = b'x9-prime: pass'


def source_has_code(source: Path) -> bool:
    inc = source / PRIMING_INC
    mm = source / FRONTEND_MM
    if not inc.is_file() or not mm.is_file():
        return False
    return PRIMING_INC in mm.read_text(errors='replace')


def check_source(source: Path) -> int:
    if not (source / PRIMELIST).is_file():
        return 0
    if source_has_code(source):
        return 0
    print(f'priming consistency: {source} ships {PRIMELIST} but the priming '
          f'sources are absent or not included by {FRONTEND_MM} there. The '
          f'bundle would package a dead inventory (the rev287 failure). The '
          f'bridge dir may resolve through .local into another checkout; '
          f'make the priming sources and the inventory agree in that dir.',
          file=sys.stderr)
    return 1


def check_bundle(bundle: Path) -> int:
    binary = bundle / BINARY
    if not binary.is_file():
        print(f'priming consistency: no {BINARY} in {bundle}', file=sys.stderr)
        return 1
    has_code = MARKER in binary.read_bytes()
    has_list = (bundle / PRIMELIST).is_file()
    if has_list and not has_code:
        print(f'priming consistency: {bundle} ships {PRIMELIST} but the '
              f'{BINARY} binary contains no priming code (marker '
              f'{MARKER!r} absent). Refusing a silently dead feature; see '
              f'the rev287 note in X9MetalPriming.inc.', file=sys.stderr)
        return 1
    if has_code and not has_list:
        print('priming consistency: WARNING priming is compiled in but no '
              'primelist is packaged; on device this logs "x9-prime: no '
              'inventory" and primes nothing (the rev286 state).',
              file=sys.stderr)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument('--source', type=Path)
    group.add_argument('--bundle', type=Path)
    opts = ap.parse_args()
    if opts.source:
        return check_source(opts.source)
    return check_bundle(opts.bundle)


if __name__ == '__main__':
    sys.exit(main())
