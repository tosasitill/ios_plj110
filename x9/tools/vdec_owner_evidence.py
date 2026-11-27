#!/usr/bin/env python3
"""Qualify bounded MT6993 KO owner-selection instructions, without loading.

The caller supplies the existing whole-file-qualified capture report. Its
identity is RETAINED evidence, not a fresh whole-file hash of either input.
This reader independently checks selected instruction bytes, function anchors,
the file build-id note and relocation-linked symbols. It never upgrades that
scope to live text, current flags, firmware pairing, IRQ or NS/MMIO authority.
No device calls, sample execution, arbitrary disassembler or scratch imports.
"""
import argparse
import importlib.util
import json
import os
import stat
from pathlib import Path


TOOL_DIR = Path(__file__).resolve().parent
REPO = TOOL_DIR.parent.parent

# Maintained ELF/RELA reader only; no scratch dependency or main() execution.
_spec = importlib.util.spec_from_file_location('_vcp_owner_elf', TOOL_DIR / 'vcp_artifacts.py')
_elf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_elf)

IDENTITIES = {
    'decoder': ('mtk-vcodec-dec.ko', 607352,
        '4de1ff25c4b16936bc487f6176d9feda24b4b9c85254f4921d4f8f330eb2bae0',
        'a5f63a89f1658568bb7ffc2ae837e96df2361ea1', 'decoder_common_evidence'),
    'common': ('mtk-vcodec-common.ko', 172288,
        'de863471e77b05cdec514c7c523c12a785fc1c81e603bdf222a6054f987b7de5',
        'aca809858dda2fc48a73dfa661324e402d0361a9', 'common_vp_evidence'),
}

ANCHORS = {
    'decoder': {
        'mtk_vcodec_dec_probe': (0xfd88, 0x1398),
        'vdec_decode_prepare': (0x16590, 0x134),
        'vdec_decode_unprepare': (0x166c8, 0x1c4),
        'vdec_if_dev_ctx_init': (0x15a70, 0x194),
        'vdec_if_init': (0x15c44, 0x254),
        'mtk_vdec_smi_pwr_ctrl': (0x14c28, 0x25c),
        'mtk_vcodec_dec_smi_pwr_ctrl_register': (0x13e48, 0x134),
    },
    'common': {
        'mtk_vcodec_dec_irq_setup': (0x16e4, 0x2c0),
        'mtk_vcodec_is_vcp': (0x22a0, 0x1c),
    },
}

