#!/usr/bin/env python3
"""Cross-compile the explicit iOS VT client/decoder. No injection/device IO.

The MT6993 VCP physical provider is not implemented; hardware registration is
runtime-gated and ordinary VT sessions are excluded. Own binaries only.
"""
import argparse
import json
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--developer-dir', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    clang = a.developer_dir / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    sdk = a.developer_dir / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
    common = [str(clang), '-target', 'arm64-apple-ios16.0', '-isysroot', str(sdk),
              '-Os', '-Wall', '-Wextra', '-Werror', '-fvisibility=hidden']
    frameworks = ['-framework', 'CoreFoundation', '-framework', 'CoreMedia',
                  '-framework', 'CoreVideo', '-framework', 'VideoToolbox']
    library = a.out / 'libx9-hevc.dylib'
    subprocess.run(common + ['-dynamiclib', str(HERE / 'vt_decoder.c'),
                            str(HERE / 'hevc_bitstream.c'), *frameworks,
                            '-install_name', '@rpath/libx9-hevc.dylib', '-o', str(library)], check=True)
    ctl = a.out / 'vdecctl'
    subprocess.run(common + [str(HERE / 'vdecctl.c'), str(HERE / 'hevc_bitstream.c'),
                            str(library), *frameworks, '-Wl,-rpath,@executable_path', '-o', str(ctl)], check=True)
    for p in (library, ctl):
        subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(p)], check=True)
    (a.out / 'hevc-bridge.json').write_text(json.dumps({
        'profile': '20H392 VT base v1/decoder v2; independent hvc1 IDR only',
        'physical_provider': False, 'hardware_tested': False,
        'automatic_injection': False, 'required_specification': {'X9IndependentHEVC': True},
        'decoder_id': 'org.ios-sweet.x9.vcp-idr.hvc1',
        'library': str(library), 'client': str(ctl),
    }, indent=2) + '\n')
    print('X9 HEVC: iOS decoder/client compiled; physical VCP provider absent, no auto-injection', flush=True)

if __name__ == '__main__':
    main()
