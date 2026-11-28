#!/usr/bin/env python3
"""iOS-ABI X9 kernel + XnuPrelink of selected original 20H392 kexts + boot.img.

    python3 -I x9/build/scripts/build-usb-prelink.py <name> [--boot-arg TOKEN ...]

1. build.py kernel against .local/dxb-iosabi (iOS kext C++ ABI; x9/xnu/build-xnu.sh ios)
2. .local/references/XnuPrelink/emit_prelink.py links the selected roots
   out of .local/x9/usb/native-20H392/kernelcache.macho into the kernel's
   empty __PRELINK_* segments at a slide anchored above the native kernel
3. build.py boot packages the prelinked kernel (--boot-config, default
   x9/build/boot-device.json)

Roots: the default root set is com.apple.iokit.IOStorageFamily only -- the
APFS P0 line needs the IOStorageFamily classes in-kernel, and its declared
libraries are KPIs only, so the closure is exactly one module (~114 KiB).
The historical USB roots (IOUSBDeviceFamily, AppleUSBDeviceMux, +
AppleUSBCommon) are NOT linked by default: the current kernel carries its
own USB mux and production candidates ship with an empty __PRELINK_INFO, so
nothing in the current kernel consumes them; pass --usb-roots to reproduce
the old USB experiment set.

Prelink base: PRELINK_BASE_FLOOR (0xfffffe000a400000) was the fixed anchor
of the 284-era kernel. The native kernel has since grown past it (its
__LINKEDIT now ends above the anchor), so emit plan() reported the rebased
components colliding with the native __LINKEDIT and this script's guard
aborted -- for every root set. Nothing else in the chain pins the anchor:
the shim validator, mkpayload and XNU all derive the kernel layout from
the Mach-O segment table. The base is therefore derived per build:
max(PRELINK_BASE_FLOOR, 16K-align(kernel end + 0x40000)), keeping the
historical 256 KiB guard gap above the native kernel. The blob's VA span
is set by the kernelcache coalesced geometry (__PRELINK_TEXT ..
__PRELINK_DATA = 0x2df4000); native kernel + gap + blob + __PRELINK_INFO
totals ~100 MiB, far inside the shim's first contiguous RAM extent
(~972 MiB), and mkpayload's kernel_end follows the segment table.

Every input is under the repository; outputs: x9/out/<name>-kernel (native kernel),
x9/out/<name>-prelink (prelinked kernel, logs), x9/out/<name> (boot.img).
XnuPrelink places __PRELINK_INFO 4K-aligned right after __LINKEDIT. It is moved
above every other segment, 16K-aligned (its XML holds no self-references):
XNU's 16K-page arm_vm_init and the shim validator need the alignment, and for a
non-fileset kernel arm_vm_init sets vm_kernel_slid_top = vm_prelink_einfo, so
__PRELINK_INFO must end above all kext segments or ml_static_slide() returns 0
for them and every kext start fails "pointer is outside of kext range"
(prelink5, 2026-11-12). No device operations.
"""
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
LOCAL = ROOT / '.local'
PRELINK_BASE_FLOOR = 0xfffffe000a400000  # historical anchor; the live base is derived in main()
KC_BASE = 0xfffffff004dc8000        # unslid base of the 20H392 kernelcache (__PRELINK_TEXT va)
DEFAULT_ROOTS = ['com.apple.iokit.IOStorageFamily']
USB_ROOTS = ['com.apple.iokit.IOUSBDeviceFamily', 'com.apple.driver.AppleUSBDeviceMux']
# APFS on userdata (2026-11-28): closure = apfs + AppleEffaceableStorage,
# CoreAnalyticsFamily, IOCryptoAcceleratorFamily, IOStorageFamily,
# kec.corecrypto, CoreTrust, AppleImage4.  Needs the kernel's
# vfs_getvfs_with_vfsops and the prelink_rebind.py decline table.
APFS_ROOTS = ['com.apple.filesystems.apfs']
PAGE = 0x4000


def segments(d):
    """(name, command offset, vmaddr, vmsize) for every LC_SEGMENT_64."""
    ncmds, = struct.unpack_from('<I', d, 16)
    off, out = 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        if cmd == 0x19:
            name = d[off + 8:off + 24].rstrip(b'\0').decode()
            va, vs = struct.unpack_from('<QQ', d, off + 24)
            out.append((name, off, va, vs))
        off += size
    return out


def align_prelink_info(path):
    d = bytearray(path.read_bytes())
    segs = segments(d)
    name, off, va, vs = next(s for s in segs if s[0] == '__PRELINK_INFO')
    top = max(ova + ovs for other, _, ova, ovs in segs if other != name and ovs)
    new = (top + PAGE - 1) & ~(PAGE - 1)
    if va == new:
        return va
    nsects, = struct.unpack_from('<I', d, off + 64)
    struct.pack_into('<Q', d, off + 24, new)
    for i in range(nsects):
        sect = off + 72 + i * 80
        addr, = struct.unpack_from('<Q', d, sect + 32)
        struct.pack_into('<Q', d, sect + 32, addr - va + new)
    path.write_bytes(d)
    return new


