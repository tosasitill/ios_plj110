#!/usr/bin/env python3
"""Symbolically execute mediatek-drm.ko's mout callback under unicorn.

The .ko is relocatable; we map .text/.rodata/.data at fixed VMAs, patch the
ADRP/ADD/CALL/JUMP relocations in-place, then call
mtk_ddp_exdma_mout_MT6993(cur, next, out) for every (cur, next) enum pair and
collect {cur, next, addr, val, mask} routing rows.

Output: /tmp/mout_table.json
"""
import json
import struct
import sys

import unicorn
from unicorn import arm64_const as A64

import sys as _s
KO = _s.argv[1] if len(_s.argv) > 1 else '/tmp/vbmods/mediatek-drm.ko'
TEXT_VMA = 0x100000
RODATA_VMA = 0x800000
DATA_VMA = 0xA00000
STACK = 0x7F000000
OUTBUF = 0x600000

f = open(KO, 'rb').read()

e_shoff, = struct.unpack_from('<Q', f, 0x28)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', f, 0x3a)
shstr_off = struct.unpack_from('<Q', f, e_shoff + e_shstrndx * e_shentsize + 24)[0]
def sname(o):
    return f[shstr_off + o: f.index(b'\x00', shstr_off + o)].decode()

secs = {}
symtab_off = symtab_size = strt_off = 0
rela_text = None
for i in range(e_shnum):
    s = e_shoff + i * e_shentsize
    no, typ, flags, addr, off, size = struct.unpack_from('<IIQQQQ', f, s)
    n = sname(no)
    secs[n] = (off, size)
    if n == '.symtab':
        symtab_off, symtab_size = off, size
    if n == '.strtab':
        strt_off = off
    if n == '.rela.text':
        rela_text = (off, size)

text_off, text_size = secs['.text']
ro_off, ro_size = secs['.rodata']
data_off, data_size = secs['.data']

# section index -> base VMA
shndx_base = {}
for i in range(e_shnum):
    s = e_shoff + i * e_shentsize
    no, typ, flags, addr, off, size = struct.unpack_from('<IIQQQQ', f, s)
    n = sname(no)
    shndx_base[i] = (TEXT_VMA if n == '.text' else
                     RODATA_VMA if n == '.rodata' else
                     DATA_VMA if n == '.data' else 0)

def sym_shndx(idx):
    so = symtab_off + idx * 24
    info_b, othr, shndx, val = struct.unpack_from('<BBHQ', f, so + 4)
    typ = info_b & 0xf
    return shndx

def symname(idx):
    so = symtab_off + idx * 24
    nmoff, = struct.unpack_from('<I', f, so)
    return f[strt_off + nmoff: f.index(b'\x00', strt_off + nmoff)].decode()

