#!/usr/bin/env python3
"""Cross-compile/sign the own-code BT diagnostic client; no install/device access."""
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
    dev = a.developer_dir
    if dev is None:
        dev = Path(json.loads((HERE.parent / 'build/local.json').read_text())['developer_dir'])
    sdk = dev / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
    clang = dev / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    a.out.mkdir(parents=True, exist_ok=True)
    binary = a.out / 'btctl'
    subprocess.run([str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk),
                    '-Os', '-Wall', '-Wextra', '-Werror', '-framework', 'CoreFoundation',
                    '-framework', 'IOKit', str(HERE / 'btctl.c'), '-o', str(binary)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(binary)], check=True)
    print(f'BT diagnostic client built: {binary}; not installed or hardware-tested')


if __name__ == '__main__':
    main()
