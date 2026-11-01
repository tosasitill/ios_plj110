#!/usr/bin/env python3
"""Build this repo's direct X9 LK/shim/XNU candidate, without accessing a device.

Examples:
  python3 -I x9/tools/boot-xnu.py --stub
  python3 -I x9/tools/boot-xnu.py --kernel xnu/out/kernel.development.sweet

A real kernel must include the appended X9 board-isolation patch. A stub only
exercises the loader ABI. UEFI, UniXNU and the reference workspaces are unused.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from shared import adt, load
import mkadt
import mkpayload
import bootctl
shim = load('x9_shim_build', 'x9/shim/build.py')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    source = ap.add_mutually_exclusive_group(required=True)
    source.add_argument('--kernel', type=Path, help='this repo\'s plain arm64 SWEET XNU kernel')
    source.add_argument('--stub', action='store_true', help='build the no-XNU loader test image')
    ap.add_argument('--out', type=Path, default=ROOT / 'x9/out')
    ap.add_argument('--ramdisk', type=Path, help='optional md0 root image embedded in SWEETXNU')
    ap.add_argument('--boot-args', default='debug=0xc serial=3 cpus=1 keepsyms=1 -v')
    ap.add_argument('--timebase', type=lambda s: int(s, 0), default=mkadt.TIMEBASE)
    ap.add_argument('--console-base', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--console-size', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--boot-control', type=Path, help='optional existing 32-byte A/B snapshot; no device IO')
    ap.add_argument('--slot', choices=['a', 'b'], default='a')
    ap.add_argument('--diag-only', action='store_true',
                    help='shim validates and logs, then halts without copying or jumping')
    ap.add_argument('--pstore-mirror', action='store_true',
                    help='also mirror the shim log into the X9 ramoops console zone')
    a = ap.parse_args()
    # Mirrored output needs a real zone; without one the kernel's pstore stays
    # NULL and every milestone silently goes to a local buffer that no readback
    # path can see. That combination looks exactly like "the kernel never got
    # there", so refuse it instead of producing an image with no evidence trail.
    if a.pstore_mirror and (not a.console_base or not a.console_size):
        ap.error('--pstore-mirror requires --console-base and --console-size')
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    bctl = bootctl.prepare(a.boot_control.read_bytes(), a.slot) if a.boot_control else None
    tree = mkadt.build(timebase=a.timebase, console_base=a.console_base, console_size=a.console_size)
    tree_path = out / 'x9.adt'
    tree_path.write_bytes(adt.serialize(tree))
    if a.stub:
        kernel = out / 'stub.macho'
        args = [sys.executable, '-I', str(HERE / 'stub/mkstub.py'), str(kernel),
                '--console-base', str(a.console_base), '--console-size', str(a.console_size)]
        if a.pstore_mirror:
            args.append('--pstore-mirror')
        subprocess.run(args, check=True, cwd=ROOT)
    else:
        kernel = a.kernel.resolve()
    blob, info = mkpayload.build(kernel, tree_path, a.boot_args, a.ramdisk, loader_stub=a.stub)
    container = out / 'payload.bin'
    container.write_bytes(blob)
    report = shim.build(container, out / 'shim', a.console_base, a.console_size,
                        a.diag_only, a.pstore_mirror)
    image = out / 'boot.img'
    image.write_bytes((out / 'shim/shim-boot.img').read_bytes())
    report.update({'kernel_kind': 'loader-test-stub' if a.stub else 'SWEET-XNU-candidate',
                   'kernel_sha256': info['kernel_sha256'], 'slot': a.slot,
                   'x9_board_isolation_tag': info['x9_board_isolation_tag'],
                   'device_written': False, 'boot_control_prepared': bctl is not None})
    if bctl:
        (out / f'bctl_slot_{a.slot}.bin').write_bytes(bctl)
    (out / 'boot.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Candidate: {image}\nType: {report["kernel_kind"]}; no UEFI, no device writes, not hardware-verified.')


if __name__ == '__main__':
    main()
