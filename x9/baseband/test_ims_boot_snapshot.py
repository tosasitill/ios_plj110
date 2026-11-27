#!/usr/bin/env python3
"""Exercise the production boot IMS observer with a fake MIPC transport."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'

class BootIms(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_independent_sim_snapshot_and_stale_confirmation(self):
        source = SOURCE.read_text()
        start = source.index('static void x9m_ims_poll(')
        end = source.index('static bool x9m_diag_enabled()', start)
        feature_start = source.index('static void x9m_ims_feature_poll(')
        feature_end = source.index('static void x9m_rawsend_reject_diag(', feature_start)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
struct x9m_sim { unsigned ims_stage=0, reg_epoch=0, ims_epoch=0, reg=99; uint64_t ims_token=0,ims_deadline=0,ims_admit_deadline=0; bool ims_hold=false,reg_valid=false; };
x9m_sim x9m_sims[2]; bool x9w_control_ready=true, message_online=true, feature_diag=false; uint64_t clock_ms=1, next_token=1; unsigned frames[10]={}, masks[10]={}, count=0;
uint64_t abandoned[2]={}; unsigned abandoned_count=0;
void x9_md_mipc_ims_registration_abandon(uint64_t token){abandoned[abandoned_count++]=token;}
uint64_t abandoned_feature=0;
void x9_md_mipc_ims_feature_abandon(uint64_t token){abandoned_feature=token;}
int sweet_x9_arg(const char*,int){return feature_diag;}
unsigned x9_md_sim_count(){return 2;} bool x9_md_mipc_online(){return true;} bool x9_md_mipc_message_online(){return message_online;} uint64_t x9q_ms(){return clock_ms;}
struct x9w_frame { unsigned id,mask; x9w_frame(unsigned i,unsigned m):id(i),mask(m){} void tlv(unsigned,const uint8_t*,unsigned){} void byte(unsigned,unsigned){} void word(unsigned tag,unsigned value){assert(id==0xa02&&tag==0x100&&!value);} uint64_t submit(){frames[count]=id;masks[count++]=mask;return next_token++;} };
uint8_t answer[2][16]={}; bool ready[2]={};
int x9_md_mipc_take(uint64_t token,uint8_t *p,unsigned,unsigned *n){unsigned i=(token-1)%2;if(!ready[i])return 0;ready[i]=false;memcpy(p,answer[i],16);*n=16;return 1;}
bool x9w_wire(const uint8_t*,unsigned n){return n==16;} unsigned x9q_u16(const uint8_t*p){return p[0]|(p[1]<<8);} bool x9w_word(const uint8_t*p,unsigned,unsigned,unsigned &v){v=p[2];return true;} bool x9w_byte(const uint8_t*p,unsigned,unsigned tag,unsigned &v){v=p[tag==0x100?3:4];return true;}
void x9m_reg(unsigned sim,unsigned reg){x9m_sims[sim].reg=reg;x9m_sims[sim].reg_valid=true;}
struct x9q_view {const uint8_t *p;unsigned n;};
unsigned feature_reads=0;
x9q_view x9w_tlv(const uint8_t *p,unsigned,unsigned tag){assert(tag==0x100);feature_reads++;return {p,6};}
''' + source[start:end] + source[feature_start:feature_end] + r'''
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
 // Read-only owners expire without publishing late data or retrying; SIMs
 // are independent, and subscription tokens never use the read abandon API.
 uint64_t first=x9m_sims[0].ims_token,second=x9m_sims[1].ims_token;
 clock_ms+=60000;cnf(0,0xa05,2);cnf(1,0xa05,2);x9m_ims_poll(p,sizeof(p));
 assert(abandoned_count==2&&abandoned[0]==first&&abandoned[1]==second);
 assert(!x9m_sims[0].ims_token&&!x9m_sims[1].ims_token&&!x9m_sims[0].reg_valid&&!x9m_sims[1].reg_valid&&count==2);
 x9m_ims_poll(p,sizeof(p));assert(count==2&&abandoned_count==2);
 // Configuration reads are opt-in and use independent per-SIM owners.
 ready[0]=ready[1]=false;
 x9m_ims_feature_poll(p,sizeof(p));assert(count==2);
 feature_diag=true;x9m_ims_feature_poll(p,sizeof(p));
 assert(count==4&&frames[2]==0xa02&&frames[3]==0xa02&&masks[2]==1&&masks[3]==2);
 cnf(0,0xa03);answer[0][8]=2; // wrong-SIM confirmation cannot publish cached data
 x9m_ims_feature_poll(p,sizeof(p));assert(!feature_reads);
 clock_ms+=10000;
 cnf(0,0xa03);cnf(1,0xa03);x9m_ims_feature_poll(p,sizeof(p));
 assert(abandoned_feature==4&&count==4&&!feature_reads);
 x9m_ims_feature_poll(p,sizeof(p));assert(count==4);
}
'''
        self.compile_run(code)

    def test_feature_cache_prints_raw_and_decoded_bits(self):
        source = SOURCE.read_text()
        start = source.index('static void x9m_ims_feature_poll(')
        end = source.index('static void x9m_rawsend_reject_diag(', start)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
struct x9m_sim { unsigned ims_stage=0; uint64_t ims_token=0,ims_deadline=0; bool ims_hold=false,reg_valid=true; };
x9m_sim x9m_sims[1]; bool x9w_control_ready=true; uint64_t clock_ms=1,next_token=1;
unsigned count=0,feature_reads=0; uint8_t answer[16]={};
int sweet_x9_arg(const char*,int){return 1;}
unsigned x9_md_sim_count(){return 1;} bool x9_md_mipc_online(){return true;} uint64_t x9q_ms(){return clock_ms;}
struct x9w_frame { unsigned id,mask; x9w_frame(unsigned i,unsigned m):id(i),mask(m){} void tlv(unsigned,const uint8_t*,unsigned){} void byte(unsigned,unsigned){} void word(unsigned tag,unsigned value){assert(id==0xa02&&tag==0x100&&!value);} uint64_t submit(){assert(id==0xa02);count++;return next_token++;} };
int x9_md_mipc_take(uint64_t,uint8_t *p,unsigned,unsigned *n){memcpy(p,answer,16);*n=16;return 1;}
void x9_md_mipc_ims_feature_abandon(uint64_t){assert(false);}
bool x9w_wire(const uint8_t*,unsigned n){return n==16;} unsigned x9q_u16(const uint8_t*p){return p[0]|(p[1]<<8);} bool x9w_word(const uint8_t*p,unsigned,unsigned,unsigned &v){v=p[2];return true;}
struct x9q_view {const uint8_t *p;unsigned n;};
x9q_view x9w_tlv(const uint8_t *p,unsigned,unsigned tag){assert(tag==0x100);feature_reads++;return {p+3,6};}
''' + source[start:end] + r'''
int main(){uint8_t p[32]={};
 answer[2]=0;answer[8]=1;answer[10]=0xa03&255;answer[11]=0xa03>>8;memcpy(answer+3,"\x03\x03\x00\x00\x01\x01",6);
 x9m_ims_feature_poll(p,sizeof(p));assert(count==1);
 x9m_ims_feature_poll(p,sizeof(p));assert(count==1&&feature_reads==1);
}
'''
        self.assertIn('feature cache raw=030300000101 volte=1 vonr=1 vilte=1 vinr=1 '
                      'wfc=0 viwifi=0 sms=1 ims=1', self.compile_run(code))

if __name__ == '__main__': unittest.main()
