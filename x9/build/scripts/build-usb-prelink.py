#!/usr/bin/env python3
"""iOS-ABI X9 kernel + XnuPrelink of the original 20H392 USB device kexts + boot.img.

    python3 -I x9/build/scripts/build-usb-prelink.py <name>

1. build.py kernel against .local/dxb-iosabi (iOS kext C++ ABI; x9/xnu/build-xnu.sh ios)
2. .local/references/XnuPrelink/emit_prelink.py links IOUSBDeviceFamily and
   AppleUSBDeviceMux (and their dependency AppleUSBCommon) out of
   .local/x9/usb/native-20H392/kernelcache.macho into the kernel's empty
   __PRELINK_* segments at a fixed slide
3. build.py boot packages the prelinked kernel with x9/build/boot-device.json

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
PRELINK_BASE = 0xfffffe000a400000   # where the kexts land (above the native kernel)
KC_BASE = 0xfffffff004dc8000        # unslid base of the 20H392 kernelcache
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
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    name = sys.argv[1]
    out = ROOT / 'x9/out' / (name + '-prelink')
    out.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((ROOT / 'x9/build/local.json').read_text())
    cfg['dxb'] = str(LOCAL / 'dxb-iosabi')
    cfg.pop('compiler_launcher', None)
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
    run('kernel', [py, '-I', 'x9/build/build.py', 'kernel', '--name', kname, '--config', str(cfg_path), '-j', '8'])
    kernel = ROOT / 'x9/out' / kname / 'kernel.development.sweet'
    end = max(va + vs for _, _, va, vs in segments(kernel.read_bytes()) if vs)
    if end + 0x40000 > PRELINK_BASE:
        sys.exit(f'native kernel ends at {end:#x}, too close to the prelink base {PRELINK_BASE:#x}')
    prelinked = out / 'kernel.prelinked'
    run('prelink', [py, str(LOCAL / 'references/XnuPrelink/emit_prelink.py'), '--kernel', str(kernel),
                    '--prelinked', str(LOCAL / 'x9/usb/native-20H392/kernelcache.macho'),
                    '--root', 'com.apple.iokit.IOUSBDeviceFamily', '--root', 'com.apple.driver.AppleUSBDeviceMux',
                    f'--slide={hex(PRELINK_BASE - KC_BASE)}', '--output', str(prelinked)])
    print('__PRELINK_INFO at', hex(align_prelink_info(prelinked)), flush=True)
    # build.py boot expects the kernel output's sidecar metadata next to the kernel.
    for p in (ROOT / 'x9/out' / kname).glob('*.json'):
        (out / p.name).write_bytes(p.read_bytes())
    run('boot', [py, '-I', 'x9/build/build.py', 'boot', '--kernel', str(prelinked), '--name', name,
                 '--config', str(cfg_path), '--boot-config', str(ROOT / 'x9/build/boot-device.json')])
    print('done', out, flush=True)


if __name__ == '__main__':
    main()
