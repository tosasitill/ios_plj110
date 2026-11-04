"""shim/fdt_wifi_dump.c against a synthetic LK FDT and Apple DT, on a host."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

SHIM = Path(__file__).resolve().parents[1] / 'shim'


def fdt(nodes):
    """nodes: nested (name, [(key, bytes)], [children]) -> FDT blob."""
    strings, offsets, struct_ = b'', {}, b''

    def soff(k):
        nonlocal strings
        if k not in offsets:
            offsets[k] = len(strings)
            strings += k.encode() + b'\0'
        return offsets[k]

    def emit(n):
        nonlocal struct_
        name, props, kids = n
        nb = name.encode() + b'\0'
        struct_ += struct.pack('>I', 1) + nb + b'\0' * (-len(nb) % 4)
        for k, v in props:
            struct_ += struct.pack('>III', 3, len(v), soff(k)) + v + b'\0' * (-len(v) % 4)
        for c in kids:
            emit(c)
        struct_ += struct.pack('>I', 2)
    emit(nodes)
    struct_ += struct.pack('>I', 9)
    rsv = b'\0' * 16
    off_rsv = 40
    off_struct = off_rsv + len(rsv)
    off_str = off_struct + len(struct_)
    total = off_str + len(strings)
    hdr = struct.pack('>10I', 0xd00dfeed, total, off_struct, off_str, off_rsv, 17, 16, 0, len(strings), len(struct_))
    return hdr + rsv + struct_ + strings


def adt(placeholder=8192):
    def prop(k, v):
        return k.encode().ljust(32, b'\0') + struct.pack('<I', len(v)) + v + b'\0' * (-len(v) % 4)
    chosen = struct.pack('<II', 2 if placeholder else 1, 0) + prop('name', b'chosen\0')
    if placeholder:
        chosen += prop('x9-lk-wifi-dt', bytes(placeholder))
    root = struct.pack('<II', 1, 1) + prop('name', b'device-tree\0')
    return root + chosen


def u64s(*v):
    return b''.join(struct.pack('>Q', x) for x in v)


@unittest.skipUnless(shutil.which('cc'), 'requires cc')
class ShimWifiDumpTests(unittest.TestCase):
    def test_dump(self):
        tree = ('', [('#address-cells', struct.pack('>I', 2))], [
            ('reserved-memory', [('#address-cells', struct.pack('>I', 2)), ('#size-cells', struct.pack('>I', 2))], [
                ('consys-reserve-memory', [('reg', u64s(0xf8000000, 0x1900000)), ('no-map', b'')], []),
                ('wifi-reserve-memory', [('size', u64s(0x5400000))], []),
            ]),
            ('soc', [], [
                ('pcie@16910000', [('compatible', b'mediatek,mt6993-pcie\0'), ('status', b'okay\0'),
                                   ('reg', u64s(0x16910000, 0x4000))], []),
                ('i2c@1d840000', [('status', b'okay\0')], []),
                ('wifi', [('emi-addr', struct.pack('>I', 0xa0000000))], []),
            ]),
        ])
        c = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
int fdt_wifi_dump_adt(uintptr_t,void *,size_t);
static unsigned char *rd(const char *p,size_t *n){FILE *f=fopen(p,"rb");fseek(f,0,2);*n=ftell(f);rewind(f);
 unsigned char *b=aligned_alloc(16,(*n+15)&~15ul);if(fread(b,1,*n,f)!=*n)exit(2);fclose(f);return b;}
int main(int c,char **v){(void)c;size_t fn,an;unsigned char *f=rd(v[1],&fn),*a=rd(v[2],&an);
 int r=fdt_wifi_dump_adt((uintptr_t)f,a,an);printf("%d\n",r);
 unsigned char *p=memmem(a,an,"X9DT",4);if(p)fputs((char *)p+8,stdout);return 0;}
'''
        with tempfile.TemporaryDirectory(prefix='x9-wifi-dump-') as d:
            d = Path(d)
            (d / 'fdt.bin').write_bytes(fdt(tree))
            (d / 'adt.bin').write_bytes(adt())
            (d / 'adt0.bin').write_bytes(adt(0))
            (d / 'main.c').write_text(c)
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-I', str(SHIM),
                            str(SHIM / 'fdt_wifi_dump.c'), str(d / 'main.c'), '-o', str(d / 't')], check=True)
            out = subprocess.run([str(d / 't'), str(d / 'fdt.bin'), str(d / 'adt.bin')],
                                 capture_output=True, text=True, check=True).stdout
            lines = out.splitlines()
            self.assertEqual(lines[0], '0')
            self.assertIn('R consys-reserve-memory reg=00000000.f8000000.00000000.01900000', lines)
            self.assertIn('R consys-reserve-memory no-map', lines)
            self.assertIn('R wifi-reserve-memory size=00000000.05400000', lines)
            self.assertIn('N /soc/pcie@16910000 compatible="mediatek,mt6993-pcie"', lines)
            self.assertIn('N /soc/pcie@16910000 status="okay"', lines)
            self.assertIn('N /soc/wifi emi-addr=a0000000', lines)
            self.assertFalse(any('i2c' in l for l in lines))
            out0 = subprocess.run([str(d / 't'), str(d / 'fdt.bin'), str(d / 'adt0.bin')],
                                  capture_output=True, text=True, check=True).stdout
            self.assertEqual(out0.strip(), '1')


if __name__ == '__main__':
    unittest.main()
