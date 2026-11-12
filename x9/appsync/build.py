#!/usr/bin/env python3
"""Build an offline X9 AppSync runtime; never mount, stage or modify a device.

Produces new files only, plus a kernel hash manifest. No Apple binary changes,
LC_LOAD_DYLIB rewriting, PAC/4KiB board patches or global vtable changes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PREFIX = '/private/var/x9/jailbreak/20H392'
sys.path.insert(0, str(HERE / 'vendor'))
import signer


def build(out, developer):
    out = out.resolve()
    if not any(out.is_relative_to(p.resolve()) for p in (ROOT / '.local', ROOT / 'x9/out')):
        raise ValueError('output must be in .local or x9/out')
    # Never reuse a successful output, even for a failed partial build.
    out.mkdir(parents=True, exist_ok=False)
    sdk = developer / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
    clang = developer / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    env = dict(os.environ, DEVELOPER_DIR=str(developer),
               X9_APPSYNC_CACHE=str(ROOT / '.local/x9/appsync-signer'))
    root = out / 'root'
    install = root / PREFIX.lstrip('/')
    lib, bin_dir = install / 'lib', install / 'bin'
    lib.mkdir(parents=True)
    bin_dir.mkdir()
    flags = [str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk),
             '-O2', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-framework', 'Foundation']

    def run(command):
        print('+', ' '.join(map(str, command)), flush=True)
        subprocess.run(list(map(str, command)), env=env, check=True)

    dylib = lib / 'UniXNUAppSync.dylib'
    run(flags + ['-dynamiclib', HERE / 'vendor/AppSync.m', HERE / 'vendor/Signing.m',
                 '-DAS_SIGNER_PATH="' + PREFIX + '/lib/' + signer.LIBRARY + '"',
                 '-install_name', PREFIX + '/lib/UniXNUAppSync.dylib', '-o', dylib])
    # Full in-process recursive signer; installd need not fork or execute ldid.
    run([sys.executable, '-I', HERE / 'vendor/signer.py', '--output', lib / signer.LIBRARY,
         '--metadata', out / 'signer.json'])
    appinst = bin_dir / 'x9-appinst'
    run(flags + [HERE / 'vendor/appinst.m', '-o', appinst])
    ctl = bin_dir / 'x9-jbctl'
    run(flags + [HERE / 'jbctl.m', '-o', ctl])
    entitlements = out / 'appinst-entitlements.plist'
    entitlements.write_bytes(plistlib.dumps({
        'application-identifier': 'org.x9.appinst', 'platform-application': True,
        'com.apple.private.mobileinstall.allowedSPI': ['Install', 'InstallForLaunchServices'],
        'com.apple.security.exception.mach-lookup.global-name': ['com.apple.mobile.installd'],
    }))
    for binary in (dylib, lib / signer.LIBRARY, appinst, ctl):
        extra = ['--entitlements', entitlements] if binary == appinst else []
        run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', *extra, binary])
        binary.chmod(0o755)
    # This is build metadata, not device or post-build image verification.
    files = {}
    for binary in (dylib, lib / signer.LIBRARY, appinst, ctl):
        data = binary.read_bytes()
        files['/' + binary.relative_to(root).as_posix()] = {
            'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'mode': '0755'}
    license_dir = install / 'licenses'
    license_dir.mkdir()
    import shutil
    shutil.copyfile(HERE / 'vendor/LICENSE', license_dir / 'AppSync-GPL-3.0')
    # signer's content-addressed cache
    # has licenses and linkable LGPL objects. Copy dependency notices into seed.
    value = json.loads((out / 'signer.json').read_text())
    if value['target'] != 'arm64-apple-ios16.0' or value['library'] != signer.INSTALL_NAME:
        raise ValueError('signer target/path mismatch')
    cache = Path(value['cache'])
    shutil.copytree(cache / 'licenses', license_dir / 'signer')
    files_manifest = dict(abi=1, build='20H392', target='arm64-apple-ios16.0', prefix=PREFIX,
        files=files, hardware_verified=False, installed=False,
        native_usb_complete=False, bootstrap_included=False,
        signer_relink_objects=str(cache), activation='appsync=1 after explicit extent staging')
    (out / 'runtime.json').write_text(json.dumps(files_manifest, indent=2) + '\n')
    print(f'Built offline AppSync runtime: {out}; NOT staged or hardware verified', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--developer-dir', type=Path)
    a = ap.parse_args()
    developer = a.developer_dir or Path(json.loads((ROOT / 'x9/build/local.json').read_text())['developer_dir'])
    build(a.out, developer.resolve())


if __name__ == '__main__':
    main()
