#!/usr/bin/env python3
"""Build/sign own-code iOS 16 orientation adapter; no device or Apple inputs."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
X9 = HERE.parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--developer-dir', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    clang = a.developer_dir / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    sdk = a.developer_dir / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
    library = a.out / 'libx9-orientation.dylib'
    subprocess.run([str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk),
                    '-Os', '-fobjc-arc', '-fblocks', '-Wall', '-Wextra', '-Werror',
                    '-dynamiclib', '-framework', 'Foundation',
                    '-install_name', '/usr/lib/libx9-orientation.dylib',
                    str(HERE / 'orientation_bridge.m'), '-o', str(library)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none',
                    str(library)], check=True)
    data = library.read_bytes()
    if not data or len(data) > 256 * 1024:
        raise ValueError('orientation bridge exceeds 256 KiB embedding budget')
    digest = hashlib.sha256(data).digest()
    path = f'/private/var/x9/motion/libx9-orientation-{digest.hex()[:16]}.dylib'
    header = X9 / 'kernel/generated/x9_motion_bridge.inc'
    lines = ['/* Generated own-code adapter; do not commit. */',
             f'#define X9_MOTION_BRIDGE_PATH "{path}"',
             'static const uint8_t x9_motion_bridge_sha[32] = {' +
             ','.join(f'0x{b:02x}' for b in digest) + '};',
             'static const uint8_t x9_motion_bridge_data[] = {']
    lines += [','.join(f'0x{b:02x}' for b in data[i:i+16]) + ','
              for i in range(0, len(data), 16)]
    text = '\n'.join(lines) + '\n};\n'
    header.parent.mkdir(parents=True, exist_ok=True)
    if not header.exists() or header.read_text() != text:
        header.write_text(text)
    (a.out / 'motion-bridge.json').write_text(json.dumps({
        'bytes': len(data), 'sha256': digest.hex(), 'ios_path': path,
        'profile': 'iOS 16.7.16 / 20H392 / CMDeviceOrientationManager',
        'hardware_tested': False}, indent=2) + '\n')
    print(f'orientation bridge: {len(data)} bytes; opt-in x9_motion=3', flush=True)


if __name__ == '__main__':
    main()
