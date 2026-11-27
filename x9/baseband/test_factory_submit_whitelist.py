#!/usr/bin/env python3
"""Factory-init submit whitelist: the 0x601{0x108} shape in x9mm_request
(real extracted code) and the 0xa1c/0xa18/0xa00 admission predicates of
x9mi_submit's factory branches, fed with frames built by the real
x9w_frame. x9mi_submit itself is not compiled here (its kernel
dependency web is too large for a host stub); its branch predicates are
transcribed verbatim below and test_branches_present_in_sources pins
the branches' presence and shape markers in the bridge source, so
removing or reshaping a branch fails this test."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
MESSAGES = ROOT / 'x9/kernel/src/bsd/dev/x9_mipc_messages.inc'
BRIDGE = ROOT / 'x9/kernel/src/bsd/dev/x9_mipc_bridge.inc'
WDS = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9WDS.inc'


class FactorySubmitWhitelist(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_branches_present_in_sources(self):
        bridge = BRIDGE.read_text()
        submit = bridge[bridge.index('static uint64_t x9mi_submit'):]
        for marker in ('else if(id==0xa18)', 'else if(id==0xa1c)', 'else if(id==0xa00)',
                       'n!=88', 'k!=64', 'id==0xa00'):
            self.assertIn(marker, submit)
        messages = MESSAGES.read_text()
        self.assertIn('(n==24&&x9mm_one(p,n,0x108,1))', messages)
        messages_inc = (ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc').read_text()
        self.assertIn('submit rejected locally', messages_inc)

    def test_shapes(self):
        messages = MESSAGES.read_text()
        m0 = messages.index('static int x9mm_find')
        m1 = messages.index('/* Only requests whose usable session')
        wds = WDS.read_text()
        w0 = wds.index('struct x9w_frame {')
        w1 = wds.index('\n};', w0) + 3
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
static unsigned x9mi_u16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static unsigned sim_count=2;
unsigned x9_md_sim_count(){return sim_count;}
uint64_t x9_md_mipc_submit(const uint8_t*,uint32_t){return 1;}
''' + wds[w0:w1] + messages[m0:m1] + r'''
/* Transcribed verbatim from the x9mi_submit factory branches;
 * admit == the branch does not return 0. */
static int admit_a18(const uint8_t *p,unsigned n){
 if((p[8]!=1&&(p[8]!=2||x9_md_sim_count()!=2))||n!=24||!x9mm_one(p,n,0x100,0))return 0;
 return 1;
}
static int admit_a1c(const uint8_t *p,unsigned n){
 const uint8_t *v;unsigned k;
 if(p[8]!=1||n!=88||!x9mm_find(p,n,0x100,&v,&k)||!v||k!=64)return 0;
 for(unsigned i=0;i<64;i++)if(v[i])return 0;
 return 1;
}
static int admit_a00(const uint8_t *p,unsigned n){
 const uint8_t *v;unsigned k;
 if((p[8]!=1&&(p[8]!=2||x9_md_sim_count()!=2))||n!=40||
    !x9mm_find(p,n,0x100,&v,&k)||!v||k!=4||v[0]||v[1]||v[2]||v[3])return 0;
 if(!x9mm_find(p,n,0x102,&v,&k)||!v||k!=6)return 0;
 return 1;
}
int main(){
 /* 0x601 factory config tag: rejected before the whitelist fix. */
 {x9w_frame f(0x601,1);f.byte(0x108,1);assert(f.n==24);assert(x9mm_request(f.p,f.n,0x601)==1);}
 {x9w_frame f(0x601,2);f.byte(0x108,1);assert(x9mm_request(f.p,f.n,0x601)==1);}
 {x9w_frame f(0x601,1);f.byte(0x108,0);assert(x9mm_request(f.p,f.n,0x601)==0);}
 {x9w_frame f(0x601,1);f.byte(0x108,1);f.byte(0x109,1);assert(x9mm_request(f.p,f.n,0x601)==0);}
 {x9w_frame f(0x601,1);f.word(0x108,1);assert(f.n==24);assert(x9mm_request(f.p,f.n,0x601)==0);}
 {x9w_frame f(0x601,3);f.byte(0x108,1);assert(x9mm_request(f.p,f.n,0x601)==0);}
 /* Pre-existing 0x601 shapes keep their verdicts. */
 {x9w_frame f(0x601,1);f.byte(0x100,0);f.byte(0x105,1);f.byte(0x106,2);assert(f.n==40);assert(x9mm_request(f.p,f.n,0x601)==1);}
 {x9w_frame f(0x601,1);f.byte(0x102,1);assert(x9mm_request(f.p,f.n,0x601)==1);}
 {x9w_frame f(0x601,1);f.byte(0x102,0);assert(x9mm_request(f.p,f.n,0x601)==0);}
 /* 0xa1c: SIM1 only, 64 zero bytes, n=88. */
 {uint8_t z[64]={};x9w_frame f(0xa1c,1);f.tlv(0x100,z,64);assert(f.n==88);assert(admit_a1c(f.p,f.n)==1);}
 {uint8_t z[64]={};x9w_frame f(0xa1c,2);f.tlv(0x100,z,64);assert(admit_a1c(f.p,f.n)==0);}
 {uint8_t z[64]={};z[63]=1;x9w_frame f(0xa1c,1);f.tlv(0x100,z,64);assert(admit_a1c(f.p,f.n)==0);}
 {uint8_t z[63]={};x9w_frame f(0xa1c,1);f.tlv(0x100,z,63);assert(admit_a1c(f.p,f.n)==0);}
 /* 0xa18: either SIM, byte 0x100=0, n=24. */
 {x9w_frame f(0xa18,1);f.byte(0x100,0);assert(f.n==24);assert(admit_a18(f.p,f.n)==1);}
 {x9w_frame f(0xa18,2);f.byte(0x100,0);assert(admit_a18(f.p,f.n)==1);}
 {x9w_frame f(0xa18,1);f.byte(0x100,1);assert(admit_a18(f.p,f.n)==0);}
 {x9w_frame f(0xa18,2);f.byte(0x100,0);sim_count=1;assert(admit_a18(f.p,f.n)==0);sim_count=2;}
 /* 0xa00: word 0x100=0 + 6-byte 0x102, n=40. */
 {uint8_t b[6]={3,3,0,0,1,1};x9w_frame f(0xa00,1);f.word(0x100,0);f.tlv(0x102,b,6);assert(f.n==40);assert(admit_a00(f.p,f.n)==1);}
 {uint8_t b[6]={3,3,0,0,1,1};x9w_frame f(0xa00,2);f.word(0x100,0);f.tlv(0x102,b,6);assert(admit_a00(f.p,f.n)==1);}
 {uint8_t b[6]={3,3,0,0,1,1};x9w_frame f(0xa00,1);f.word(0x100,1);f.tlv(0x102,b,6);assert(admit_a00(f.p,f.n)==0);}
 {uint8_t b[5]={3,3,0,0,1};x9w_frame f(0xa00,1);f.word(0x100,0);f.tlv(0x102,b,5);assert(admit_a00(f.p,f.n)==0);}
 {x9w_frame f(0xa00,1);f.word(0x100,0);assert(admit_a00(f.p,f.n)==0);}
 printf("factory-submit-whitelist ok\n");}
'''
        self.assertIn('factory-submit-whitelist ok', self.compile_run(code))


if __name__ == '__main__':
    unittest.main()
