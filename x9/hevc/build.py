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
        'heif_standalone_picture_gate': 'Main/Main10 base-layer BLA/IDR/CRA; Main-compatible Main Still Picture IDR-only',
        'heif_standalone_picture_dispatch': 'fresh native SESSION_CREATE; SUBMIT once; DRAIN/RECEIVE through ENODATA',
        'heif_standalone_irap_runtime_verified': False,
        'heif_hdr_properties': 'item-local mdcv24/clli4/amve8; exact CM format and hardware callback metadata identity; amve is nominal source environment, not a destination condition',
        'heif_hdr_sei': 'this standalone AU base-layer direct/resolved-scalable prefix SEI137/144/147/148; direct Tid equals VCL, nested Tid0/MaxTid6; canonical EBSP and independent logical child bounds; at most8 agreeing base HDR repeats per type',
        'heif_preferred_transfer': 'raw SEI147 and standard CM alternative string; supersedes selected-SPS transfer fallback, explicit non-2 container and ICC policy retained; actual source-view transfer/gamma consumed by VT pixel transfer',
        'heif_ambient_viewing_environment': 'item-local amve and resolved SEI148, exact8 big-endian; nonzero illuminance, xy<=50000; duplicates agree; original source info and CM/CV identity; not a tone mapper or derived destination environment',
        'heif_hdr_sei_sources': {'direct': 1, 'scalable_layers': 2, 'scalable_operations': 4},
        'heif_hdr_conflict_policy': 'container and resolved base-layer SEI must match; absent stays absent',
        'heif_hdr_converted_rgb_metadata': 'retain original in Items, not relabelled on derived ICC pixels',
        'heif_hdr_runtime_verified': False,
        'hevc_persistent_hdr_sei_state': False,
        'hevc_direct_hdr_forward_state': True,
        'hevc_scoped_hdr_forward_state': True,
        'hevc_preferred_transfer_forward_state': True,
        'hevc_preferred_transfer_metadata': 'raw u8 identity, no CFString alias inversion; first CLVS IRAP/persistent exact, SUBMIT-success-only; CM alternative to effective owned CV transfer/gamma; CICP4/5 gamma2.2/2.8; unknown raw does not become a guessed label',
        'hevc_preferred_transfer_runtime_verified': False,
        'hevc_ambient_viewing_environment_forward_state': True,
        'hevc_ambient_viewing_environment_state': 'first CLVS IRAP presence; exact persistent value; SUBMIT-success-only commit; genuine CLVS reset, DRAIN retains',
        'hevc_ambient_viewing_environment_runtime_verified': False,
        'hevc_hdr_sequence_boundaries': 'IDR/BLA, first IRAP or IRAP after EOS; middle CRA/no_output and DRAIN do not reset',
        'hevc_hdr_state_commit': 'SUBMIT success only; SEI-only state, CM/container exact-conflict policy; original-token immutable merged description',
        'hevc_hdr_late_clli': 'accepted without a first-picture restriction; SUBMIT-success-only atomic replacements of same-CLVS pending token descriptions, plus current/future inputs; emitted pixels not relabelled',
        'hevc_hdr_late_clli_pending_backfill': True,
        'hevc_hdr_cm_light_history': 'bounded exact constraints from accepted original CM/container values, including emitted frames; reset only on genuine CLVS boundary/cancel/fault/new-session',
        'hevc_hdr_late_clli_retroactive': False,
        'hevc_hdr_nested_scope': False,
        'hevc_hdr_scalable_nesting_scope': True,
        'hevc_hdr_scalable_scope': 'D.2.24/D.3.24 layers/operation layer sets; actual picture PPS->SPS->VPS, union base-layer membership; non-base HDR not promoted',
        'hevc_hdr_vps_scope_query': 'bounded selected VPS prefix through layer sets only; not full VPS/HRD/extensions validation or multilayer HW capability',
        'hevc_hdr_other_nesting_families': False,
        'hevc_hdr_runtime_verified': False,
        'heif_item_offset_input': True,
        'heif_derived_geometry': True, 'heif_packed_raster_compositor': True,
        'heif_iovl_signed_offset_input': True, 'heif_iovl_linear_source_over': True,
        'heif_iovl_native_canvas_rgb_transfer': True, 'heif_iovl_runtime_verified': False,
        'heif_premultiplied_source_decode': False,
        'heif_linear_export_alpha': 'preserve CPU alpha; convert opaque unassociated RGB; owned straight RGBA-half',
        'heif_native_raster_adapter': True, 'heif_auxiliary_merge': False,
        'heif_explicit_working_rgb_icc': True, 'heif_bitstream_color_resolver': True,
        'hevc_complete_supported_sps_syntax': True,
        'heif_mixed_derived_color_contexts': False,
        'heif_explicit_per_item_color_normalization': True,
        'heif_graph_color_default_policy': 'inherit_identical',
        'heif_per_item_color_interpretation': 'own colr and selected SPS to common ICC; iovl colr interprets RGB fill only',
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
