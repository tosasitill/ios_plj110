#!/usr/bin/env python3
"""Exercise the factory SMS memory-status report point (0601 tag 0x108)."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'


class MemStatus(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_report_point_and_dedupe(self):
        source = SOURCE.read_text()
        # The init-step send and this event point share one dedupe flag.
        self.assertIn('if(s.init==X9M_FACTORY_CFG)s.mem_sent=true;', source)
        b0 = source.index('static void x9m_mem_status_poll(')
        b1 = source.index('static void x9m_init_transport_loss(', b0)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
struct x9m_sim { bool ready=false, mem_sent=false, mem_hold=false; uint64_t mem_token=0, mem_deadline=0; };
x9m_sim x9m_sims[2]; bool x9w_control_ready=true; bool factory_on=true; uint64_t clock_ms=1, next_token=1;
bool x9m_factory_enabled(){return factory_on;}
unsigned x9_md_sim_count(){return 2;} bool x9_md_mipc_online(){return true;} uint64_t x9q_ms(){return clock_ms;}
struct Rec { unsigned id=0, mask=0, byte108=99; };
Rec recs[8]; unsigned count=0; uint64_t tok_sim[64]={}; int reject_sim=-1;
struct x9w_frame { unsigned id, mask; Rec *r;
 x9w_frame(unsigned i, unsigned m):id(i),mask(m){r=&recs[count];r->id=i;r->mask=m;}
 void tlv(unsigned,const void*,unsigned){}
 void byte(unsigned tag, uint8_t v){ if(tag==0x108)r->byte108=v; }
 void word(unsigned, uint32_t){}
 uint64_t submit(){ count++; uint64_t t=next_token++; tok_sim[t]=(mask==1)?0:1; return t; } };
uint8_t answer[32];
int x9_md_mipc_take(uint64_t token, uint8_t *p, unsigned, unsigned *n){
 unsigned sim=tok_sim[token]; memset(answer,0,sizeof(answer));
 answer[8]=(uint8_t)(1u<<sim); answer[10]=0x02; answer[11]=0x06;
 answer[2]=(reject_sim==(int)sim)?1:0;
 memcpy(p,answer,32); *n=32; return 1; }
bool x9w_wire(const uint8_t*, unsigned){return true;}
unsigned x9q_u16(const uint8_t *p){return p[0]|(p[1]<<8);}
bool x9w_word(const uint8_t *p, unsigned, unsigned, unsigned &v){v=p[2];return true;}
''' + source[b0:b1] + r'''
int main(){uint8_t p[64]={};
 /* Gate off: nothing, even at host-ready. */
 factory_on=false; x9m_sims[0].ready=x9m_sims[1].ready=true;
 x9m_mem_status_poll(p,sizeof(p)); assert(count==0);
 /* Gate on but host not ready: nothing. */
 factory_on=true; x9m_sims[0].ready=x9m_sims[1].ready=false;
 x9m_mem_status_poll(p,sizeof(p)); assert(count==0);
 /* Host-ready: exactly one 0601{108=1} per SIM, then silence. */
 x9m_sims[0].ready=x9m_sims[1].ready=true;
 for(int i=0;i<8;i++)x9m_mem_status_poll(p,sizeof(p));
 assert(count==2&&recs[0].id==0x601&&recs[1].id==0x601);
 assert(recs[0].byte108==1&&recs[1].byte108==1);
 assert(x9m_sims[0].mem_sent&&x9m_sims[1].mem_sent);
 /* Dedupe: init-step send already reported SIM1 -> only SIM2 fires. */
 count=0; x9m_sims[0]=x9m_sim{}; x9m_sims[1]=x9m_sim{};
 x9m_sims[0].ready=x9m_sims[1].ready=true; x9m_sims[0].mem_sent=true;
 for(int i=0;i<8;i++)x9m_mem_status_poll(p,sizeof(p));
 assert(count==1&&recs[0].mask==2);
 /* Rejection holds without retry; the other SIM still completes. */
 count=0; x9m_sims[0]=x9m_sim{}; x9m_sims[1]=x9m_sim{};
 x9m_sims[0].ready=x9m_sims[1].ready=true; reject_sim=1;
 for(int i=0;i<8;i++)x9m_mem_status_poll(p,sizeof(p));
 assert(count==2);
 assert(x9m_sims[1].mem_hold&&!x9m_sims[1].mem_sent);
 assert(x9m_sims[0].mem_sent);
 printf("mem-status ok\n");}
'''
        out = self.compile_run(code)
        self.assertIn('mem-status ok', out)
        self.assertIn('memory status reported at host-ready', out)


if __name__ == '__main__':
    unittest.main()
