#!/usr/bin/env python3
"""Cross-compile/sign the own-code proximity registry client; no install/device access."""
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
    root = HERE.parents[1]
    out = a.out.resolve()
    if not out.is_relative_to((root / 'x9/out').resolve()):
        ap.error('output must be under this checkout x9/out')
    a.out = out
    a.out.mkdir(parents=True, exist_ok=True)
    binary = a.out / 'proxctl'
    subprocess.run([str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk),
                    '-Os', '-Wall', '-Wextra', '-Werror', '-framework', 'CoreFoundation',
                    '-framework', 'IOKit', str(HERE / 'proxctl.c'), '-o', str(binary)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(binary)], check=True)
    print(f'proximity registry client built: {binary}; not installed or hardware-tested')


if __name__ == '__main__':
    main()
