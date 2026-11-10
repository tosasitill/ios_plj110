#!/usr/bin/env python3
"""Build/sign iOS16 X9Audio.driver and audioctl locally; never install or access a device."""
import argparse
import json
import plistlib
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--developer-dir', type=Path)
    a = ap.parse_args()
    dev = a.developer_dir or Path(json.loads((HERE.parent / 'build/local.json').read_text())['developer_dir'])
    ios = dev / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
    mac = dev / 'Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
    clang = dev / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    bundle = a.out / 'X9Audio.driver'
    if bundle.exists():
        ap.error('output bundle already exists; use a new output directory')
    bundle.mkdir(parents=True)
    common = [str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(ios),
              '-O2', '-Wall', '-Wextra', '-Werror', '-framework', 'CoreFoundation', '-framework', 'IOKit']
    with tempfile.TemporaryDirectory() as tmp:
        headers = Path(tmp) / 'CoreAudio'
        headers.mkdir()
        for name in ('AudioServerPlugIn.h', 'AudioHardwareBase.h', 'AudioHardware.h'):
            (headers / name).symlink_to(mac / 'System/Library/Frameworks/CoreAudio.framework/Headers' / name)
        subprocess.run(common + ['-I', tmp, '-bundle', '-fvisibility=hidden', str(HERE / 'X9AudioPlugIn.c'),
                                 '-o', str(bundle / 'X9Audio')], check=True)
    factory = 'AE8F4830-DB5A-47D7-871E-FD8806A91E19'
    info = dict(CFBundleExecutable='X9Audio', CFBundleIdentifier='org.ios-sweet.x9.audio',
                CFBundleName='X9Audio', CFBundlePackageType='BNDL', CFBundleVersion='1',
                CFBundleShortVersionString='0.1', MinimumOSVersion='16.0',
                CFBundleSupportedPlatforms=['iPhoneOS'], CFPlugInFactories={factory: 'X9Audio_Create'},
                CFPlugInTypes={'443ABAB8-E7B3-491A-B985-BEB9187030DB': [factory]})
    (bundle / 'Info.plist').write_bytes(plistlib.dumps(info))
    (bundle / 'LICENSE.UniXNU').write_bytes((HERE / 'LICENSE.UniXNU').read_bytes())
    tool = a.out / 'audioctl'
    subprocess.run(common + [str(HERE / 'audioctl.c'), '-o', str(tool)], check=True)
    for path in (bundle, tool):
        subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(path)], check=True)
    print(f'Built {bundle} and {tool}; not installed, physical audio not verified')

if __name__ == '__main__':
    main()
