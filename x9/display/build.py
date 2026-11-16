#!/usr/bin/env python3
"""Build/sign/embed own-code status bar bridge. No device/root/cache IO."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
X9 = HERE.parent


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--developer-dir', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    dev = a.developer_dir
    clang = dev / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    sdk = dev / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
    library = a.out / 'libx9-statusbar.dylib'
    command = [str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk),
               '-Os', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-dynamiclib',
               '-framework', 'UIKit', '-framework', 'Foundation',
               '-install_name', '/usr/lib/libx9-statusbar.dylib',
               str(HERE / 'statusbar.m'), '-o', str(library)]
    subprocess.run(command, check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none',
                    str(library)], check=True)
    data = library.read_bytes()
    if not data or len(data) > 128 * 1024:
        raise ValueError('status bar bridge exceeds 128 KiB budget')
    digest = hashlib.sha256(data).digest()
    path = f'/private/var/x9/display/libx9-statusbar-{digest.hex()[:16]}.dylib'
    lines = ['/* Generated own code. Do not commit. */',
             f'#define X9_UI_BRIDGE_PATH "{path}"',
             'static const uint8_t x9_ui_bridge_sha[32] = {' +
             ','.join(f'0x{b:02x}' for b in digest) + '};',
             'static const uint8_t x9_ui_bridge_data[] = {']
    lines += [','.join(f'0x{b:02x}' for b in data[i:i+16]) + ','
              for i in range(0, len(data), 16)]
    text = '\n'.join(lines) + '\n};\n'
    header = X9 / 'kernel/generated/x9_ui_bridge.inc'
    header.parent.mkdir(parents=True, exist_ok=True)
    if not header.exists() or header.read_text() != text:
        header.write_text(text)
    (a.out / 'display-build.json').write_text(json.dumps({
        'schema': 1, 'ios_build': '20H392', 'bytes': len(data),
        'sha256': digest.hex(), 'ios_path': path, 'boot_arg': 'x9_ui=1',
        'disable_boot_arg': 'x9_ui=0',
        'artwork_subtype': 2556, 'offset_native_pixels': 20,
        'portrait_main_display_only': True, 'runtime_verified': False,
        'device_installation_executed': False, 'argv': command,
    }, indent=2) + '\n')
    print(f'X9 UI: signed {len(data)} bytes; x9_ui=0 disables injection; no device IO')


if __name__ == '__main__':
    main()
