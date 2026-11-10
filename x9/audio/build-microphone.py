#!/usr/bin/env python3
"""Build/sign the iOS microphone HAL, embed its complete bundle, and build micctl.

No host-side install/device IO. PID1 publishes the embedded bundle at boot.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
X9 = HERE.parent


def embed_bundle(bundle):
    # Use the signed bundle as one indivisible version. CodeResources and plist
    # are included, not just its Mach-O. No old signed file is overwritten.
    files = []
    identity = hashlib.sha256()
    for path in sorted(bundle.rglob('*')):
        if path.is_symlink():
            raise ValueError('symlinks are not allowed in the embedded HAL')
        if not path.is_file():
            continue
        name = path.relative_to(bundle).as_posix()
        if name not in ('Info.plist', 'LICENSE.hakios', 'X9Microphone',
                        '_CodeSignature/CodeResources'):
            raise ValueError(f'unexpected signed HAL entry: {name}')
        data = path.read_bytes()
        if not data:
            raise ValueError(f'empty HAL entry: {name}')
        digest = hashlib.sha256(data).digest()
        identity.update(name.encode() + b'\0' + len(data).to_bytes(8, 'little') + digest)
        files.append((name, data, digest))
    if len(files) != 4 or sum(len(data) for _, data, _ in files) > 512 * 1024:
        raise ValueError('incomplete signed HAL or 512 KiB embedding budget exceeded')
    root = '/private/var/x9/microphone/' + identity.hexdigest()
    source = root + '/hal'
    text = ['/* Generated signed own-code HAL bundle. Do not commit. */',
            f'#define X9_MIC_HAL_SOURCE "{source}"',
            '#define X9_MIC_HAL_POINT "/System/Library/Audio/Plug-Ins/HAL"',
            'struct x9_mic_hal_file { const char *name; uint32_t bytes; '
            'const uint8_t *data; uint8_t sha[32]; };']
    for i, (_, data, _) in enumerate(files):
        text.append(f'static const uint8_t x9_mic_hal_data_{i}[] = {{')
        text.extend(','.join(f'0x{b:02x}' for b in data[off:off+16]) + ','
                    for off in range(0, len(data), 16))
        text.append('};')
    text.append('static const struct x9_mic_hal_file x9_mic_hal_files[] = {')
    for i, (name, data, digest) in enumerate(files):
        sha = ','.join(f'0x{b:02x}' for b in digest)
        text.append(f'{{"{name}", {len(data)}, x9_mic_hal_data_{i}, {{{sha}}}}},')
    text.append('};\n')
    header = X9 / 'kernel/generated/x9_microphone_hal.inc'
    header.parent.mkdir(parents=True, exist_ok=True)
    content = '\n'.join(text)
    if not header.exists() or header.read_text() != content:
        header.write_text(content)
    return {'identity': identity.hexdigest(), 'bytes': sum(len(d) for _, d, _ in files),
            'source': source, 'mountpoint': '/System/Library/Audio/Plug-Ins/HAL',
            'files': [{'name': n, 'bytes': len(d), 'sha256': s.hex()} for n, d, s in files]}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--developer-dir', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    dev = a.developer_dir
    sdk = dev / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
    macsdk = dev / 'Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
    clang = dev / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    bundle = a.out / 'X9Microphone.driver'
    if bundle.exists():
        ap.error('output bundle already exists; use a new output directory')
    bundle.mkdir(parents=True)
    # Flat iOS CFBundle, matching the UniXNU/HakiOS on-device HAL layout.
    shutil.copyfile(HERE / 'microphone.plist', bundle / 'Info.plist')
    shutil.copyfile(HERE / 'LICENSE.hakios', bundle / 'LICENSE.hakios')
    flags = [str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk),
             '-Os', '-Wall', '-Wextra', '-Werror']
    binary = bundle / 'X9Microphone'
    # Like UniXNU-sheng's HAL builder: use Apple SDK declarations rather than
    # guessing the undocumented-in-iOS vtable. Headers remain local, not in git.
    with tempfile.TemporaryDirectory() as tmp:
        headers = Path(tmp) / 'CoreAudio'
        headers.mkdir()
        for name in ('AudioServerPlugIn.h', 'AudioHardwareBase.h', 'AudioHardware.h'):
            (headers / name).symlink_to(macsdk / 'System/Library/Frameworks/CoreAudio.framework/Headers' / name)
        subprocess.run(flags + ['-I', tmp, '-bundle', '-framework', 'CoreFoundation',
                               str(HERE / 'microphone_plugin.c'), '-o', str(binary)], check=True)
    ctl = a.out / 'micctl'
    subprocess.run(flags + [str(HERE / 'micctl.c'), '-o', str(ctl)], check=True)
    for target in (bundle, ctl):
        subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(target)], check=True)
    embedded = embed_bundle(bundle)
    (a.out / 'microphone-build.json').write_text(json.dumps({
        'abi': 1, 'format': '48000 Hz / mono / S16_LE', 'uid': 'Codec', 'source': 'imic',
        'hal_bundle': str(bundle), 'installed': False,
        'board_prerequisites_implemented': False, 'board_capture_lifecycle_implemented': True,
        'dma_provider_registered': False, 'plj110_cold_sessions_per_boot': 1,
        'exact_recording_frames_required': True, 'kernel_stop_state_checked': True,
        'exact_nonblocking_read_contract_required': 1,
        'passive_status_discovery': True, 'status_discovery_interval_ms': 500,
        'realtime_fault_notifications_deferred': True,
        'enumeration_empty_lists_supported': True,
        'codec_masked_restore_guard': True, 'codec_restore_fault_location': True,
        'cold_power_probe_control': True,
        'power_probe_activates_recording': False, 'hardware_tested': False,
        'route_tested': False, 'autoload': 'PID1 read-only union mount in microphone mode2',
        'embedded_hal': embedded, 'micctl_embedded': False,
        'installation_executed': False}, indent=2) + '\n')
    print(f'X9 microphone HAL embedded + micctl built: {a.out}; no device IO/hardware test', flush=True)


if __name__ == '__main__':
    main()
