#!/usr/bin/env python3
"""Check bounded stock IOSurface notification instructions in pinned 20H392 image.

Static inspection only; requires capstone and the existing private kernelcache.
"""
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'x9/usb'))
from prepare_native import MachO
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

cache = MachO((ROOT / '.local/x9/usb/native-20H392/kernelcache.macho').read_bytes())
cs = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
exports = cache.exports()


def check(address, expected):
    ins = list(cs.disasm_lite(cache.read(address, 4), address))
    actual = f'{ins[0][2]} {ins[0][3]}'
    if actual != expected:
        raise AssertionError(f'{address:#x}: expected {expected!r}; got {actual!r}')
    print(f'{address:#x}: {actual}')


def stub(address, symbol):
    ins = list(cs.disasm_lite(cache.read(address, 12), address))
    assert [i[2] for i in ins] == ['adrp', 'ldr', 'br']
    page = int(ins[0][3].rsplit('#', 1)[1], 0)
    match = re.search(r'#(0x[0-9a-f]+|\d+)', ins[1][3])
    offset = int(match[1], 0) if match else 0
    target = struct.unpack('<Q', cache.read(page + offset, 8))[0]
    assert any(symbol in name for name in exports.get(target, [])), (hex(address), hex(target), symbol)
    print(f'{address:#x}: {symbol} -> {target:#x}')


# Input 39 -> handler values -> notifier fields -> async completion buffer.
for pc, instruction in [
    (0xfffffff005a5d8f4, 'ldr w1, [x8]'),
    (0xfffffff005a5d8f8, 'ldp x2, x3, [x8, #8]'),
    (0xfffffff005a5d8fc, 'ldp x4, x5, [x8, #0x18]'),
    (0xfffffff005a601dc, 'mov x21, x5'),
    (0xfffffff005a601e0, 'mov x20, x4'),
    (0xfffffff005a601e4, 'mov x22, x3'),
    (0xfffffff005a602c0, 'mov x2, x21'),
    (0xfffffff005a602c4, 'mov x3, x22'),
    (0xfffffff005a602c8, 'mov x4, x20'),
    (0xfffffff005a45a90, 'stp x20, x21, [x22, #0x10]'),
    (0xfffffff005a45a98, 'str x19, [x22, #0x20]'),
    (0xfffffff005a444f8, 'ldp x9, x10, [x0, #0x10]'),
    (0xfffffff005a444fc, 'ldr x11, [x0, #0x20]'),
    (0xfffffff005a44500, 'stp x11, x10, [sp, #8]'),
    (0xfffffff005a44504, 'lsr x10, x9, #0x20'),
    (0xfffffff005a44508, 'stp x9, x10, [sp, #0x18]'),
    (0xfffffff005a44514, 'mov w1, #0'),
    (0xfffffff005a44518, 'mov w3, #4'),
    (0xfffffff005a4451c, 'bl #0xfffffff005a67420'),
    (0xfffffff005a44830, 'bl #0xfffffff005a6742c'),
    (0xfffffff005a44864, 'bl #0xfffffff005a67444'),
    (0xfffffff005a442a0, 'bl #0xfffffff005a6787c'),
    (0xfffffff005a444c0, 'bl #0xfffffff005a67888'),
    (0xfffffff005a44360, 'bl #0xfffffff005a444d4'),
]:
    check(pc, instruction)
for pc, symbol in [
    (0xfffffff005a67420, 'sendAsyncResult64'),
    (0xfffffff005a6742c, 'setAsyncReference64'),
    (0xfffffff005a67444, 'releaseAsyncReference64'),
    (0xfffffff005a6787c, 'thread_call_allocate'),
    (0xfffffff005a67888, 'thread_call_enter'),
    (0xfffffff005a67894, 'thread_call_free'),
]:
    stub(pc, symbol)
print('PASS: static delivery instruction and symbol checks; no runtime test')
