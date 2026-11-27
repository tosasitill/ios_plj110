#!/usr/bin/env python3
"""Factory IMS ACT (0201) byte-exactness: drive the real x9m_factory_poll
(X9Messages.inc) with the real x9w_frame (X9WDS.inc) and captured
transport stubs, and compare the submitted stage-4 frame byte for byte
against the factory DATA_ACT_CALL_REQ for APN ims (radio-baseline.txt
2026-11-15 18:38:15.635, TXID 034c). The baseline logcat dump truncates
at 478 B; the wire frame is 720 B (header payload len 704). Bytes
12..13 (txid) are stamped by the bridge at submit time, so the factory
copy is compared with those two bytes zeroed. The one content byte the
old construction got wrong: URSP descriptor +414 prot_id_next_hdr,
factory 0xff (unspecified), was 0x00 -- the MD answered 0x15000A."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
MESSAGES = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'
WDS = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9WDS.inc'
FACTORY_B_HEX = '8419542400000000010001024C03C0020F0104000000000002010400020000000A010100030000000D01A00200030000696D730000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000FF00000000000000000000000000000000000000'


class FactoryActFrame(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_source_pins_factory_fill(self):
        self.assertIn('td[414]=0xff', MESSAGES.read_text())

    def test_act_frame_byte_exact(self):
        wds = WDS.read_text()
        w0 = wds.index('struct x9w_frame {')
        w1 = wds.index('/* P-CSCF lists decoded', w0)
        msg = MESSAGES.read_text()
        m0 = msg.index('static void x9m_factory_ims_bytes(')
        m1 = msg.index('static bool x9m_diag_storage_busy(', m0)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
static unsigned x9q_u16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static uint32_t x9q_u32(const uint8_t *p){return p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24);}
static const unsigned X9_MIPC_FRAME_MAX=4096;
struct x9q_view { const uint8_t *p; unsigned n; };
uint64_t x9_md_mipc_submit(const uint8_t *p,uint32_t n);
''' + wds[w0:w1] + r'''
struct x9m_sim { unsigned ims_stage=0, fx_stage=0; uint64_t fx_token=0, fx_deadline=0; bool ims_hold=false, fx_hold=false; };
x9m_sim x9m_sims[2];
struct x9w_pcscf_list { uint8_t v4_count=0, v6_count=0; uint8_t v4[4][4]={}; uint8_t v6[4][16]={}; };
x9w_pcscf_list x9w_pcscf_lists[2];
void x9w_pcscf_publish(unsigned, const uint8_t*, unsigned){}
bool x9w_control_ready=true;
bool x9m_factory_enabled(){return true;}
unsigned x9_md_sim_count(){return 2;}
bool x9_md_mipc_online(){return true;}
uint64_t x9q_ms(){return 1000;}
static uint64_t next_tok=100;
static unsigned sub_id[256], sub_mask[256];
static uint8_t act_frame[3][1024]; static unsigned act_n[3];
uint64_t x9_md_mipc_submit(const uint8_t *p,uint32_t n){
 uint64_t t=next_tok++;
 unsigned id=x9q_u16(p+10);
 sub_id[t]=id; sub_mask[t]=p[8];
 if(id==0x201){memcpy(act_frame[p[8]],p,n);act_n[p[8]]=n;}
 return t;
}
int x9_md_mipc_take(uint64_t token, uint8_t *p, unsigned room, unsigned *n){
 unsigned id=sub_id[token], mask=sub_mask[token], cnf=id+1;
 uint8_t b[64]; memset(b,0,sizeof(b));
 b[0]=0x84;b[1]=0x19;b[2]=0x54;b[3]=0x24;b[8]=(uint8_t)mask;
 b[10]=(uint8_t)cnf;b[11]=(uint8_t)(cnf>>8);
 unsigned at=16;
 b[at]=0;b[at+1]=0;b[at+2]=4;b[at+3]=0; at+=8;
 if(cnf==0xa03){ static const uint8_t cache[6]={3,3,0,0,1,1};
  b[at]=0x00;b[at+1]=0x01;b[at+2]=6;b[at+3]=0; memcpy(b+at+4,cache,6); at+=16; }
 b[14]=(uint8_t)(at-16); b[15]=(uint8_t)((at-16)>>8);
 memcpy(p,b,at); *n=at; (void)room; return 1;
}
''' + msg[m0:m1] + r'''
static int hexval(char c){return c<='9'?c-'0':c<='F'?c-'A'+10:c-'a'+10;}
int main(){
 static const char *hex=FACTORY_B_HEX_PLACEHOLDER;
 uint8_t want[478];
 for(unsigned i=0;i<478;i++)want[i]=(uint8_t)(hexval(hex[2*i])*16+hexval(hex[2*i+1]));
 want[12]=0; want[13]=0;
 static uint8_t p[2048];
 unsigned unused_v=0; (void)x9w_byte(p,0,0,unused_v);
 x9m_sims[0].ims_stage=4; x9m_sims[1].ims_stage=4;
 for(int i=0;i<24;i++)x9m_factory_poll(p,sizeof(p));
 assert(x9m_sims[0].fx_stage==6&&x9m_sims[1].fx_stage==6);
 assert(!x9m_sims[0].fx_hold&&!x9m_sims[1].fx_hold);
 for(unsigned mask=1;mask<=2;mask++){
  assert(act_n[mask]==720);
  const uint8_t *f=act_frame[mask];
  assert(f[8]==mask&&x9q_u16(f+10)==0x201);
  assert((f[14]|(f[15]<<8))==704);
  want[8]=(uint8_t)mask;
  assert(!memcmp(f,want,478));
  for(unsigned i=478;i<720;i++)assert(f[i]==0);
 }
 for(unsigned i=0;i<720;i++)if(i!=8)assert(act_frame[1][i]==act_frame[2][i]);
 printf("factory-act-frame ok\n");}
'''
        code = code.replace('FACTORY_B_HEX_PLACEHOLDER', '"' + FACTORY_B_HEX + '"')
        out = self.compile_run(code)
        self.assertIn('factory-act-frame ok', out)
        self.assertIn('X9 FACTORY: SIM1 IMS data call accepted by modem', out)
        self.assertIn('X9 FACTORY: SIM2 IMS data call accepted by modem', out)


if __name__ == '__main__':
    unittest.main()
