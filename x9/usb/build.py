#!/usr/bin/env python3
"""Build own iOS16 arm64 USB SDK/client. Does not install or access a device."""
import argparse
import json
import subprocess
from pathlib import Path
HERE = Path(__file__).resolve().parent

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--developer-dir', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    dev = a.developer_dir or Path(json.loads((HERE.parent / 'build/local.json').read_text())['developer_dir'])
    sdk = dev / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
    clang = dev / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    ar = dev / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/ar'
    a.out.mkdir(parents=True, exist_ok=True)
    common = [str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk), '-Os', '-Wall', '-Wextra', '-Werror']
    lib, ctl = a.out / 'libx9usb.a', a.out / 'usbctl'
    objects = []
    for source in ('x9usb.c', 'native_iokit.c'):
        obj = a.out / (Path(source).stem + '.o')
        subprocess.run(common + ['-c', str(HERE / source), '-o', str(obj)], check=True)
        objects.append(str(obj))
    lib.unlink(missing_ok=True)
    subprocess.run([str(ar), 'rcs', str(lib), *objects], check=True)
    subprocess.run(common + [str(HERE / 'usbctl.c'), str(lib), '-framework', 'CoreFoundation', '-framework', 'IOKit', '-o', str(ctl)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(ctl)], check=True)
    print(f'USB iOS SDK/client built: {ctl}; not installed/hardware-verified')

if __name__ == '__main__':
    main()
