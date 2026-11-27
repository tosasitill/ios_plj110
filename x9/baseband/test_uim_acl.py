#!/usr/bin/env python3
"""Exercise the UIM compact-ACL translator extracted from X9QMIAT.inc.

Cases are the worked examples of ETSI TS 102 221 clause 9.2.1 plus the
boundary rules the implementation documents: absent AM bit = NEVer,
non-zero SC (key reference) = untranslatable, proprietary AM = reject,
lone DEACTIVATE/ACTIVATE bit = ambiguous.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9QMIAT.inc'


class UimAcl(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_compact_translation(self):
        source = SOURCE.read_text()
        start = source.index('static bool x9q_acl_translate(')
        end = source.index('static bool x9q_fcp(', start)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
''' + source[start:end] + r'''
static void run(const uint8_t *v,unsigned n,bool ok,const uint8_t want[5]){
 uint8_t logic[5];uint16_t mask[5];
 bool got=x9q_acl_translate(v,n,logic,mask);
 assert(got==ok);
 if(ok){assert(!memcmp(logic,want,5));for(int k=0;k<5;k++)assert(!mask[k]);}
}
int main(){
 const uint8_t alw_read_write[5]={0,0,1,1,1};
 const uint8_t alw_read[5]={0,1,1,1,1};
 const uint8_t alw_write[5]={1,0,1,1,1};
 const uint8_t alw_both[5]={0,0,1,0,0};
 /* TS 102 221: AM 0x03 + SC 00 00 = READ and UPDATE ALW. */
 {const uint8_t v[]={0x03,0x00,0x00};run(v,3,true,alw_read_write);}
 /* TS 102 221: AM 0x01 + SC 00 = READ ALW, all else NEV. */
 {const uint8_t v[]={0x01,0x00};run(v,2,true,alw_read);}
 /* TS 102 221 EF_DIR/EF_ICC: AM 0x1B, SC 90 90 90 00 (ADM) -> not translated. */
 {const uint8_t v[]={0x1b,0x90,0x90,0x90,0x00};run(v,5,false,nullptr);}
 /* Absent AM bit is NEVer even when another op is ALW. */
 {const uint8_t v[]={0x02,0x00};run(v,2,true,alw_write);}
 /* OR of two rules, each fully ALW for its own op. */
 {const uint8_t v[]={0x01,0x00,0x02,0x00};run(v,4,true,alw_read_write);}
 /* DEACTIVATE+ACTIVATE together (b4|b5) with SC 00: both ALW. */
 {const uint8_t v[]={0x1b,0x00,0x00,0x00,0x00};run(v,5,true,alw_both);}
 /* A lone b4 is ambiguous between deactivate/activate: refuse. */
 {const uint8_t v[]={0x08,0x00};run(v,2,false,nullptr);}
 /* Proprietary AM (b8 set), truncation, and empty input all refuse. */
 {const uint8_t v[]={0x83,0x00,0x00};run(v,3,false,nullptr);}
 {const uint8_t v[]={0x03,0x00};run(v,2,false,nullptr);}
 run(nullptr,0,false,nullptr);
 printf("uim-acl ok\n");}
'''
        out = self.compile_run(code)
        self.assertIn('uim-acl ok', out)

    def test_source_wiring(self):
        source = SOURCE.read_text()
        # The 0x24 builder must consult the translator and keep a fallback.
        self.assertIn('x9q_acl_translate(j.sec_compact,j.sec_compact_len,acl_logic,acl_mask)', source)
        self.assertIn('bridge-permission fallback', source)
        # The FCP walk records all three security forms without inventing data.
        self.assertIn('else if(tag==0x8b)j.sec_form=1;', source)
        self.assertIn('else if(tag==0xab)j.sec_form=3;', source)
        # RegisterEvents still rejects unsupported bits; the log is observe-only.
        self.assertIn('carries unsupported event bits', source)
        self.assertIn('x9q_u32(m.p)&~0x11u', source)


if __name__ == '__main__':
    unittest.main()
