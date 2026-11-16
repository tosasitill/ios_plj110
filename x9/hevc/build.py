#!/usr/bin/env python3
"""Build/sign the iOS VT bridge, optionally embed its own bytes. No device IO.

Embedding does not grant hardware readiness or enable boot-time installation.
The opt-in kernel loader and VT registration both require real capabilities.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--developer-dir', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--embed', action='store_true')
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
    installed_path = None
    if a.embed:
        # Producing the embedding is part of this build, not a second artifact
        # verification pass. Only the signed own-code library is read here.
        data = library.read_bytes()
        if not data or len(data) > 2 * 1024 * 1024:
            raise ValueError('HEVC bridge outside the 2 MiB embedding budget')
        digest = hashlib.sha256(data).digest()
        installed_path = f'/private/var/x9/hevc/libx9-hevc-{digest.hex()[:16]}.dylib'
        header = HERE.parent / 'kernel/generated/x9_hevc_bridge.inc'
        lines = ['/* Generated signed own-code bridge. Do not commit. */',
                 f'#define X9_HEVC_BRIDGE_PATH "{installed_path}"',
                 'static const uint8_t x9_hevc_bridge_sha[32] = {' +
                 ','.join(f'0x{b:02x}' for b in digest) + '};',
                 'static const uint8_t x9_hevc_bridge_data[] = {']
        lines += [','.join(f'0x{b:02x}' for b in data[i:i+16]) + ','
                  for i in range(0, len(data), 16)]
        text = '\n'.join(lines) + '\n};\n'
        if not header.exists() or header.read_text() != text:
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(text)
    (a.out / 'hevc-bridge.json').write_text(json.dumps({
        'profile': '20H392 VT base v1/decoder v2; native base-layer hvc1 sessions',
        'physical_provider': False, 'hardware_tested': False,
        'hevc_session_output_delivery': 'native PEEK retains CPU-owned frame/token across payload and ioctl result copyout; exact input-only ACK before VT token/output-order mutation',
        'hevc_session_output_delivery_runtime_verified': False,
        'automatic_injection': False,
        'automatic_injection_code': True,
        'automatic_injection_policy': 'opt-in x9_hevc=1, installed exact own-code file and live native capabilities; selected processes only',
        'automatic_injection_runtime_verified': False,
        'embedded': a.embed, 'ios_path': installed_path,
        'required_specification': None,
        'decoder_id': 'org.ios-sweet.x9.vcp.hvc1',
        'heif_coded_item_input': True, 'heif_complete_image_compositor': False,
        'heif_standalone_picture_gate': 'Main/Main10 base-layer BLA/IDR/CRA; Main-compatible Main Still Picture IDR-only',
        'heif_standalone_picture_dispatch': 'fresh native SESSION_CREATE; SUBMIT once; DRAIN/PEEK/ACK through ENODATA',
        'heif_standalone_irap_runtime_verified': False,
        'heif_hdr_properties': 'item-local mdcv24/clli4/amve8; exact CM format and hardware callback metadata identity; amve is nominal source environment, not a destination condition',
        'heif_hdr_sei': 'this standalone AU base-layer direct/resolved-scalable prefix SEI137/144/147/148; direct Tid equals VCL, nested Tid0/MaxTid6; canonical EBSP and independent logical child bounds; at most8 agreeing base HDR repeats per type',
        'heif_preferred_transfer': 'raw SEI147 and standard CM alternative string; supersedes selected-SPS transfer fallback, explicit non-2 container and ICC policy retained; actual source-view transfer/gamma consumed by VT pixel transfer',
        'heif_ambient_viewing_environment': 'item-local amve and resolved SEI148, exact8 big-endian; nonzero illuminance, xy<=50000; duplicates agree; original source info and CM/CV identity; not a tone mapper or derived destination environment',
        'heif_pasp_coded_sample_forwarding': 'ordinary8-byte Box; positive unsigned32 spacings, duplicate rejection; owned standard CM/CV keys, exact raw info, rational CM/callback identity; no sample resize',
        'heif_pasp_derived_geometry': False,
        'heif_pasp_agreed_grid_identity_lattice': True,
        'heif_pasp_agreed_overlay_lattice': True,
        'heif_pasp_identity_output_preservation': True,
        'heif_pasp_derived_exact_copy': 'absent-pasp iden preserves unique exact-size child ordered OUTPUT metric/presence as INPUT with identity origin4 and immediate child ID, then own transforms; explicit canvas/grid/overlay still check every child OUTPUT against INPUT and completed cached raster at sample-copy/source-over placement; signed overlay offsets stay sample coordinates without scaling; no grid/overlay absent inheritance, mixed-lattice resampling or non-square aux',
        'heif_pasp_untransformed_coded_raster': 'owned raster output spacings and standard CV aspect through Normal transfer, read-only rational output check; no resize or square-pixel claim',
        'heif_pasp_ordered_coded_raster_metric': 'present source pasp; mirror/integer crop retain, odd quarter turns swap, explicit linear-ceil exact aperture/output sample counts; cross-cancelled positive signed64 output ratio, E2BIG on unrepresentable metric; source CM stays pre-transform',
        'heif_pasp_raster_gate': 'explicit grid/iden/overlay mismatch or grid/overlay missing non-square placement metric rejects before raster callbacks/hardware; absent-pasp identity resolves unique exact-size child ordered output; all parent edges checked including DAG reuse and fully invisible overlay placements; visible cached overlay metric rechecked before linear blending; non-square aux rejects; no grid/overlay inheritance or absent/SPS square default',
        'heif_pasp_selected_sps_raster_metric': True,
        'heif_source_sample_aspect': 'pasp else known actual PPS->SPS VUI else observed CM-format-only value; separate origin0/1/2/3 and raw container metadata; no synthetic1:1; reserved/zero Extended_SAR without pasp rejects',
        'heif_source_aspect_preflight': 'immutable native plan queries actual CM source metadata per reachable coded item, no hardware or retained AU; color preflight and actual CM/CV source presence/ratio must re-match plan before composition',
        'heif_pasp_runtime_verified': False,
        'heif_hdr_sei_sources': {'direct': 1, 'scalable_layers': 2, 'scalable_operations': 4},
        'heif_hdr_conflict_policy': 'container and resolved base-layer SEI must match; absent stays absent',
        'heif_hdr_converted_rgb_metadata': 'retain original in Items, not relabelled on derived ICC pixels',
        'heif_hdr_runtime_verified': False,
        'heif_alpha_sei165_preflight': 'full prefix165 cancel/use-idc3/depth3/9..16-bit endpoints/incr/clip/type syntax; equal/reversed range legal, incr requires nonzero power-of-two distance; bounded payload tail and ignored reserved extension data; direct/scalable base scope and agreeing repeats; suffix165 reserved; raw per-AU facts, no guessed associated-primary depth or pixel normalization',
        'heif_alpha_sei165_consumer': 'existing still/HEIF HDR preflight and stateful VT admission explicitly reject resolved base alpha before decode/SUBMIT; output-order persistence and carrier/aux/prem remain unsupported',
        'heif_alpha_sei165_runtime_verified': False,
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
        'hevc_slice_picture_facts': 'actual selected PPS output/dependent/extra-header flags and SPS POC width/CTB addressing; all independent segments agree on POC LSB/output, dependent segments inherit; IDR LSB0 and absent output flag1 are syntax inferences',
        'hevc_poc_input_state': 'base-layer H265 8.3.1; previous Tid0 excludes RADL/RASL/*_N, asymmetric half-modulus wrap, signed32 full POC; genuine CLVS boundaries; SUBMIT-success-only commit',
        'hevc_poc_original_token_output_check': True,
        'hevc_poc_output_state': 'only correlated non-DROPPED pixel receipts with derived PicOutputFlag1 advance strictly increasing (CLVS,POC); no host sorting or PTS inference; client suppression does not erase physical observation',
        'hevc_pic_output_flag_zero': 'raw flag0 OR RASL with associated-IRAP NoRaslOutputFlag1 suppresses VT pixels only after actual original-token terminal RECEIVE; does not fabricate DROPPED/retirement',
        'hevc_associated_irap_output_eligibility': 'previous IRAP in decoding order, not previous output or only CLVS-start IRAP; middle CRA replaces NoRaslOutputFlag association without resetting CLVS/POC; SUBMIT-success-only state',
        'hevc_poc_runtime_verified': False,
        'hevc_ccv_output_state': False,
        'hevc_ccv_sei149_preflight': 'resolved base-layer prefix149 cancel/persistence/4 presence flags; raw signed32 primaries in original order and unscaled u32 luminance; independently bounded tail, ignored reserved bits/extensions, semantic duplicate agreement and direct/scalable scope; suffix149 reserved',
        'hevc_ccv_applicability': 'D.3.40 mandatory ignore conditions from actual selected-SPS VUI plus prospective CLVS SEI147, not CM/container labels; applicable messages including cancel reject before token/SUBMIT until output-order consumer exists; still preflight uses this AU SEI147',
        'hevc_ccv_runtime_verified': False,
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
    print('X9 HEVC: iOS bridge compiled; native capabilities required, no device installation performed', flush=True)

if __name__ == '__main__':
    main()
