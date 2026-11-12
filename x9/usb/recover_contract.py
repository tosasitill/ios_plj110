#!/usr/bin/env python3
"""Recover bounded endpoint/request observations from stock D22/20H392.

This is a read-only analysis command, not a test or a loader. It emits evidence
and field widths, NOT callable C++ declarations. Apple bytes stay private.
Requires capstone; Mach-O parsing and the source SHA gate reuse prepare_native.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_native import CONTROLLER_VPTR, CONTROLLER_SLOTS, MachO, ROOT, SHA256

# Bounded original methods, not an entire kernel disassembly.
REGIONS = {
    'create_pipe_gated': (0xfffffff005cf0438, 0xfffffff005cf047c),
    'create_endpoint_gated': (0xfffffff005cf04c4, 0xfffffff005cf054c),
    'destroy_endpoint_gated': (0xfffffff005cf0594, 0xfffffff005cf05d0),
    'complete_request': (0xfffffff005cf069c, 0xfffffff005cf0750),
    'pipe_factory': (0xfffffff005ce548c, 0xfffffff005ce5550),
    'pipe_settings': (0xfffffff005ce5550, 0xfffffff005ce5994),
    'pipe_accessors': (0xfffffff005ce697c, 0xfffffff005ce69dc),
    'submit_normal': (0xfffffff005cf1008, 0xfffffff005cf1114),
    'submit_normal_success_return': (0xfffffff005cf135c, 0xfffffff005cf137c),
    'submit_isochronous': (0xfffffff005cf137c, 0xfffffff005cf1494),
    'request_allocate_free': (0xfffffff005cf15d4, 0xfffffff005cf1610),
    'pipe_sync_callback': (0xfffffff005ce79f4, 0xfffffff005ce7a0c),
    'control_write_callback': (0xfffffff005cea7ec, 0xfffffff005cea818),
    'otg_constructor': (0xfffffff00606318c, 0xfffffff0060631ac),
    'otg_submit_entry': (0xfffffff00606c5cc, 0xfffffff00606c614),
    'otg_submit_initialize': (0xfffffff00606cd4c, 0xfffffff00606cdd8),
    'otg_request_dma_acquire': (0xfffffff00606c848, 0xfffffff00606c864),
    'otg_build_descriptors': (0xfffffff006063bc8, 0xfffffff006063d60),
    'otg_count_delta': (0xfffffff006063f5c, 0xfffffff006063fd0),
    'otg_program_chunk': (0xfffffff00607119c, 0xfffffff0060711b0),
    'otg_out_count': (0xfffffff006070af8, 0xfffffff006070b20),
    'otg_in_count': (0xfffffff006072148, 0xfffffff006072170),
    'otg_abort_out_count': (0xfffffff006072df4, 0xfffffff006072e20),
    'otg_abort_in_count': (0xfffffff006073264, 0xfffffff006073290),
    'otg_complete_request': (0xfffffff00607177c, 0xfffffff006071804),
    'otg_complete_import': (0xfffffff0060742d0, 0xfffffff0060742dc),
    'otg_flush_completion': (0xfffffff00606e46c, 0xfffffff00606e494),
    'otg_submit_transfer_kind': (0xfffffff00606c72c, 0xfffffff00606c73c),
    'otg_out_retire': (0xfffffff006070e5c, 0xfffffff006070fcc),
    'otg_completion_drain': (0xfffffff006067974, 0xfffffff0060679d4),
    'control_input_setup': (0xfffffff005ce8854, 0xfffffff005ce8ed4),
    'control_process_setup': (0xfffffff005ce8fb4, 0xfffffff005ce91f8),
    'controller_setup_dispatch': (0xfffffff005cf9080, 0xfffffff005cf91d8),
    'otg_setup_producer': (0xfffffff00607030c, 0xfffffff00607032c),
    'otg_process_setup': (0xfffffff00606ffe0, 0xfffffff00606fff4),
    'controller_control_factory': (0xfffffff005cf0ff8, 0xfffffff005cf1008),
    'control_factory': (0xfffffff005ce7f90, 0xfffffff005ce8040),
    'control_init_fields': (0xfffffff005ce8040, 0xfffffff005ce8080),
    'control_in_request_producer': (0xfffffff005cea4e4, 0xfffffff005cea600),
    'control_in_zlp_qualification': (0xfffffff005cea6b8, 0xfffffff005cea6fc),
    'control_in_zlp_publish': (0xfffffff005cea778, 0xfffffff005cea7cc),
    'control_write_advance': (0xfffffff005cea7ec, 0xfffffff005cea818),
    'control_write_result': (0xfffffff005cea8e8, 0xfffffff005cea954),
    'control_write_ready': (0xfffffff005ceaa50, 0xfffffff005ceaa68),
    'control_write_final_callback': (0xfffffff005ceab44, 0xfffffff005ceabdc),
    'control_zlp_callback': (0xfffffff005ceae14, 0xfffffff005ceb008),
    'control_status_submit': (0xfffffff005ce9eec, 0xfffffff005ce9f78),
    'control_status_completion': (0xfffffff005ceabdc, 0xfffffff005ceac30),
    'control_status_state_notify': (0xfffffff005cead60, 0xfffffff005cead88),
    'control_in_status_final_callback': (0xfffffff005ce9ce8, 0xfffffff005ce9d18),
}
# Assertions are source-qualification checks, not claims of runtime success.
OBSERVED = {
    0xfffffff005cf06b4: ('ldr', 'w3, [x1, #0x28]'),
    0xfffffff005cf06ec: ('ldr', 'x8, [x19, #0x58]'),
    0xfffffff005cf06f4: ('ldr', 'x0, [x19, #0x50]'),
    0xfffffff005cf06f8: ('ldr', 'x1, [x19, #0x60]'),
    0xfffffff005cf06fc: ('ldr', 'w2, [x19, #0x28]'),
    0xfffffff005cf0704: ('blr', 'x8'),
    0xfffffff005cf0738: ('ldr', 'x2, [x8, #0x718]'),
    0xfffffff005cf074c: ('br', 'x2'),
    0xfffffff005cf15ec: ('stp', 'q0, q0, [x0]'),
    0xfffffff005cf15f0: ('stp', 'q0, q0, [x0, #0x20]'),
    0xfffffff00606c860: ('str', 'x0, [x19, #0x10]'),
    0xfffffff00606cd4c: ('str', 'w21, [x19, #0x28]'),
    0xfffffff00606cd50: ('str', 'xzr, [x19, #0x20]'),
    0xfffffff005cf0448: ('ldp', 'w8, w2, [x1]'),
    0xfffffff005cf044c: ('ldp', 'w3, w4, [x1, #8]'),
    0xfffffff005cf0450: ('ldr', 'w5, [x1, #0x10]'),
    0xfffffff005cf0520: ('ldr', 'x6, [x8, #0x5d8]'),
    0xfffffff005cf05b4: ('ldr', 'x8, [x8, #0x5e0]'),
    0xfffffff005cf06c8: ('ldrb', 'w8, [x19, #0x30]'),
    0xfffffff005cf06e0: ('ldp', 'x3, x0, [x19, #0x48]'),
    0xfffffff005cf0700: ('ldr', 'w3, [x19, #0x48]'),
    0xfffffff005cf0708: ('ldrb', 'w8, [x19, #0x31]'),
    0xfffffff005ce54c4: ('mov', 'w1, #0x70'),
    0xfffffff005ce5860: ('stp', 'w24, w22, [x19, #0x18]'),
    0xfffffff005ce5868: ('stp', 'w21, w9, [x19, #0x28]'),
    0xfffffff005ce586c: ('stp', 'w25, w8, [x19, #0x20]'),
    0xfffffff005ce6984: ('ldr', 'w0, [x0, #0x2c]'),
    0xfffffff005ce69d4: ('ldr', 'w0, [x0, #0x50]'),
    0xfffffff005cf1064: ('ldr', 'x8, [x8, #0x710]'),
    0xfffffff005cf1078: ('str', 'w25, [x0]'),
    0xfffffff005cf107c: ('str', 'x24, [x0, #8]'),
    0xfffffff005cf1084: ('ldr', 'x8, [x8, #0x20]'),
    0xfffffff005cf1090: ('strb', 'wzr, [x21, #0x30]'),
    0xfffffff005cf1094: ('str', 'x23, [x21, #0x40]'),
    0xfffffff005cf1098: ('str', 'w22, [x21, #0x2c]'),
    0xfffffff005cf10a4: ('str', 'x8, [x21, #0x60]'),
    0xfffffff005cf10a8: ('str', 'q0, [x21, #0x50]'),
    0xfffffff005cf10ac: ('strb', 'wzr, [x21, #0x31]'),
    0xfffffff005cf10b8: ('ldr', 'x1, [x8, #0x708]'),
    0xfffffff005cf10c4: ('mov', 'x2, x21'),
    0xfffffff005cf10d8: ('mov', 'x20, x0'),
    0xfffffff005cf10dc: ('cbz', 'w0, #0xfffffff005cf135c'),
    0xfffffff005cf10e0: ('str', 'w20, [x21, #0x28]'),
    0xfffffff005cf10f0: ('ldr', 'x8, [x8, #0x28]'),
    0xfffffff005cf10f8: ('str', 'xzr, [x21, #8]'),
    0xfffffff005cf1100: ('ldr', 'x8, [x8, #0x718]'),
    0xfffffff005cf110c: ('blr', 'x8'),
    0xfffffff005cf135c: ('mov', 'x0, x20'),
    0xfffffff005cf1360: ('ldp', 'x29, x30, [sp, #0x60]'),
    0xfffffff005cf1364: ('ldp', 'x20, x19, [sp, #0x50]'),
    0xfffffff005cf1368: ('ldp', 'x22, x21, [sp, #0x40]'),
    0xfffffff005cf136c: ('ldp', 'x24, x23, [sp, #0x30]'),
    0xfffffff005cf1370: ('ldp', 'x26, x25, [sp, #0x20]'),
    0xfffffff005cf1374: ('add', 'sp, sp, #0x70'),
    0xfffffff005cf1378: ('ret', ''),
    0xfffffff005cf13e4: ('ldr', 'x8, [x8, #0x710]'),
    0xfffffff005cf1414: ('strb', 'w8, [x22, #0x30]'),
    0xfffffff005cf141c: ('stp', 'x8, x23, [x22, #0x40]'),
    0xfffffff005cf1438: ('ldr', 'x1, [x8, #0x708]'),
    0xfffffff005cf15e4: ('bl', '#0xfffffff005cfbae8'),
    0xfffffff005cf15f4: ('stp', 'q0, q0, [x0, #0x40]'),
    0xfffffff005cf15f8: ('str', 'xzr, [x0, #0x60]'),
    0xfffffff005cf160c: ('b', '#0xfffffff005cfba88'),
    0xfffffff005cf0728: ('ldr', 'x8, [x8, #0x28]'),
    0xfffffff005cf0730: ('str', 'xzr, [x19, #8]'),
    0xfffffff005ce79f4: ('stp', 'w3, w2, [x1]'),
    0xfffffff005cea80c: ('ldr', 'x8, [x0, #0x20]'),
    0xfffffff005cea810: ('add', 'x8, x8, w3, uxtw'),
    0xfffffff005cea814: ('str', 'x8, [x0, #0x20]'),
    0xfffffff006063198: ('adrp', 'x8, #0xfffffff006e7b000'),
    0xfffffff00606319c: ('add', 'x8, x8, #0x188'),
    0xfffffff0060631a0: ('str', 'x8, [x0]'),
    0xfffffff00606c5ec: ('mov', 'x19, x1'),
    0xfffffff00606c5f4: ('ldr', 'x23, [x1, #8]'),
    0xfffffff00606cd54: ('cmp', 'w26, #1'),
    0xfffffff00606cd98: ('str', 'xzr, [x19, #0x48]'),
    0xfffffff006063c88: ('str', 'w8, [x9]'),
    0xfffffff006063d3c: ('dmb', 'ish'),
    0xfffffff006063f84: ('ldr', 'w13, [x11], #8'),
    0xfffffff006063f88: ('add', 'w10, w10, w13, uxth'),
    0xfffffff006063f98: ('ldr', 'w9, [x0, #0x24]'),
    0xfffffff006063fa0: ('csel', 'w10, w10, w9, ls'),
    0xfffffff006063fbc: ('sub', 'w9, w9, w10'),
    0xfffffff006063fc0: ('str', 'w9, [x1]'),
    0xfffffff0060711a8: ('bl', '#0xfffffff006063bc8'),
    0xfffffff0060711ac: ('str', 'w0, [x21, #0x24]'),
    0xfffffff006070b08: ('bl', '#0xfffffff006063f5c'),
    0xfffffff006070b18: ('add', 'x8, x10, x8'),
    0xfffffff006070b1c: ('str', 'x8, [x9, #0x48]'),
    0xfffffff006072158: ('bl', '#0xfffffff006063f5c'),
    0xfffffff006072168: ('add', 'x8, x10, x8'),
    0xfffffff00607216c: ('str', 'x8, [x9, #0x48]'),
    0xfffffff006072e08: ('bl', '#0xfffffff006063f5c'),
    0xfffffff006072e1c: ('str', 'x8, [x9, #0x48]'),
    0xfffffff006073278: ('bl', '#0xfffffff006063f5c'),
    0xfffffff00607328c: ('str', 'x8, [x9, #0x48]'),
    0xfffffff006071800: ('b', '#0xfffffff0060742d0'),
    0xfffffff0060742d4: ('ldr', 'x16, [x16, #0x750]'),
    0xfffffff00606e474: ('str', 'w20, [x21, #0x28]'),
    0xfffffff00606e47c: ('ldr', 'x8, [x8, #0x7e0]'),
    0xfffffff00606c730: ('ldr', 'w26, [x22, #0x10]'),
    0xfffffff006070e5c: ('str', 'wzr, [x21, #0x28]'),
    0xfffffff006070f98: ('ldr', 'x9, [x8, #0x48]!'),
    0xfffffff006070fa0: ('str', 'x21, [x9, #0x20]'),
    0xfffffff006070fa8: ('str', 'x21, [x20, #0x48]'),
    0xfffffff006070fb0: ('str', 'x21, [x8]'),
    0xfffffff006067994: ('ldr', 'x22, [x21, #0x40]'),
    0xfffffff006067998: ('stp', 'xzr, xzr, [x21, #0x40]'),
    0xfffffff0060679bc: ('ldr', 'x8, [x8, #0x7e0]'),
    0xfffffff0060679c4: ('mov', 'x1, x22'),
    0xfffffff00607030c: ('ldr', 'x20, [x19, #0x1b8]'),
    0xfffffff006070310: ('ldr', 'x0, [x19, #0x288]'),
    0xfffffff006070318: ('ldr', 'x8, [x8, #0x148]'),
    0xfffffff006070320: ('mov', 'x1, x0'),
    0xfffffff006070324: ('mov', 'x0, x20'),
    0xfffffff006070328: ('bl', '#0xfffffff006074330'),
    0xfffffff005ce8c70: ('ldr', 'x8, [x20]'),
    0xfffffff005ce8c78: ('str', 'x8, [x20, #0x10]!'),
    0xfffffff005ce8dac: ('mov', 'w8, #2'),
    0xfffffff005ce8db0: ('str', 'w8, [x19, #0xc]'),
    0xfffffff005ce8e90: ('ldr', 'x0, [x19, #0x68]'),
    0xfffffff005ce8e9c: ('ldr', 'x3, [x8, #0x6a8]'),
    0xfffffff00606ffe0: ('ldr', 'x0, [x19, #0x1b8]'),
    0xfffffff00606ffe4: ('bl', '#0xfffffff00607433c'),
    0xfffffff005ce8fcc: ('ldr', 'w8, [x0, #0xc]'),
    0xfffffff005ce8fd0: ('cmp', 'w8, #2'),
    0xfffffff005ce8fd8: ('ldrsb', 'w8, [x19, #0x10]'),
    0xfffffff005ce8fe0: ('ldrh', 'w8, [x19, #0x16]'),
    0xfffffff005ce9058: ('mov', 'w8, #3'),
    0xfffffff005ce905c: ('str', 'w8, [x19, #0xc]'),
    0xfffffff005ce91bc: ('ldr', 'x0, [x19, #0x68]'),
    0xfffffff005ce91c8: ('ldr', 'x8, [x8, #0x6a8]'),
    0xfffffff005ce91cc: ('mov', 'w1, #2'),
    0xfffffff005ce91d8: ('bl', '#0xfffffff005cf9080'),
    0xfffffff005cf9080: ('ldr', 'x8, [x0, #0x1b8]'),
    0xfffffff005cf90f0: ('ldr', 'x1, [x8, #0x6e0]'),
    0xfffffff005cf0ff8: ('mov', 'w1, #0x80'),
    0xfffffff005cf0ffc: ('mov', 'w2, #0'),
    0xfffffff005cf1000: ('mov', 'w3, #0'),
    0xfffffff005cf1004: ('b', '#0xfffffff005ce7f90'),
    0xfffffff005ce7fc0: ('mov', 'w1, #0xa0'),
    0xfffffff005ce7ffc: ('mov', 'x1, x23'),
    0xfffffff005ce8000: ('mov', 'x2, x22'),
    0xfffffff005ce8004: ('mov', 'x3, x21'),
    0xfffffff005ce8008: ('mov', 'x4, x20'),
    0xfffffff005ce8054: ('str', 'x1, [x0, #0x68]'),
    0xfffffff005ce8058: ('str', 'w4, [x0, #0x88]'),
    0xfffffff005ce805c: ('stp', 'w2, w3, [x0, #0x60]'),
    0xfffffff005ce8068: ('mov', 'w0, #0xffff'),
    0xfffffff005ce8078: ('str', 'x0, [x19, #0x70]'),
    0xfffffff005cea508: ('ldr', 'w8, [x0, #0xc]'),
    0xfffffff005cea50c: ('cmp', 'w8, #3'),
    0xfffffff005cea538: ('ldr', 'x23, [x19, #0x70]'),
    0xfffffff005cea53c: ('str', 'x23, [x19, #0x40]'),
    0xfffffff005cea544: ('ldr', 'x8, [x8, #0x20]'),
    0xfffffff005cea558: ('ldr', 'x8, [x8, #0xd8]'),
    0xfffffff005cea55c: ('mov', 'w1, #0'),
    0xfffffff005cea56c: ('stp', 'x20, xzr, [x23, #-0x10]'),
    0xfffffff005cea5c8: ('add', 'x8, x8, #0x7ec'),
    0xfffffff005cea5cc: ('stp', 'x19, x8, [sp, #0x48]'),
    0xfffffff005cea5d0: ('str', 'x20, [sp, #0x58]'),
    0xfffffff005cea5d4: ('ldr', 'x0, [x19, #0x68]'),
    0xfffffff005cea5d8: ('ldr', 'w1, [x19, #0x60]'),
    0xfffffff005cea5dc: ('ldr', 'x2, [x19, #0x40]'),
    0xfffffff005cea5e4: ('ldr', 'x8, [x8, #0x6f8]'),
    0xfffffff005cea5ec: ('mov', 'x3, x20'),
    0xfffffff005cea5f4: ('mov', 'w6, #0'),
    0xfffffff005cea6bc: ('cbz', 'x20, #0xfffffff005cea7d0'),
    0xfffffff005cea6c4: ('tbnz', 'w8, #0, #0xfffffff005cea7d0'),
    0xfffffff005cea6c8: ('ldrh', 'w8, [x19, #0x16]'),
    0xfffffff005cea6d0: ('b.ls', '#0xfffffff005cea7cc'),
    0xfffffff005cea6e0: ('ldr', 'x8, [x8, #0x5c0]'),
    0xfffffff005cea6e8: ('cmp', 'w0, #2'),
    0xfffffff005cea6ec: ('mov', 'w8, #0x40'),
    0xfffffff005cea6f0: ('mov', 'w9, #0x200'),
    0xfffffff005cea6f4: ('csel', 'x8, x9, x8, gt'),
    0xfffffff005cea77c: ('udiv', 'x9, x20, x8'),
    0xfffffff005cea780: ('msub', 'x8, x9, x8, x20'),
    0xfffffff005cea784: ('cbnz', 'x8, #0xfffffff005cea7cc'),
    0xfffffff005cea78c: ('ldr', 'x8, [x8, #0x88]'),
    0xfffffff005cea790: ('stp', 'x19, x8, [sp, #0x28]'),
    0xfffffff005cea79c: ('ldr', 'w1, [x19, #0x60]'),
    0xfffffff005cea7a0: ('ldp', 'x0, x2, [x19, #0x68]'),
    0xfffffff005cea7a8: ('ldr', 'x8, [x8, #0x6f8]'),
    0xfffffff005cea7b0: ('mov', 'x3, #0'),
    0xfffffff005cea8fc: ('ldp', 'x21, x8, [x19, #0x18]'),
    0xfffffff005cea900: ('cmp', 'x21, x8'),
    0xfffffff005ceaa58: ('cbz', 'x8, #0xfffffff005ceab5c'),
    0xfffffff005ceaa60: ('mov', 'w8, #8'),
    0xfffffff005ceaa64: ('str', 'w8, [x19, #0xc]'),
    0xfffffff005ceab50: ('ldr', 'x8, [x8, #0x6a8]'),
    0xfffffff005ceab68: ('ldr', 'x8, [x8, #0xe0]'),
    0xfffffff005ceab6c: ('mov', 'w1, #0'),
    0xfffffff005ceab80: ('ldp', 'x3, x0, [x19, #0x20]'),
    0xfffffff005ceab84: ('ldp', 'x1, x4, [x19, #0x38]'),
    0xfffffff005ceab8c: ('blr', 'x8'),
    0xfffffff005ceabac: ('ldr', 'x0, [x19, #0x40]'),
    0xfffffff005ceabc0: ('str', 'xzr, [x19, #0x40]'),
    0xfffffff005ceae28: ('mov', 'x19, x0'),
    0xfffffff005ceaefc: ('mov', 'w8, #8'),
    0xfffffff005ceaf00: ('str', 'w8, [x19, #0xc]'),
    0xfffffff005ceafec: ('ldr', 'x3, [x8, #0x6a8]'),
    0xfffffff005ce9f08: ('cmp', 'w8, #7'),
    0xfffffff005ce9f28: ('add', 'x8, x8, #0xbdc'),
    0xfffffff005ce9f34: ('ldr', 'w1, [x19, #0x60]'),
    0xfffffff005ce9f40: ('ldr', 'x8, [x8, #0x6f8]'),
    0xfffffff005ce9f48: ('mov', 'x3, #0'),
    0xfffffff005ceabfc: ('cmp', 'w2, w8'),
    0xfffffff005ceac24: ('mov', 'w8, #1'),
    0xfffffff005ceac28: ('str', 'w8, [x19, #0xc]'),
    0xfffffff005cead6c: ('ldr', 'x3, [x8, #0x6a8]'),
    0xfffffff005ce9ce8: ('ldr', 'x8, [x19, #0x50]'),
    0xfffffff005ce9cf0: ('ldr', 'x0, [x19, #0x48]'),
    0xfffffff005ce9cf4: ('ldr', 'x1, [x19, #0x58]'),
    0xfffffff005ce9cfc: ('mov', 'w3, #0'),
    0xfffffff005ce9d00: ('blr', 'x8'),
}

OTG_VPTR = 0xfffffff006e7b188
REQUEST_TYPE = 0xfffffff006e12b28
REQUEST_FREE_TYPE = 0xfffffff006e12b68
OTG_REQUEST_SLOTS = {
    223: 0xfffffff005cf1008, 224: 0xfffffff005cf137c,
    225: 0xfffffff00606c5cc, 226: 0xfffffff005cf15d4,
    227: 0xfffffff005cf1604, 252: 0xfffffff00607177c,
}


def field(offset, width, meaning, confidence='observed access; semantic label provisional'):
    return dict(offset=offset, width=width, meaning=meaning, confidence=confidence)


def recover(data):
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
    cache = MachO(data)
    if cache.kind != 2:
        raise ValueError('Requires the stock MH_EXECUTE, not a built candidate')
    cs = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    decoded, regions = {}, {}
    for name, (start, end) in REGIONS.items():
        rows = [dict(pc=i.address, mnemonic=i.mnemonic, operands=i.op_str)
                for i in cs.disasm(cache.read(start, end - start), start)]
        if len(rows) * 4 != end - start:
            raise ValueError(f'Incomplete arm64 decode: {name}')
        regions[name] = rows
        decoded.update({r['pc']: (r['mnemonic'], r['operands']) for r in rows})
    for pc, expected in OBSERVED.items():
        if decoded.get(pc) != expected:
            raise ValueError(f'Unqualified observation at {pc:#x}: {decoded.get(pc)}')
    exports = cache.exports()
    slots = []
    for slot in range(168, CONTROLLER_SLOTS):
        target, = struct.unpack('<Q', cache.read(CONTROLLER_VPTR + slot * 8, 8))
        slots.append(dict(slot=slot, target=target,
                          pure_virtual='___cxa_pure_virtual' in exports.get(target, [])))
    # This subclass supplies one concrete request path in the same stock cache;
    # its descriptor engine is evidence for semantics, NOT an MTU3 backend.
    for slot, expected in OTG_REQUEST_SLOTS.items():
        target, = struct.unpack('<Q', cache.read(OTG_VPTR + slot * 8, 8))
        if target != expected:
            raise ValueError(f'Unqualified OTG request slot {slot}: {target:#x}')
    type_name, = struct.unpack('<Q', cache.read(REQUEST_TYPE + 16, 8))
    if cache.read(type_name, len(b'site.IOUSBDeviceIORequest\0')) != b'site.IOUSBDeviceIORequest\0':
        raise ValueError('Unqualified request allocation type')
    type_sizes = [struct.unpack('<I', cache.read(a + 44, 4))[0]
                  for a in (REQUEST_TYPE, REQUEST_FREE_TYPE)]
    if type_sizes != [0x68, 0x68]:
        raise ValueError(f'Unqualified request type sizes: {type_sizes}')
    completion_import, = struct.unpack('<Q', cache.read(0xfffffff006e7a750, 8))
    if completion_import != REGIONS['complete_request'][0]:
        raise ValueError('OTG completion import does not target the family completion')
    control_imports = {}
    for name, cell, expected in (
        ('input_setup', 0xfffffff006e7a790, REGIONS['control_input_setup'][0]),
        ('process_setup', 0xfffffff006e7a798, REGIONS['control_process_setup'][0]),
    ):
        target, = struct.unpack('<Q', cache.read(cell, 8))
        if target != expected:
            raise ValueError(f'Unqualified control import {name}: {target:#x}')
        control_imports[name] = target
    control_vptr = 0xfffffff006e10098
    control_slots = {15: 0xfffffff005ce8040, 16: 0xfffffff005cea4e4,
                     17: 0xfffffff005ceae14}
    for slot, expected in control_slots.items():
        target, = struct.unpack('<Q', cache.read(control_vptr + slot * 8, 8))
        if target != expected:
            raise ValueError(f'Unqualified control slot {slot}: {target:#x}')
    result = dict(
        build='20H392', identity='d22ap', kernelcache_sha256=SHA256,
        qualification='bounded static observations; NOT executable ABI',
        control_setup=dict(
            type='IOUSBDeviceControlRequest', not_ordinary_bulk_request=True,
            input_entry=REGIONS['control_input_setup'][0], imports=control_imports,
            setup_offset=0x10, setup_bytes=8, state_offset=0x0c,
            controller_offset=0x68, controller_control_object_offset=0x1b8,
            ready_state=2, control_in_state=3, control_out_state=4, no_data_out_state=7,
            state_notification_slot=213, setup_dispatch=REGIONS['controller_setup_dispatch'][0],
            class_vendor_slot=220,
            warning='state observation is NOT native bootstrap/current-layout qualification; MTU3 PIO is separate, no hardcoded Apple callable address'),
        control_requests=dict(
            original_control_size=0xa0, vptr=control_vptr, qualified_slots=control_slots,
            in_endpoint=0x80, out_endpoint=0, timeout_word=0,
            data_memory_offset=0x40, fallback_memory_offset=0x70, fallback_capacity=0xffff,
            original_control_descriptor_ownership='retain and prepare(direction0) before slot223; complete(direction0) before user data callback, then release; adapter nested pair must be independent',
            data_callback=0xfffffff005cea7ec, data_callback_actual_width=4,
            progress_offset=0x20, requested_offset=0x18,
            zlp_callback=0xfffffff005ceae14, zlp_ready_state=8,
            zlp_callback_ignores_status=True,
            explicit_zlp='queued after data admission, before data callback, if nonzero short response is exact packet multiple and flag88 bit0 clear',
            packet_size_from_controller_slot=184, packet_size='Family speed <=2:64; >2:512; not MTU3 raw speed enum',
            data_ready_state=8, data_final_callback_may_precede_status=True,
            in_status_out_final_callback_offsets=[0x48, 0x50, 0x58],
            out_status_in_callback=0xfffffff005ceabdc, out_status_returns_state=1,
            warning='EP0 plain requests need distinct PIO stage callbacks, not bulk owner. Family speed enum/flags, control object glue and memory vptr/layout are still unqualified'),
        original_pipe_size=0x70,
        settings_input=dict(bytes_read=20, words=5, word_width=4,
                            gate=0xfffffff005cf0438,
                            warning='NOT a USB endpoint wire descriptor or a pointer array'),
        endpoint_dispatch=dict(wrapper_slot=173, hardware_slot=187,
                               argument_count_after_this=5,
                               field_offsets_in_pipe=[0x2c, 0x20, 0x18, 0x24, 0x50],
                               warning='max-packet accessor also uses negotiated speed; not a direct load'),
        destroy_dispatch=dict(wrapper_slot=174, hardware_slot=188,
                              field_offsets_in_pipe=[0x2c]),
        pipe_fields=[field(0x10, 8, 'controller pointer'),
                     field(0x18, 4, 'requested max packet'), field(0x1c, 4, 'direction'),
                     field(0x20, 4, 'transfer type'), field(0x24, 4, 'interval'),
                     field(0x28, 4, 'fifth settings input'), field(0x2c, 4, 'assigned endpoint'),
                     field(0x30, 4, 'speed used by effective max packet accessor'),
                     field(0x50, 4, 'opaque fifth hardware-create argument')],
        request_allocation=dict(type='IOUSBDeviceIORequest', bytes=0x68,
                                allocate_slot=226, free_slot=227,
                                initialized_bytes=0x68, type_sizes=type_sizes,
                                warning='plain typed allocation, not an OSObject or a proven request pool'),
        request_dispatch=dict(normal_slot=223, isochronous_slot=224,
                              gated_hardware_slot=225, hardware_this_and_request_only=True,
                              normal_register_inputs={'x1': 'endpoint address incl. direction bit7',
                                                      'x2': 'memory descriptor', 'x3': 'requested bytes',
                                                      'x4': '24-byte completion record',
                                                      'x5': 'not qualified; not persisted by this wrapper',
                                                      'w6': 'timeout-related word stored at request+0x2c'},
                              warning='observed call sites, NOT a callable C++ ABI'),
        completion_fields=[field(0, 4, 'endpoint address incl. direction bit7'),
                           field(8, 8, 'retained memory descriptor, released after callback'),
                           field(0x28, 4, 'status'), field(0x30, 1, 'wide union payload callback selector'),
                           field(0x2c, 4, 'timeout-related word; expiry/cancel synchronization unqualified'),
                           field(0x31, 1, 'skip post-callback buffer release/free selector'),
                           field(0x40, 8, 'ordinary: requested bytes; isochronous: frame count'),
                           field(0x48, '4 or 8', 'union: ordinary accumulated actual bytes OR isochronous frame-array pointer'),
                           field(0x50, 8, 'callback target'), field(0x58, 8, 'callback action'),
                           field(0x60, 8, 'callback parameter')],
        callback_arguments=['target pointer', 'parameter pointer', '32-bit status',
                            '32-bit actual bytes if flag30=0; 64-bit union payload (frame-array pointer on slot224) otherwise'],
        normal_count=dict(meaning='actual transferred bytes, NOT bytes remaining',
                          confidence='stock static producer -> accumulator -> callback -> consumer chain',
                          initialization_pc=0xfffffff00606cd98,
                          delta=dict(entry=0xfffffff006063f5c,
                                     expression='programmed chunk bytes - min(sum(descriptor low16 residual), programmed chunk bytes)',
                                     incremental=True),
                          accumulator_pcs=[0xfffffff006070b1c, 0xfffffff00607216c,
                                           0xfffffff006072e1c, 0xfffffff00607328c],
                          consumer=dict(entry=0xfffffff005cea7ec,
                                        expression='control data-stage progress += callback count32'),
                          warning='error/abort callbacks can carry partial actual; this does NOT qualify X9 QMU partial accounting'),
        isochronous_payload=dict(slot=224, selector30=1, request48='caller frame-array pointer',
                                 request40='32-bit frame count zero-extended to 64-bit',
                                 warning='NOT a 64-bit byte count; do not map tracked TX actual here'),
        request_lifetime=dict(
            submit=['allocate via slot226', 'retain descriptor via original OSObject slot4',
                    'copy completion target/action/parameter', 'gate slot225 with request'],
            accepted='wrapper returns status0 without releasing; backend owns completion obligation',
            accepted_return=dict(branch_pc=0xfffffff005cf10dc,
                                 epilogue=0xfffffff005cf135c,
                                 request_access_after_gate_success=False,
                                 evidence='success branch goes directly to register/stack epilogue; no request dereference',
                                 warning='does not qualify all outer callers, callback receivers or native gate ABI'),
            rejected='nonzero gated status: wrapper releases descriptor, clears request+8, frees via slot227; no wrapper callback',
            completion=['optional action(target, parameter, status, typed payload)',
                        'if flag31=0: release descriptor, clear request+8, free via slot227'],
            concrete_completion_queue=dict(next_offset=0x20,
                                           endpoint_head_offset=0x40, endpoint_tail_offset=0x48,
                                           out_retire=0xfffffff006070e5c,
                                           drain=0xfffffff006067974, complete_slot=252,
                                           warning='endpoint+48 queue tail is NOT request+48 actual/frames union'),
            warning='callback precedes release/free; flag31!=0 skips BOTH. No safe timeout/free policy or reentrant adapter established'),
        ordinary_adapter=dict(
            scope='kernel source adapter only; binding/glue/loader still required',
            allocation_size=0x68,
            accepts_endpoints=[0x01, 0x81, 0x03, 0x83],
            requires_current_layout_memory=True,
            memory_identity_offset=8,
            rejects_flags=[0x30, 0x31],
            requires_initial_zero_fields=[0x10, 0x20, 0x28, 0x48],
            final_writes=[dict(offset=0x28, width=4, meaning='completion status'),
                          dict(offset=0x48, width=8, meaning='accumulated actual bytes')],
            native_completion_entry=REGIONS['complete_request'][0],
            callback_record_offsets=[0x50, 0x58, 0x60],
            post_native_completion_request_access=False,
            warning='Never cast opaque memory to current IOMemoryDescriptor without ABI qualification; no hardcoded callable address or original abort mapping'),
        concrete_producer=dict(bundle='com.apple.driver.AppleSynopsysOTGDevice',
                               vptr=OTG_VPTR, request_slots=OTG_REQUEST_SLOTS,
                               completion_import=completion_import,
                               warning='stock descriptor-count evidence only; DWC/OTG registers must NOT be copied to MTU3'),
        original_controller_slots=slots,
        checked_instructions=[dict(pc=pc, mnemonic=v[0], operands=v[1])
                              for pc, v in OBSERVED.items()],
        abi_compatible=False, runtime_loadable=False,
        unresolved=['168 versus 272 IOService slots', 'timeout/cancel/retention/reentrancy contract',
                    'all hardware pure-virtual signatures', 'native interface and mux integration',
                    'physical MTU3 DMA and authoritative partial-byte accounting'])
    return result, regions


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernelcache', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    output = a.out.resolve()
    if output.exists() or not any(output != p.resolve() and output.is_relative_to(p.resolve())
                                  for p in (ROOT / '.local', ROOT / 'x9/out')):
        ap.error('Use a fresh private output directory under .local or x9/out')
    data = a.kernelcache.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        ap.error('Wrong source: requires the reviewed original D22/20H392 cache')
    result, regions = recover(data)
    output.mkdir(parents=True)
    (output / 'contract.json').write_text(json.dumps(result, indent=2) + '\n')
    for name, rows in regions.items():
        (output / (name + '.dis')).write_text(''.join(
            f"{r['pc']:016x}: {r['mnemonic']:8} {r['operands']}\n" for r in rows))
    print(f'20H392: {len(OBSERVED)} checked observations; settings=5x32, create=slot173->187')
    print(f'Request: 0x68B; {len(REGIONS)} bounded regions; ordinary callback count=actual bytes')
    print('Slot224 wide callback payload is a frame-array pointer, NOT a 64-bit byte count')
    print('Control SETUP: distinct IOUSBDeviceControlRequest; setup8@10, ready=2, controller notification=slot213')
    print('Native ABI/runtime loading and timeout/cancel contracts remain unqualified')


if __name__ == '__main__':
    main()
