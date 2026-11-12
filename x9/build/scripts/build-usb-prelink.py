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
Status 2026-11-12: step 3 fails in the shim payload validator
("bad segment address or size") - the shim does not yet accept the prelinked
layout (segments above __LINKEDIT, 67 MB span). No device operations.
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
    d = kernel.read_bytes()
    ncmds, = struct.unpack_from('<I', d, 16)
    off, end = 32, 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            va, vs = struct.unpack_from('<QQ', d, off + 24)
            if vs:
                end = max(end, va + vs)
        off += size
    if end + 0x40000 > PRELINK_BASE:
        sys.exit(f'native kernel ends at {end:#x}, too close to the prelink base {PRELINK_BASE:#x}')
    prelinked = out / 'kernel.prelinked'
    run('prelink', [py, str(LOCAL / 'references/XnuPrelink/emit_prelink.py'), '--kernel', str(kernel),
                    '--prelinked', str(LOCAL / 'x9/usb/native-20H392/kernelcache.macho'),
                    '--root', 'com.apple.iokit.IOUSBDeviceFamily', '--root', 'com.apple.driver.AppleUSBDeviceMux',
                    f'--slide={hex(PRELINK_BASE - KC_BASE)}', '--output', str(prelinked)])
    # build.py boot expects the kernel output's sidecar metadata next to the kernel.
    for p in (ROOT / 'x9/out' / kname).glob('*.json'):
        (out / p.name).write_bytes(p.read_bytes())
    run('boot', [py, '-I', 'x9/build/build.py', 'boot', '--kernel', str(prelinked), '--name', name,
                 '--config', str(cfg_path), '--boot-config', str(ROOT / 'x9/build/boot-device.json')])
    print('done', out, flush=True)


if __name__ == '__main__':
    main()