# Exact selected bytes include unresolved relocation opcodes. Their symbols
# and addends are separately checked through the relocation's OWN symtab.
WINDOWS = {
    'decoder': [
        ('power_field_address', 'mtk_vcodec_dec_probe', 0xfeb8,
         '0a718452087400f92900c0d217000a8b087800f908d08452f30300aa'),
        ('power_field_spill', 'mtk_vcodec_dec_probe', 0x103bc,
         'f7e700a97b02088b'),
        ('probe_irq_setup_call', 'mtk_vcodec_dec_probe', 0x107bc,
         'e00314aae10313aa00000094fc03002a00070034'),
        ('probe_vcp_call_and_power_property', 'mtk_vcodec_dec_probe', 0x10fe4,
         'e00313aaa812043900000094807e41f90100009021000091e2630091'
         '23008052e4031faa00000094e81b40b9090000901f00007129014039'
         '04a9407ae80740f9e4079f1a3f05007104010039'),
        ('probe_post_property_consumers', 'mtk_vcodec_dec_probe', 0x11098,
         'e00313aa00000094e00313aa00000094fc03002a80010035'),
        ('prepare_power_gate', 'vdec_decode_prepare', 0x165ac,
         '3f0c007108080054080440f909718452f30300aa0869693868070037'),
        ('unprepare_power_gate', 'vdec_decode_unprepare', 0x166e8,
         '3f0c0071c8080054080440f909718452f30300aa0869693828080037'),
        ('device_context_selection', 'vdec_if_dev_ctx_init', 0x15b34,
         '0800009075da02f9080140f9880000b4e0031f2a0000009460000036'
         '00000094020000140000009460d202f9'),
        ('codec_context_selection', 'vdec_if_init', 0x15df8,
         '08000090080140f9880000b4e0031f2a000000948003003600000094'
         '603a02f9800300b4080040f961021291e00313aa10c15fb8f1218972'
         '7164b2721f02116b40000054004530d400013fd6'),
        ('codec_context_vcu_selection', 'vdec_if_init', 0x15e7c,
         '00000094603a02f9c0fcffb5'),
        ('smi_two_power_flags', 'mtk_vdec_smi_pwr_ctrl', 0x14c5c,
         '16084091a8831ff8'),
        ('smi_power_flags_log', 'mtk_vdec_smi_pwr_ctrl', 0x14c84,
         'c6224e39c7264e39'),
        ('smi_owner_dispatch_gate', 'mtk_vdec_smi_pwr_ctrl', 0x14d08,
         '68000034c8264e39c8010036bf0a0071e000005415030035'),
        ('smi_vcp_power_call', 'mtk_vdec_smi_pwr_ctrl', 0x14d48,
         'e0031f2aff1700b9000000940007003680c20491e2330091c1038052'
         'f5cf012900000094e81740b91f00007100b0881a'),
        ('smi_register_power_gate', 'mtk_vcodec_dec_smi_pwr_ctrl_register', 0x13e48,
         '08718452086868381f050071610300543f2303d5fd7bbda9f50b00f9f44f02a9'),
    ],
    'common': [
        ('irq_index_loop', 'mtk_vcodec_dec_irq_setup', 0x1718,
         '08ca8452f5031faaf6031f2a9b02088b'),
        ('irq_platform_read_and_negative_gate', 'mtk_vcodec_dec_irq_setup', 0x1768,
         'e00313aae103152a00000094e503002a607b35b80002f837'),
        ('irq_missing_zero_index', 'mtk_vcodec_dec_irq_setup', 0x17bc, 'd50a00b4'),
        ('irq_initial_disable', 'mtk_vcodec_dec_irq_setup', 0x1754,
         '607b75b800000094b5060091bf1600f1c0100054'),
        ('irq_request', 'mtk_vcodec_dec_irq_setup', 0x1860,
         'e3031faa84108052e60314aa00000094f603002a16f7ff34'),
        ('irq_missing_zero_return', 'mtk_vcodec_dec_irq_setup', 0x195c,
         '08000090e0031f2a080140391f050071a10000540000009000000091'
         '00000094e0031f2a'),
        ('is_vcp_current_flag', 'mtk_vcodec_is_vcp', 0x22a0,
         '080000901f080071080140b9e9279f1a0825c01a2001080ac0035fd6'),
    ],
}

RELAS = {
    'decoder': [
        (0x107c4, (283, 'mtk_vcodec_dec_irq_setup', None, 0, 0, 0)),
        (0x10fec, (283, 'vdec_vcp_probe', '.text', 2, 0x23bf8, 0)),
        (0x10ff4, (275, '', '.rodata', 3, 0, 0x278)),
        (0x10ff8, (277, '', '.rodata', 3, 0, 0x278)),
        (0x11008, (283, 'of_property_read_variable_u32_array', None, 0, 0, 0)),
        (0x1109c, (283, 'mtk_vcodec_dec_smi_pwr_ctrl_register', '.text', 2, 0x13e48, 0)),
        (0x110a4, (283, 'vdec_if_dev_ctx_init', '.text', 2, 0x15a70, 0)),
        (0x15b34, (275, 'vcu_func', None, 0, 0, 0)),
        (0x15b3c, (286, 'vcu_func', None, 0, 0, 0)),
        (0x15b48, (283, 'mtk_vcodec_is_vcp', None, 0, 0, 0)),
        (0x15b50, (283, 'get_dec_vcp_if', '.text', 2, 0x24d3c, 0)),
        (0x15b58, (283, 'get_dec_vcu_if', '.text', 2, 0x1cf44, 0)),
        (0x15df8, (275, 'vcu_func', None, 0, 0, 0)),
        (0x15dfc, (286, 'vcu_func', None, 0, 0, 0)),
        (0x15e08, (283, 'mtk_vcodec_is_vcp', None, 0, 0, 0)),
        (0x15e10, (283, 'get_dec_vcp_if', '.text', 2, 0x24d3c, 0)),
        (0x15e7c, (283, 'get_dec_vcu_if', '.text', 2, 0x1cf44, 0)),
        (0x14d50, (283, 'mtk_vcodec_is_vcp', None, 0, 0, 0)),
        (0x14d68, (283, 'vdec_if_set_param', '.text', 2, 0x1621c, 0)),
    ],
    'common': [
        (0x1770, (283, 'platform_get_irq', None, 0, 0, 0)),
        (0x1758, (283, 'disable_irq', None, 0, 0, 0)),
        (0x186c, (283, 'devm_request_threaded_irq', None, 0, 0, 0)),
        (0x22a0, (275, 'mtk_vcodec_vcp', '.bss', 1, 0x6c, 0)),
        (0x22a8, (285, 'mtk_vcodec_vcp', '.bss', 1, 0x6c, 0)),
    ],
}


