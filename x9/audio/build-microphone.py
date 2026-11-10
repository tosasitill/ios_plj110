#!/usr/bin/env python3
"""Cross-compile/sign input-only iOS HAL bundle and micctl. No install/device IO."""
import argparse
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


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
    (a.out / 'microphone-build.json').write_text(json.dumps({
        'abi': 1, 'format': '48000 Hz / mono / S16_LE', 'uid': 'Codec', 'source': 'imic',
        'hal_bundle': str(bundle), 'installed': False,
        'board_prerequisites_implemented': False, 'board_capture_lifecycle_implemented': True,
        'dma_provider_registered': False, 'plj110_cold_sessions_per_boot': 1,
        'exact_recording_frames_required': True, 'kernel_stop_state_checked': True,
        'cold_power_probe_control': True,
        'power_probe_activates_recording': False, 'hardware_tested': False,
        'route_tested': False, 'autoload': False}, indent=2) + '\n')
    print(f'X9 microphone HAL + micctl built: {a.out}; not installed/hardware-tested', flush=True)


if __name__ == '__main__':
    main()
