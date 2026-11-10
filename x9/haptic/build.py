#!/usr/bin/env python3
"""Build/sign/embed the complete Actuator HAL. No device IO or test suite."""
import argparse
import hashlib
import json
import plistlib
import shutil
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
X9 = HERE.parent


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--developer-dir', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    sdk = a.developer_dir / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
    macsdk = a.developer_dir / 'Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
    clang = a.developer_dir / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    bundle = a.out / 'X9Haptic.driver'
    if bundle.exists():
        ap.error('bundle exists; use a new output directory')
    bundle.mkdir(parents=True)
    factory = '96E7C0E3-73BC-40F9-9E19-2795A65BEAA2'
    plist = {'CFBundleIdentifier': 'com.x9.haptic', 'CFBundleName': 'X9Haptic',
             'CFBundleExecutable': 'X9Haptic', 'CFBundlePackageType': 'BNDL',
             'CFBundleInfoDictionaryVersion': '6.0', 'CFBundleVersion': '1',
             'CFBundleSupportedPlatforms': ['iPhoneOS'],
             'CFPlugInTypes': {'443ABAB8-E7B3-491A-B985-BEB9187030DB': [factory]},
             'CFPlugInFactories': {factory: 'X9Haptic_Create'}}
    (bundle / 'Info.plist').write_bytes(plistlib.dumps(plist))
    shutil.copyfile(X9 / 'audio/LICENSE.hakios', bundle / 'LICENSE.hakios')
    flags = [str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk),
             '-Os', '-Wall', '-Wextra', '-Werror']
    with tempfile.TemporaryDirectory() as tmp:
        headers = Path(tmp) / 'CoreAudio'
        headers.mkdir()
        for name in ('AudioServerPlugIn.h', 'AudioHardwareBase.h', 'AudioHardware.h'):
            (headers / name).symlink_to(macsdk / 'System/Library/Frameworks/CoreAudio.framework/Headers' / name)
        subprocess.run(flags + ['-I', tmp, '-bundle', '-framework', 'CoreFoundation',
                               str(HERE / 'actuator_plugin.c'), '-o', str(bundle / 'X9Haptic')], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(bundle)], check=True)
    files = []
    identity = hashlib.sha256()
    allowed = {'Info.plist', 'LICENSE.hakios', 'X9Haptic', '_CodeSignature/CodeResources'}
    for path in sorted(bundle.rglob('*')):
        if path.is_symlink():
            raise ValueError('unexpected symlink in signed bundle')
        if not path.is_file():
            continue
        name = path.relative_to(bundle).as_posix()
        data = path.read_bytes()
        if name not in allowed or not data:
            raise ValueError(f'unexpected/empty signed entry: {name}')
        digest = hashlib.sha256(data).digest()
        identity.update(name.encode() + b'\0' + len(data).to_bytes(8, 'little') + digest)
        files.append((name, data, digest))
    if len(files) != 4 or sum(len(d) for _, d, _ in files) > 512*1024:
        raise ValueError('incomplete or oversized signed bundle')
    source = '/private/var/x9/haptic/' + identity.hexdigest() + '/hal'
    text = ['/* Generated complete signed own-code Actuator HAL. Do not commit. */',
            f'#define X9_HAPTIC_HAL_SOURCE "{source}"',
            '#define X9_HAPTIC_HAL_POINT "/System/Library/Audio/Plug-Ins/HAL"',
            'struct x9_haptic_hal_file { const char *name; uint32_t bytes; const uint8_t *data; uint8_t sha[32]; };']
    for i, (_, data, _) in enumerate(files):
        text.append(f'static const uint8_t x9_haptic_hal_data_{i}[] = {{')
        text.extend(','.join(f'0x{b:02x}' for b in data[o:o+16]) + ',' for o in range(0, len(data), 16))
        text.append('};')
    text.append('static const struct x9_haptic_hal_file x9_haptic_hal_files[] = {')
    for i, (name, data, digest) in enumerate(files):
        sha = ','.join(f'0x{b:02x}' for b in digest)
        text.append(f'{{"{name}",{len(data)},x9_haptic_hal_data_{i},{{{sha}}}}},')
    text.append('};\n')
    header = X9 / 'kernel/generated/x9_haptic_hal.inc'
    header.parent.mkdir(parents=True, exist_ok=True)
    content = '\n'.join(text)
    if not header.exists() or header.read_text() != content:
        header.write_text(content)
    (a.out / 'haptic-build.json').write_text(json.dumps({
        'identity': identity.hexdigest(), 'source': source, 'uid': 'Actuator',
        'input_format': '48000Hz mono S16_LE', 'native_format': '12000Hz signed8 RTP',
        'route': 'original iOS VirtualAudio / d22_actuator_general',
        'files': [{'name': n, 'bytes': len(d), 'sha256': s.hex()} for n, d, s in files],
        'hardware_tested': False, 'route_tested': False, 'installation_executed': False}, indent=2)+'\n')
    print(f'X9 Actuator HAL signed and embedded: {a.out}; no device IO', flush=True)


if __name__ == '__main__':
    main()
