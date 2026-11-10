#!/usr/bin/env python3
"""Build/sign iOS16 X9Audio.driver and audioctl locally; never install or access a device."""
import argparse
import hashlib
import json
import plistlib
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

def embed_bundle(bundle):
    """Embed this build's complete signed bundle, never a cached Mach-O alone."""
    allowed = ('Info.plist', 'LICENSE.UniXNU', 'X9Audio', '_CodeSignature/CodeResources')
    files = []
    identity = hashlib.sha256()
    for path in sorted(bundle.rglob('*')):
        if path.is_symlink():
            raise ValueError('symlinks are not allowed in the embedded speaker HAL')
        name = path.relative_to(bundle).as_posix()
        if path.is_dir():
            if name != '_CodeSignature':
                raise ValueError(f'unexpected signed HAL directory: {name}')
            continue
        if not path.is_file() or name not in allowed:
            raise ValueError(f'unexpected signed HAL entry: {name}')
        data = path.read_bytes()
        if not data or len(data) > 512 * 1024:
            raise ValueError(f'empty/oversized HAL entry: {name}')
        digest = hashlib.sha256(data).digest()
        identity.update(name.encode('ascii') + b'\0' + len(data).to_bytes(8, 'little') + digest)
        files.append((name, data, digest))
    if tuple(n for n, _, _ in files) != allowed or sum(len(d) for _, d, _ in files) > 512 * 1024:
        raise ValueError('incomplete signed HAL or 512 KiB embedding budget exceeded')
    source = '/private/var/x9/speaker/' + identity.hexdigest() + '/hal'
    if len(source.encode('ascii')) >= 128:
        raise ValueError('HAL source exceeds the kernel manifest contract')
    text = ['/* Generated complete signed own-code speaker HAL. Do not commit. */',
            f'const char x9_speaker_hal_source[X9_SPEAKER_HAL_SOURCE_CAP] = "{source}";']
    for i, (_, data, _) in enumerate(files):
        text.append(f'static const uint8_t x9_speaker_hal_data_{i}[] = {{')
        text.extend(','.join(f'0x{b:02x}' for b in data[o:o+16]) + ',' for o in range(0, len(data), 16))
        text.append('};')
    text.append('const struct x9_speaker_hal_file x9_speaker_hal_files[X9_SPEAKER_HAL_FILES] = {')
    for i, (name, data, digest) in enumerate(files):
        sha = ','.join(f'0x{b:02x}' for b in digest)
        text.append(f'{{"{name}",{len(data)},x9_speaker_hal_data_{i},{{{sha}}}}},')
    text.append('};\n')
    header = HERE.parent / 'kernel/generated/x9_speaker_hal.inc'
    header.parent.mkdir(parents=True, exist_ok=True)
    content = '\n'.join(text)
    if not header.exists() or header.read_text() != content:
        # Replace only complete output; an interrupted generator must not
        # leave a partial manifest for a later kernel compile.
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', dir=header.parent, delete=False) as f:
                temporary = Path(f.name)
                f.write(content)
            temporary.replace(header)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    return {'identity': identity.hexdigest(), 'source': source,
            'mountpoint': '/System/Library/Audio/Plug-Ins/HAL',
            'bytes': sum(len(d) for _, d, _ in files),
            'files': [{'name': n, 'bytes': len(d), 'sha256': s.hex()} for n, d, s in files]}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--developer-dir', type=Path)
    ap.add_argument('--embed', action='store_true', help='emit complete signed resources for a final kernel build; no installation')
    a = ap.parse_args()
    dev = a.developer_dir or Path(json.loads((HERE.parent / 'build/local.json').read_text())['developer_dir'])
    ios = dev / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
    mac = dev / 'Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
    clang = dev / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    bundle = a.out / 'X9Audio.driver'
    if bundle.exists() or (a.out / 'audioctl').exists():
        ap.error('output bundle/tool already exists; use a new output directory')
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
    subprocess.run(common + [str(HERE / 'audioctl.c'),
                             str(HERE.parent / 'kernel/src/bsd/dev/x9_tfa98xx.c'),
                             str(HERE.parent / 'kernel/src/bsd/dev/x9_tfa_multimsg.c'),
                             str(HERE.parent / 'kernel/src/bsd/dev/x9_tfa_re25.c'),
                             '-o', str(tool)], check=True)
    for path in (bundle, tool):
        subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(path)], check=True)
    embedded = embed_bundle(bundle) if a.embed else None
    (a.out / 'speaker-build.json').write_text(json.dumps({
        'hal_bundle': str(bundle), 'pcm_abi': 1, 'uid': 'X9SpeakerDevice',
        'embedded_hal': embedded, 'audioctl_embedded': False,
        'publication_opt_in': 'x9_speaker_hal=1 with x9_audio=1 or 2',
        'publication_default': False, 'installation_executed': False,
        'passive_ready_discovery': True, 'physical_backend_registered': None,
        'hardware_tested': False, 'route_tested': False}, indent=2) + '\n')
    print(f'Built {bundle} and {tool}; not installed, physical audio not verified')

if __name__ == '__main__':
    main()
