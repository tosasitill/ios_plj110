#!/usr/bin/env python3
"""Exercise the production boot IMS observer with a fake MIPC transport."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'

class BootIms(unittest.TestCase):
    def test_independent_sim_snapshot_and_stale_confirmation(self):
        source = SOURCE.read_text()
        start = source.index('static void x9m_ims_poll(')
        end = source.index('static bool x9m_diag_enabled()', start)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
struct x9m_sim { unsigned ims_stage=0, reg_epoch=0, ims_epoch=0, reg=99; uint64_t ims_token=0,ims_deadline=0,ims_admit_deadline=0; bool ims_hold=false,reg_valid=false; };
x9m_sim x9m_sims[2]; bool x9w_control_ready=true, message_online=true; uint64_t clock_ms=1, next_token=1; unsigned frames[10]={}, masks[10]={}, count=0;
unsigned x9_md_sim_count(){return 2;} bool x9_md_mipc_online(){return true;} bool x9_md_mipc_message_online(){return message_online;} uint64_t x9q_ms(){return clock_ms;}
struct x9w_frame { unsigned id,mask; x9w_frame(unsigned i,unsigned m):id(i),mask(m){} void tlv(unsigned,const uint8_t*,unsigned){} void byte(unsigned,unsigned){} uint64_t submit(){frames[count]=id;masks[count++]=mask;return next_token++;} };
uint8_t answer[2][16]={}; bool ready[2]={};
int x9_md_mipc_take(uint64_t token,uint8_t *p,unsigned,unsigned *n){unsigned i=(token-1)%2;if(!ready[i])return 0;ready[i]=false;memcpy(p,answer[i],16);*n=16;return 1;}
bool x9w_wire(const uint8_t*,unsigned n){return n==16;} unsigned x9q_u16(const uint8_t*p){return p[0]|(p[1]<<8);} bool x9w_word(const uint8_t*p,unsigned,unsigned,unsigned &v){v=p[2];return true;} bool x9w_byte(const uint8_t*p,unsigned,unsigned tag,unsigned &v){v=p[tag==0x100?3:4];return true;}
void x9m_reg(unsigned sim,unsigned reg){x9m_sims[sim].reg=reg;x9m_sims[sim].reg_valid=true;}
''' + source[start:end] + r'''
void cnf(unsigned sim,unsigned id,unsigned state=0){answer[sim][10]=id&255;answer[sim][11]=id>>8;answer[sim][8]=1u<<sim;answer[sim][4]=state;ready[sim]=true;}
int main(){uint8_t p[32]={};x9m_ims_poll(p,sizeof(p));assert(count==2&&frames[0]==0x307&&frames[1]==0x307&&masks[0]==1&&masks[1]==2);
 cnf(0,0x308);cnf(1,0x308);x9m_ims_poll(p,sizeof(p));assert(count==4&&frames[2]==0xa04&&frames[3]==0xa04);
 ++x9m_sims[0].reg_epoch;x9m_reg(0,0);cnf(0,0xa05,2);cnf(1,0xa05,2);x9m_ims_poll(p,sizeof(p));assert(x9m_sims[0].reg==0&&x9m_sims[1].reg==2);
 x9m_ims_poll(p,sizeof(p));assert(count==4);
 // A fresh observer that sees state 1 must leave registration unknown.
 x9m_sims[0]={};x9m_sims[1]={};count=0;next_token=1;
 x9m_ims_poll(p,sizeof(p));cnf(0,0x308);cnf(1,0x308);x9m_ims_poll(p,sizeof(p));
 cnf(0,0xa05,1);cnf(1,0xa05,3);x9m_ims_poll(p,sizeof(p));
 assert(!x9m_sims[0].reg_valid&&x9m_sims[0].reg_epoch==1&&x9m_sims[1].reg==0);
 // Timed-out owners drain late confirmations without publishing or retrying.
 x9m_sims[0]={};x9m_sims[1]={};count=0;next_token=1;
 x9m_ims_poll(p,sizeof(p));clock_ms+=60000;cnf(0,0x308);cnf(1,0x308);x9m_ims_poll(p,sizeof(p));
 assert(x9m_sims[0].ims_hold&&x9m_sims[1].ims_hold&&count==2);
 // An unavailable SMS indication inbox cannot starve the independent read.
 x9m_sims[0]={};x9m_sims[1]={};count=0;next_token=1;message_online=false;
 x9m_ims_poll(p,sizeof(p));assert(count==2&&frames[0]==0xa04&&frames[1]==0xa04);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == '__main__': unittest.main()