def read_regular(path, limit):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as f:
        before = os.fstat(f.fileno())
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= limit:
            raise ValueError('require bounded regular file')
        blob = f.read(limit + 1)
        after = os.fstat(f.fileno())
        if len(blob) != before.st_size or (before.st_size, before.st_mtime_ns) != (
                after.st_size, after.st_mtime_ns):
            raise ValueError('input changed during read')
        return blob


def section(elf, name):
    matches = [(i, s) for i, s in enumerate(elf.sections) if s['name'] == name]
    if len(matches) != 1:
        raise ValueError('require unique section: ' + name)
    return matches[0]


def qualify(kind, path, report):
    leaf, length, digest, build_id, profile = IDENTITIES[kind]
    retained = [r for r in report.get('modules', [])
                if (r.get('bytes'), r.get('sha256'), r.get('build_id')) ==
                (length, digest, build_id)]
    if len(retained) != 1 or retained[0].get(profile, {}).get('profile_match') is not True:
        raise ValueError('missing exact retained capture identity/profile for ' + kind)
    blob = read_regular(path, 2 << 20)
    if len(blob) != length:
        raise ValueError('module length differs from retained original')
    elf = _elf.ELF(blob)
    _, note = section(elf, '.note.gnu.build-id')
    if _elf.gnu_build_id(elf.data(note)) != build_id:
        raise ValueError('original file build-id note differs')
    index, sec = section(elf, '.text')
    if sec['type'] != 1:
        raise ValueError('require PROGBITS text')
    text, anchors, evidence = elf.data(sec), {}, []
    for name, (start, size) in ANCHORS[kind].items():
        symbols = [s for s in elf.symbols if (s['name'], s['section'], s['type'],
                   s['value'], s['bytes']) == (name, index, 2, start, size)]
        if len(symbols) != 1:
            raise ValueError('selected function anchor mismatch: ' + name)
        anchors[name] = dict(section='.text', offset=start, bytes=size)
    for label, name, start, expected in WINDOWS[kind]:
        raw = bytes.fromhex(expected)
        lo, size = ANCHORS[kind][name]
        if start % 4 or len(raw) % 4 or not lo <= start < start + len(raw) <= lo + size:
            raise ValueError('unaligned or out-of-function selected window')
        if _elf.span(text, start, len(raw)) != raw:
            raise ValueError('selected original bytes mismatch: ' + label)
        evidence.append(dict(name=label, function=name, section='.text', start=start,
            end_exclusive=start + len(raw), file_offset=sec['offset'] + start,
            raw_hex=raw.hex()))
    # No flattened symtab assumptions: each RELA resolves its linked table.
    relocations = [_elf.captured_rela(elf, '.text', at, expected)
                   for at, expected in RELAS[kind]]
    if kind == 'decoder':
        _, ro = section(elf, '.rodata')
        if _elf.span(elf.data(ro), 0x278, 18) != b'vdec-power-in-vcp\0':
            raise ValueError('power property linked literal differs')
    return dict(source=str(path), original_basename=leaf,
        retained_capture_identity=dict(bytes=length, sha256=digest, build_id=build_id,
            source_claim=retained[0].get('source'), profile=profile),
        file_build_id_note_matches=True, retained_identity_matches_expected=True,
        current_whole_file_hash_rechecked=False, current_whole_file_identity_proven=False,
        selected_ranges_match=True, anchors=anchors,
        instruction_evidence=evidence, relocations=relocations)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--decoder', type=Path, required=True)
    p.add_argument('--common', type=Path, required=True)
    p.add_argument('--capture-report', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True, help='NEW repository x9/out directory')
    args = p.parse_args()
    out = args.out.resolve()
    private = (REPO / 'x9/out').resolve()
    if out == private or private not in out.parents:
        p.error('output must be a NEW child of this checkout x9/out')
    out.mkdir(parents=True, exist_ok=False)
    try:
        report = json.loads(read_regular(args.capture_report, 4 << 20))
        modules = {kind: qualify(kind, path, report) for kind, path in (
            ('decoder', args.decoder), ('common', args.common))}
        result = dict(schema=1,
            identity_scope='retained whole-file identity plus fresh selected ranges/RELA only',
            retained_report_source=str(args.capture_report), modules=modules,
            file_selection_abi_proven=True, runtime_ready=False,
            runtime_dec_if_selection_proven=False, runtime_power_flag_value_proven=False,
            runtime_vcp_mode_value_proven=False, module_firmware_pairing_proven=False,
            runtime_irq_owner_proven=False, runtime_irq_completion_proven=False,
            runtime_ns_authority_proven=False, native_cold_provider_implemented=False,
            device_action_performed=False,
            power_selection=dict(device_power_in_vcp_offset=0x2388,
                device_power_in_kernel_offset=0x2389,
                property='vdec-power-in-vcp', property_literal_offset=0x278,
                property_writer='nonnegative u32 reader result AND nonzero value; bool store',
                prepare_and_unprepare_skip_before_lock_owner_clock_when_bit0_set=True,
                probe_order=['IRQ setup', 'VCP probe', 'power property write',
                    'SMI callback registration', 'device-context interface selection']),
            interface_selection=dict(context_interface_offset=0x470,
                context_handle_offset=0x480, device_context_offset=0x130,
                device_context_interface_offset=0x5a0,
                vcp_if_selected_if='vcu_func first pointer NULL OR mtk_vcodec_is_vcp(0) bit0 set',
                vcu_if_selected_if='vcu_func first pointer non-NULL AND vcp flag bit0 clear',
                common_vcp_flag_symbol='mtk_vcodec_vcp', common_vcp_flag_nobits_offset=0x6c,
                common_vcp_flag_nobits_is_not_runtime_zero=True),
            irq_setup=dict(first_negative_irq_returns_zero=True,
                setup_return0_is_not_irq_registration=True,
                registered_irq_initially_disabled=True, register_addresses_inferred=False),
            smi_callback=dict(is_debug_power_callback_not_cold_bootstrap=True,
                parameter_id=30, parameter_input_bytes=12,
                vcp_route_if='power_in_vcp nonzero AND power_in_kernel bit0 clear, after special branches',
                vcp_route_requires_common_vcp_flag_bit0=True,
                call='vdec_if_set_param',
                return_semantics='negative call result, otherwise supplied info.ret',
                no_clock_error_or_firmware_success_claim=True))
        with (out / 'owner-selection.json').open('x') as f:
            json.dump(result, f, indent=2); f.write('\n')
        print(json.dumps(dict(output=str(out / 'owner-selection.json'),
            selected_windows=sum(len(WINDOWS[k]) for k in WINDOWS),
            linked_relocations=sum(len(RELAS[k]) for k in RELAS), runtime_ready=False)))
    except (ValueError, OSError, KeyError, TypeError) as e:
        with (out / 'error.json').open('x') as f:
            json.dump(dict(error=str(e), runtime_ready=False), f); f.write('\n')
        raise


if __name__ == '__main__':
    main()
