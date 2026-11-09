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
    ap.add_argument('--ufs-native-manifest',type=Path,help='native XNU ufsroot0: no whole-image RAM copy')
    ap.add_argument('--ufs-root-manifest', type=Path,
                    help='read plaintext pinned-file image from UFS into RAM md0; legacy512 only')
    ap.add_argument('--ufs-probe', action='store_true',
                    help='read-only retained-link UFS qualification before XNU, with SMMU DMA gate')
    ap.add_argument('--usb-receiver', action='store_true',
                    help='small shim receives a raw HFS+ root over USB before XNU')
    ap.add_argument('--sparse-ramdisk', action='store_true',
                    help='reconstruct zero root-image gaps in RAM using v1 segment BSS')
    ap.add_argument('--ramdisk', type=Path, help='optional md0 root image embedded in SWEETXNU')
    ap.add_argument('--boot-args', default='debug=0xc serial=3 cpus=1 keepsyms=1 -v')
    ap.add_argument('--ram-window', choices=mkadt.RAM_WINDOWS, default='legacy512',
                    help='captured X9 RAM range; large4g is experimental, not hardware-verified')
    ap.add_argument('--boot-partition-size', type=lambda s: int(s,0), default=0x4000000)
    ap.add_argument('--max-image-size', type=lambda s: int(s,0), default=0,
                    help='optional decompressed Image limit, in bytes, before LK FDT/reserved RAM')
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
    if a.ram_window == 'segmented8g':
        if a.stub or a.ufs_root_manifest or a.ramdisk or a.sparse_ramdisk:
            ap.error('segmented8g requires a real kernel and no RAM root')
        if 'x9_ram_8g=0' in a.boot_args.split():
            ap.error('segmented8g conflicts with x9_ram_8g=0 (the kernel defaults it on for this span)')
        if b'X9 RAM: missing live map' not in a.kernel.read_bytes():
            ap.error('segmented8g requires the kernel RAM allocator patch')
    if a.ufs_native_manifest and a.ufs_root_manifest:
        ap.error('select either native UFS or RAM md0 handoff')
    root_manifest=a.ufs_native_manifest or a.ufs_root_manifest
    if root_manifest:
        if a.stub or a.usb_receiver or a.diag_only or a.ramdisk or a.sparse_ramdisk or a.ram_window not in ('legacy512','segmented8g'):
            ap.error('--ufs-root-manifest needs real kernel, legacy512, and no embedded root/USB/diag')
        from ufs_root_manifest import load
        root_info=load(root_manifest,native=bool(a.ufs_native_manifest))
        a.ufs_probe=True
    if a.ufs_probe and (a.usb_receiver or a.diag_only):
        ap.error('--ufs-probe cannot be combined with USB receiver or diag-only')
    if a.usb_receiver and (a.stub or a.ramdisk or a.diag_only or a.ram_window not in ('large4g','large5g')):
        ap.error('--usb-receiver requires a real kernel, large4g/large5g, and no embedded root/diag-only')
    # Mirrored output needs a real zone; without one the kernel's pstore stays
    # NULL and every milestone silently goes to a local buffer that no readback
    # path can see. That combination looks exactly like "the kernel never got
    # there", so refuse it instead of producing an image with no evidence trail.
    if a.pstore_mirror and (not a.console_base or not a.console_size):
        ap.error('--pstore-mirror requires --console-base and --console-size')
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    bctl = bootctl.prepare(a.boot_control.read_bytes(), a.slot) if a.boot_control else None
    tree = mkadt.build(timebase=a.timebase, console_base=a.console_base, console_size=a.console_size, ram_window=a.ram_window,
                       wlan=any(x in a.boot_args.split() for x in ('x9_wifi_fake=1', 'x9_wifi_real=1')),
                       baseband='x9_bb=1' in a.boot_args.split())
    if a.usb_receiver or a.ufs_root_manifest:
        chosen = next(n for n in tree.children if n.prop('name') == b'chosen\0')
        mmap = next(n for n in chosen.children if n.prop('name') == b'memory-map\0')
        mmap.props.append(('RAMDisk', 0, bytes(16)))
    if a.ufs_native_manifest:
        from ufs_root_manifest import add_native_adt
        add_native_adt(tree,root_info)
        if b'X9 UFS NATIVE ROOT:' not in a.kernel.read_bytes():
            ap.error('native root requires kernel patch0196')
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
    blob, info = mkpayload.build(kernel, tree_path, a.boot_args, a.ramdisk, loader_stub=a.stub, sparse_ramdisk=a.sparse_ramdisk)
    container = out / 'payload.bin'
    container.write_bytes(blob)
    report = shim.build(container, out / 'shim', a.console_base, a.console_size,
                        a.diag_only, a.pstore_mirror, a.usb_receiver, a.ufs_probe, root_manifest, bool(a.ufs_native_manifest))
    image = out / 'boot.img'
    boot_bytes = (out / 'shim/shim-boot.img').read_bytes()
    image_size = (out / 'shim/Image').stat().st_size
    if a.boot_partition_size <= 0 or len(boot_bytes) > a.boot_partition_size:
        raise ValueError('candidate exceeds captured boot partition size')
    if a.max_image_size < 0 or (a.max_image_size and image_size > a.max_image_size):
        raise ValueError('decompressed Image exceeds configured LK loading region')
    image.write_bytes(boot_bytes)
    report.update({'kernel_kind': 'loader-test-stub' if a.stub else 'SWEET-XNU-candidate',
                   'kernel_sha256': info['kernel_sha256'], 'slot': a.slot,
                   'x9_board_isolation_tag': info['x9_board_isolation_tag'],
                   'device_written': False, 'boot_control_prepared': bctl is not None,
                   'ram_window': a.ram_window, 'dram_base': hex(mkadt.RAM_WINDOWS[a.ram_window][0]),
                   'dram_size': hex(mkadt.RAM_WINDOWS[a.ram_window][1]),
                   'dram_usable_source': 'live LK FDT -> chosen/x9-ram-map' if a.ram_window=='segmented8g' else 'fixed window',
                   'dram_usable_upper_bound_bytes': 9161670656 if a.ram_window=='segmented8g' else mkadt.RAM_WINDOWS[a.ram_window][1],
                   'boot_partition_size': a.boot_partition_size,
                   'max_image_size': a.max_image_size,
                   'ramdisk_transport': 'native-ufs-extent-root' if a.ufs_native_manifest else 'ufs-plaintext-to-ram' if a.ufs_root_manifest else info.get('ramdisk_transport', 'dense-v1'),
                   'ramdisk': info.get('ramdisk'),
                   'ramdisk_stored_bytes': info.get('ramdisk_stored_bytes')})
    if bctl:
        (out / f'bctl_slot_{a.slot}.bin').write_bytes(bctl)
    (out / 'boot.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Candidate: {image}\nType: {report["kernel_kind"]}; no UEFI, no device writes, not hardware-verified.')


if __name__ == '__main__':
    main()
