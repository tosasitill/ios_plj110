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
                  '-framework', 'CoreVideo', '-framework', 'VideoToolbox',
                  '-framework', 'CoreGraphics']
    library = a.out / 'libx9-hevc.dylib'
    subprocess.run(common + ['-dynamiclib', str(HERE / 'vt_decoder.c'),
                            str(HERE / 'vt_picture.c'),
                            str(HERE / 'hevc_bitstream.c'),
                            str(HERE / 'heif_items.c'),
                            str(HERE / 'heif_compositor.c'),
                            str(HERE / 'heif_coremedia.c'),
                            str(HERE / 'heif_metadata.c'),
                            str(HERE / 'heif_native.c'),
                            str(HERE / 'vt_image_metadata.c'), *frameworks,
                            '-install_name', '@rpath/libx9-hevc.dylib', '-o', str(library)], check=True)
    ctl = a.out / 'vdecctl'
    subprocess.run(common + [str(HERE / 'vdecctl.c'), str(HERE / 'hevc_bitstream.c'),
                            str(HERE / 'heif_items.c'),
                            str(HERE / 'heif_compositor.c'),
                            str(library), *frameworks, '-Wl,-rpath,@executable_path', '-o', str(ctl)], check=True)
    for p in (library, ctl):
        subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(p)], check=True)
    (a.out / 'hevc-bridge.json').write_text(json.dumps({
        'profile': '20H392 VT base v1/decoder v2; native base-layer hvc1 sessions',
        'physical_provider': False, 'hardware_tested': False,
        'automatic_injection': False, 'required_specification': {'X9NativeHEVC': True},
        'decoder_id': 'org.ios-sweet.x9.vcp.hvc1',
        'heif_coded_item_input': True, 'heif_complete_image_compositor': False,
        'heif_item_offset_input': True,
        'heif_derived_geometry': True, 'heif_packed_raster_compositor': True,
        'heif_native_raster_adapter': True, 'heif_auxiliary_merge': False,
        'heif_explicit_working_rgb_icc': True, 'heif_bitstream_color_resolver': True,
        'hevc_complete_supported_sps_syntax': True,
        'heif_mixed_derived_color_contexts': False,
        'heif_explicit_per_item_color_normalization': True,
        'heif_graph_color_default_policy': 'inherit_identical',
        'heif_per_item_color_interpretation': 'own colr and selected SPS to common ICC; derived colr retained only',
        'heif_per_item_color_runtime_verified': False,
        'heif_fractional_clap_linear_resampler': True,
        'heif_fractional_clap_default_policy': 'exact_samples',
        'heif_fractional_clap_matrix_working_icc_required': False,
        'heif_fractional_clap_explicit_matrix_intermediate_icc': True,
        'heif_fractional_clap_linearization': 'explicit matrix intermediate ICC or matrix working ICC; no default gamut',
        'heif_fractional_clap_lut_working_icc_runtime_verified': False,
        'heif_explicit_raster_heap_budget': True,
        'library': str(library), 'client': str(ctl),
    }, indent=2) + '\n')
    print('X9 HEVC: iOS decoder/client compiled; physical VCP provider absent, no auto-injection', flush=True)

if __name__ == '__main__':
    main()
