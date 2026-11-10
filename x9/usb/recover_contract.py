#!/usr/bin/env python3
"""Recover bounded endpoint/completion observations from stock D22/20H392.

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
}
# Assertions are source-qualification checks, not claims of runtime success.
OBSERVED = {
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
    result = dict(
        build='20H392', identity='d22ap', kernelcache_sha256=SHA256,
        qualification='bounded static observations; NOT executable ABI',
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
        completion_fields=[field(8, 8, 'retained buffer object, released after callback'),
                           field(0x28, 4, 'status'), field(0x30, 1, 'wide count callback selector'),
                           field(0x31, 1, 'skip post-callback buffer release/recycle selector'),
                           field(0x48, '4 or 8', 'count; actual versus remaining NOT established'),
                           field(0x50, 8, 'callback target'), field(0x58, 8, 'callback action'),
                           field(0x60, 8, 'callback parameter')],
        callback_arguments=['target pointer', 'parameter pointer', '32-bit status',
                            '32-bit count if flag30=0; 64-bit count otherwise'],
        original_controller_slots=slots,
        checked_instructions=[dict(pc=pc, mnemonic=v[0], operands=v[1])
                              for pc, v in OBSERVED.items()],
        abi_compatible=False, runtime_loadable=False,
        unresolved=['168 versus 272 IOService slots', 'request allocation/lifetime/timeout contract',
                    'all hardware pure-virtual signatures', 'native interface and mux integration'])
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
    print('Completion count width: 32/64; count meaning and native ABI remain unqualified')


if __name__ == '__main__':
    main()