def main():
    import argparse
    ap = argparse.ArgumentParser(usage=__doc__)
    ap.add_argument('name')
    ap.add_argument('--boot-arg', action='append', default=[], metavar='TOKEN',
                    help='extra boot-arg token for this image (repeatable), passed to build.py kernel and boot')
    ap.add_argument('--usb-roots', action='store_true',
                    help='also link the historical USB roots (default: IOStorageFamily only)')
    ap.add_argument('--apfs', action='store_true',
                    help='also link the APFS closure, through prelink_rebind.py (kernel-owned interface registrations declined)')
    ap.add_argument('--root-manifest', type=Path,
                    help='override local.json root_manifest for this build only (e.g. the private manifest apfs_userdata.py provision emits)')
    ap.add_argument('--boot-config', type=Path, default=ROOT / 'x9/build/boot-device.json',
                    help='boot-device json for the boot step (default: x9/build/boot-device.json)')
    a = ap.parse_args()
    name = a.name
    extra = [x for t in a.boot_arg for x in ('--boot-arg', t)]
    out = ROOT / 'x9/out' / (name + '-prelink')
    out.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((ROOT / 'x9/build/local.json').read_text())
    cfg['dxb'] = str(LOCAL / 'dxb-iosabi')
    cfg.pop('compiler_launcher', None)
    if a.root_manifest:
        cfg['root_manifest'] = str(a.root_manifest.resolve())
    cfg_path = out / 'local-iosabi.json'
    cfg_path.write_text(json.dumps(cfg, indent=1) + '\n')
    env = dict(os.environ, DEVELOPER_DIR=cfg['developer_dir'], X9_HOMEBREW=cfg.get('homebrew', '/opt/homebrew'))
    py = sys.executable

    def run(label, cmd):
        print(label, 'starting', flush=True)
        with (out / f'{label}.log').open('w') as f:
            p = subprocess.run(cmd, cwd=ROOT, env=env, stdout=f, stderr=subprocess.STDOUT)
        print(label, 'exit', p.returncode, flush=True)
        if p.returncode:
            sys.exit(f'{label} failed, see {out / (label + ".log")}')

    kname = name + '-kernel'
    run('kernel', [py, '-I', 'x9/build/build.py', 'kernel', '--name', kname, '--config', str(cfg_path), '-j', '8'] + extra)
    kernel = ROOT / 'x9/out' / kname / 'kernel.development.sweet'
    end = max(va + vs for _, _, va, vs in segments(kernel.read_bytes()) if vs)
    # 16K-align so every rebased coalesced segment stays 16K-aligned (the
    # kernelcache segment VAs are 16K-aligned and the shim validator
    # requires 16K-aligned segment vmaddrs); the slide stays 16K-aligned.
    prelink_base = max(PRELINK_BASE_FLOOR, (end + 0x40000 + PAGE - 1) & ~(PAGE - 1))
    if end + 0x40000 > prelink_base:
        sys.exit(f'native kernel ends at {end:#x}, too close to the prelink base {prelink_base:#x}')
    print(f'native kernel ends at {end:#x}; prelink base {prelink_base:#x}; slide {prelink_base - KC_BASE:#x}', flush=True)
    roots = list(DEFAULT_ROOTS) + (list(USB_ROOTS) if a.usb_roots else []) + (list(APFS_ROOTS) if a.apfs else [])
    root_args = [x for r in roots for x in ('--root', r)]
    prelinked = out / 'kernel.prelinked'
    emitter = ([py, '-I', str(ROOT / 'x9/build/scripts/prelink_rebind.py')] if a.apfs
               else [py, str(LOCAL / 'references/XnuPrelink/emit_prelink.py')])
    run('prelink', [*emitter, '--kernel', str(kernel),
                    '--prelinked', str(LOCAL / 'x9/usb/native-20H392/kernelcache.macho'),
                    *root_args,
                    f'--slide={hex(prelink_base - KC_BASE)}', '--output', str(prelinked)])
    print('__PRELINK_INFO at', hex(align_prelink_info(prelinked)), flush=True)
    # build.py boot expects the kernel output's sidecar metadata next to the kernel.
    for p in (ROOT / 'x9/out' / kname).glob('*.json'):
        (out / p.name).write_bytes(p.read_bytes())
    run('boot', [py, '-I', 'x9/build/build.py', 'boot', '--kernel', str(prelinked), '--name', name,
                 '--config', str(cfg_path), '--boot-config', str(a.boot_config)] + extra)
    print('done', out, flush=True)


if __name__ == '__main__':
    main()