# find function symbols we want
fn_addr = {}
for i in range(symtab_size // 24):
    so = symtab_off + i * 24
    nmoff, = struct.unpack_from('<I', f, so)
    n = f[strt_off + nmoff: f.index(b'\x00', strt_off + nmoff)].decode()
    if n in ('mtk_ddp_exdma_mout_MT6993',):
        _, _, shndx, val = struct.unpack_from('<BBHQ', f, so + 4)
        fn_addr[n] = val

# ---- build patched .text image ----
text = bytearray(f[text_off:text_off + text_size])

RELOC_ADRP = 0x113
RELOC_ADD = 0x115
RELOC_CALL = 0x11b
RELOC_JUMP = 0x11d

n_patch = 0

def sym_target(sym, addend):
    """Resolve (base, value) for a relocation symbol."""
    if sym == 0:
        return None
    so = symtab_off + sym * 24
    if so + 24 > symtab_off + symtab_size:
        return None
    info_b, othr, shndx, val = struct.unpack_from('<BBHQ', f, so + 4)
    typ = info_b & 0xf
    if typ == 3:  # STT_SECTION
        base = shndx_base.get(shndx, 0)
        return (base, addend) if base else None
    base = shndx_base.get(shndx, 0)
    return (base, val + addend) if base else None

rel_off, rel_size = rela_text
for i in range(rel_size // 24):
    off, info, add = struct.unpack_from('<QQq', f, rel_off + i * 24)
    rtype = info & 0xffffffff
    sym = info >> 32
    add &= 0xffffffffffff
    if off >= text_size:
        continue
    if rtype in (RELOC_ADRP, RELOC_ADD):
        t = sym_target(sym, add)
        if not t:
            continue
        target = t[0] + t[1]
        if rtype == RELOC_ADRP:
            pc = TEXT_VMA + off
            diff = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
            diff &= 0x1FFFFF
            w = struct.unpack_from('<I', text, off)[0]
            w = (w & 0x9F00001F) | ((diff & 3) << 29) | ((diff >> 2) << 5)
            struct.pack_into('<I', text, off, w)
        else:
            w = struct.unpack_from('<I', text, off)[0]
            w = (w & 0xFFC003FF) | ((target & 0xFFF) << 10)
            struct.pack_into('<I', text, off, w)
        n_patch += 1
    elif rtype in (RELOC_CALL, RELOC_JUMP):
        t = sym_target(sym, add)
        if not t or t[0] != TEXT_VMA:
            continue
        target = t[0] + t[1]
        pc = TEXT_VMA + off
        delta = (target - pc) >> 2
        if not (-(1 << 25) <= delta < (1 << 25)):
            continue
        w = struct.unpack_from('<I', text, off)[0]
        w = (w & 0xFC000000) | (delta & 0x3FFFFFF)
        struct.pack_into('<I', text, off, w)
        n_patch += 1

sys.stderr.write('patched %d relocs\n' % n_patch)

uc = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
uc.mem_map(TEXT_VMA & ~0xFFFF, (text_size + 0x20000) & ~0xFFFF)
uc.mem_write(TEXT_VMA, bytes(text))
uc.mem_map(RODATA_VMA & ~0xFFFF, (ro_size + 0x20000) & ~0xFFFF)
uc.mem_write(RODATA_VMA, f[ro_off:ro_off + ro_size])
uc.mem_map(DATA_VMA & ~0xFFFF, 0x20000)
uc.mem_write(DATA_VMA, f[data_off:data_off + min(data_size, 0x20000)])
uc.mem_map(STACK, 0x100000)
uc.mem_map(OUTBUF, 0x1000)

MAIN = TEXT_VMA + fn_addr['mtk_ddp_exdma_mout_MT6993']
END = MAIN + 0x600  # generous; stopped via timeout/ret hook

from pathlib import Path as _P
ENUM = json.loads((_P(__file__).parent / 'ddp_components.json').read_text())
ids = sorted(int(k) for k in ENUM)

rows = []
for cur in ids:
    for nxt in ids:
        uc.mem_write(OUTBUF, b'\x00' * 0x20)
        uc.reg_write(A64.UC_ARM64_REG_X0, cur)
        uc.reg_write(A64.UC_ARM64_REG_X1, nxt)
        uc.reg_write(A64.UC_ARM64_REG_X2, OUTBUF)
        uc.reg_write(A64.UC_ARM64_REG_SP, STACK + 0x80000)
        uc.reg_write(A64.UC_ARM64_REG_LR, TEXT_VMA + text_size - 8)
        try:
            uc.emu_start(MAIN, TEXT_VMA + text_size - 8, timeout=2000, count=20000)
        except unicorn.UcError:
            continue
        out = struct.unpack('<8I', uc.mem_read(OUTBUF, 32))
        addr = out[0]
        val = uc.reg_read(A64.UC_ARM64_REG_X0) & 0xffffffff
        mask = val
        if addr or val:
            rows.append({'cur': cur, 'cur_name': ENUM.get(str(cur), '?'),
                         'next': nxt, 'next_name': ENUM.get(str(nxt), '?'),
                         'addr': addr, 'val': val, 'mask': mask})

json.dump(rows, open(_s.argv[2] if len(_s.argv) > 2 else '/tmp/mout_table.json', 'w'), indent=0)
sys.stderr.write('%d routing rows -> /tmp/mout_table.json\n' % len(rows))
