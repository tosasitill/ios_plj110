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
                 '-DAS_JBCTL_PATH="' + PREFIX + '/bin/x9-jbctl"',
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
    ctl_entitlements = out / 'jbctl-entitlements.plist'
    ctl_entitlements.write_bytes(plistlib.dumps({
        'platform-application': True,
        'com.apple.lsapplicationworkspace.rebuildappdatabases': True,
        'com.apple.private.MobileContainerManager.allowed': True,
        'com.apple.private.security.no-sandbox': True,
    }))
    for binary in (dylib, lib / signer.LIBRARY, appinst, ctl):
        extra = (['--entitlements', entitlements] if binary == appinst else
                 ['--entitlements', ctl_entitlements] if binary == ctl else [])
        run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', *extra, binary])
        binary.chmod(0o755)
    build_installer(root, install, flags, run)
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
    # This is build metadata, not device or post-build image verification.
    files = {}
    for path in sorted(root.rglob('*')):
        if path.is_file() and not path.is_symlink():
            data = path.read_bytes()
            files['/' + path.relative_to(root).as_posix()] = {
                'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                'mode': '0755' if data[:4] == b'\xcf\xfa\xed\xfe' else '0644'}
    files_manifest = dict(abi=1, build='20H392', target='arm64-apple-ios16.0', prefix=PREFIX,
        files=files, hardware_verified=False, installed=False,
        native_usb_complete=False, bootstrap_included=False,
        signer_relink_objects=str(cache), activation='appsync=1 after explicit extent staging')
    (out / 'runtime.json').write_text(json.dumps(files_manifest, indent=2) + '\n')
    print(f'Built offline AppSync runtime: {out}; NOT staged or hardware verified', flush=True)


def png(size):
    """A plain app icon: rounded-free dark square with a green "X9" bar mark."""
    import struct
    import zlib
    rows = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            u, v = x / size, y / size
            r, g, b = 24, 26, 32
            if 0.22 < v < 0.78 and (abs((u - 0.22) - (v - 0.22)) < 0.07 or abs((u - 0.22) - (0.78 - v)) < 0.07) and u < 0.5:
                r, g, b = 80, 220, 120          # X
            if 0.55 < u < 0.78 and 0.22 < v < 0.78 and (u > 0.71 or v < 0.29 or 0.47 < v < 0.54 or (u < 0.62 and v < 0.54)):
                r, g, b = 80, 220, 120          # 9
            row += bytes((r, g, b))
        rows.append(bytes(row))
    raw = zlib.compress(b''.join(rows), 9)
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', raw) + chunk(b'IEND', b''))


def build_installer(root, install, flags, run):
    """/Applications/X9Installer.app: a non-containerized system app registered
    by SpringBoard through x9-jbctl --register (see installer/main.m)."""
    app = root / 'Applications/X9Installer.app'
    app.mkdir(parents=True)
    run(flags + ['-framework', 'UIKit', '-framework', 'UniformTypeIdentifiers', HERE / 'installer/main.m', '-o', app / 'X9Installer'])
    for scale in (2, 3):
        (app / f'AppIcon60x60@{scale}x.png').write_bytes(png(60 * scale))
    info = {
        'CFBundleIdentifier': 'org.x9.installer', 'CFBundleExecutable': 'X9Installer',
        'CFBundleName': 'X9Installer', 'CFBundleDisplayName': 'X9 安装器',
        'CFBundlePackageType': 'APPL', 'CFBundleVersion': '1', 'CFBundleShortVersionString': '1.0',
        'CFBundleInfoDictionaryVersion': '6.0', 'CFBundleDevelopmentRegion': 'zh_CN',
        'CFBundleSupportedPlatforms': ['iPhoneOS'], 'MinimumOSVersion': '16.0', 'DTPlatformName': 'iphoneos',
        'LSRequiresIPhoneOS': True, 'UIDeviceFamily': [1], 'UIRequiredDeviceCapabilities': ['arm64'],
        'UILaunchScreen': {}, 'UISupportedInterfaceOrientations': ['UIInterfaceOrientationPortrait'],
        'UIFileSharingEnabled': True, 'LSSupportsOpeningDocumentsInPlace': False,
        'CFBundleIcons': {'CFBundlePrimaryIcon': {'CFBundleIconFiles': ['AppIcon60x60'], 'CFBundleIconName': 'AppIcon'}},
        'CFBundleDocumentTypes': [{'CFBundleTypeName': 'iOS App', 'CFBundleTypeRole': 'Viewer',
                                   'LSHandlerRank': 'Owner', 'LSItemContentTypes': ['com.apple.itunes.ipa']}],
        'UTImportedTypeDeclarations': [{'UTTypeIdentifier': 'com.apple.itunes.ipa', 'UTTypeDescription': 'iOS App',
                                        'UTTypeConformsTo': ['public.zip-archive', 'public.data'],
                                        'UTTypeTagSpecification': {'public.filename-extension': ['ipa']}}],
    }
    (app / 'Info.plist').write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_BINARY))
    (app / 'PkgInfo').write_bytes(b'APPL????')
    ents = {
        'application-identifier': 'org.x9.installer', 'platform-application': True,
        'com.apple.private.mobileinstall.allowedSPI': ['Install', 'InstallForLaunchServices'],
        'com.apple.security.exception.mach-lookup.global-name': ['com.apple.mobile.installd'],
        'com.apple.private.security.no-container': True,
        'com.apple.private.security.no-sandbox': True,
    }
    share = install / 'share'
    share.mkdir(exist_ok=True)
    (share / 'X9Installer.entitlements').write_bytes(plistlib.dumps(ents))
    run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none',
         '--entitlements', share / 'X9Installer.entitlements', app])
    (app / 'X9Installer').chmod(0o755)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--developer-dir', type=Path)
    a = ap.parse_args()
    developer = a.developer_dir or Path(json.loads((ROOT / 'x9/build/local.json').read_text())['developer_dir'])
    build(a.out, developer.resolve())


if __name__ == '__main__':
    main()
