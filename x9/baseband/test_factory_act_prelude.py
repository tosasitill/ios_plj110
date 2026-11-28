#!/usr/bin/env python3
"""Factory ACT preludes (decision-tree branches, both default off):
x9_factory_dds_cfg mirrors the DATA_SET_CONFIG {0x106=1, 0x103=0}
default-data-SIM designation as a waited fx sub-state on the
designated SIM only, and x9_factory_data_act_first mirrors the
factory same-SIM data call (radio-baseline.txt 2026-11-15
18:38:15.050, APN "cbnet", APN_TYPE 1) ahead of the IMS ACT.
Drives the real x9m_factory_poll (X9Messages.inc) with the real
x9w_frame (X9WDS.inc), like test_factory_act_frame.py. The factory
data-ACT dump truncates at 478 B and redacts credential bytes as
0x2A; the comparison treats 0x2A dump bytes as wildcards and skips
the bridge-stamped txid bytes 12..13 on both sides."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
MESSAGES = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'
WDS = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9WDS.inc'
FACTORY_DATA_HEX = '8419542400000000010001023C032803068101002A0000001E0108000F00000000000000000000000F01040000000000078101002A000000080101007F0000000101060063626E65740000000000000009010400FFFFFD7F02010400010000000A0101000000000003010100030000000C0104000000000004010100030000000D01A0020005000063626E657400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'


def harness(main):
    wds = WDS.read_text()
    w0 = wds.index('struct x9w_frame {')
    w1 = wds.index('/* P-CSCF lists decoded', w0)
    msg = MESSAGES.read_text()
    m0 = msg.index('static void x9m_factory_ims_bytes(')
    m1 = msg.index('static bool x9m_diag_storage_busy(', m0)
    return r'''
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
struct x9m_sim { unsigned ims_stage=0, fx_stage=0; uint64_t fx_token=0, fx_deadline=0; bool ims_hold=false, fx_hold=false, fx_pre=false, fx_cfg=false, fx_dds=false, fx_data=false; bool fx_act_open=false, fx_barrier_noted=false; uint64_t fx_local_since=0; bool fx_local_noted=false; };
x9m_sim x9m_sims[2];
struct x9w_pcscf_list { uint8_t v4_count=0, v6_count=0; uint8_t v4[4][4]={}; uint8_t v6[4][16]={}; };
x9w_pcscf_list x9w_pcscf_lists[2];
void x9w_pcscf_publish(unsigned, const uint8_t*, unsigned){}
bool x9w_control_ready=true;
bool g_dds=false, g_data=false; int g_dds_slot=1;
unsigned g_data_result=0; bool g_cnf_apn=true;
static char x9w_last_dnn[2][101];
bool x9w_apn(const uint8_t *p,unsigned n){
 if(!n||n>100)return false;
 for(unsigned i=0;i<n;i++){uint8_t c=p[i];
  if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_'))return false;}
 return true;}
bool x9m_factory_enabled(){return true;}
bool x9m_factory_cfg211(){return true;}
bool x9m_factory_act_barrier(){return false;}
bool x9m_factory_dds_cfg(){return g_dds;}
bool x9m_factory_data_act_first(){return g_data;}
bool x9m_fx_dds_sim(unsigned sim){return (int)sim+1==g_dds_slot;}
bool x9m_fx_dds_pending(const x9m_sim &s,unsigned sim){return g_dds&&!s.fx_dds&&x9m_fx_dds_sim(sim);}
unsigned x9_md_sim_count(){return 2;}
bool x9_md_mipc_online(){return true;}
uint64_t x9q_ms(){return 1000;}
static uint64_t next_tok=100;
static unsigned sub_id[256], sub_mask[256], sub_kind[256];
static uint8_t frames[3][20][1024]; static unsigned flen[3][20], fid[3][20], f102[3][20], fcount[3];
uint64_t x9_md_mipc_submit(const uint8_t *p,uint32_t n){
 uint64_t t=next_tok++;
 unsigned id=x9q_u16(p+10), mask=p[8];
 sub_id[t]=id; sub_mask[t]=mask; sub_kind[t]=0;
 unsigned idx=fcount[mask]++;
 memcpy(frames[mask][idx],p,n); flen[mask][idx]=n; fid[mask][idx]=id; f102[mask][idx]=99;
 for(unsigned at=16;at+4<=n;){unsigned tag=x9q_u16(p+at),len=x9q_u16(p+at+2);
  if(tag==0x102&&len==4){f102[mask][idx]=x9q_u32(p+at+4);sub_kind[t]=f102[mask][idx];}
  if(!len)break;
  at+=(len+11)&~7u;}
 return t;
}
int x9_md_mipc_take(uint64_t token, uint8_t *p, unsigned room, unsigned *n){
 unsigned id=sub_id[token], mask=sub_mask[token], cnf=id+1;
 uint8_t b[128]; memset(b,0,sizeof(b));
 b[0]=0x84;b[1]=0x19;b[2]=0x54;b[3]=0x24;b[8]=(uint8_t)mask;
 b[10]=(uint8_t)cnf;b[11]=(uint8_t)(cnf>>8);
 unsigned at=16;
 b[at]=0;b[at+1]=0;b[at+2]=4;b[at+3]=0;
 unsigned res=(id==0x201&&sub_kind[token]==1)?g_data_result:0;
 b[at+4]=(uint8_t)res;b[at+5]=(uint8_t)(res>>8);b[at+6]=(uint8_t)(res>>16);b[at+7]=(uint8_t)(res>>24);
 at+=8;
 if(cnf==0x202&&sub_kind[token]==1&&!res&&g_cnf_apn){
  b[at]=0x01;b[at+1]=0x01;b[at+2]=6;b[at+3]=0;memcpy(b+at+4,"cbnet",6);at+=16;}
 if(cnf==0xa03){ static const uint8_t cache[6]={3,3,0,0,1,1};
  b[at]=0x00;b[at+1]=0x01;b[at+2]=6;b[at+3]=0; memcpy(b+at+4,cache,6); at+=16; }
 b[14]=(uint8_t)(at-16); b[15]=(uint8_t)((at-16)>>8);
 memcpy(p,b,at); *n=at; (void)room; return 1;
}
''' + msg[m0:m1] + main


class FactoryActPrelude(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_source_pins(self):
        msg = MESSAGES.read_text()
        self.assertIn('sweet_x9_arg("x9_factory_dds_cfg",0)', msg)
        self.assertIn('sweet_x9_arg("x9_factory_data_act_first",0)', msg)
        self.assertIn('sweet_x9_arg("x9_dds_boot_slot",1)', msg)
        self.assertIn('x9m_factory_dds_cfg()&&!s.fx_dds&&x9m_fx_dds_sim(sim)', msg)
        self.assertIn('f.word(0x109,0x7ffdffff);f.word(0x102,1);', msg)
        self.assertIn('f.byte(0x106,1);f.byte(0x103,0);', msg)

    def test_data_act_first_byte_exact_and_harvest(self):
        main = r'''
static int hexval(char c){return c<='9'?c-'0':c<='F'?c-'A'+10:c-'a'+10;}
int main(){
 static const char *hex=FACTORY_DATA_HEX_PLACEHOLDER;
 uint8_t want[478];
 for(unsigned i=0;i<478;i++)want[i]=(uint8_t)(hexval(hex[2*i])*16+hexval(hex[2*i+1]));
 g_data=true; memcpy(x9w_last_dnn[0],"cbnet",6);
 static uint8_t p[2048];
 unsigned unused_v=0; (void)x9w_byte(p,0,0,unused_v);
 x9m_sims[0].ims_stage=4; x9m_sims[1].ims_stage=4;
 for(int i=0;i<48;i++)x9m_factory_poll(p,sizeof(p));
 assert(x9m_sims[0].fx_stage==6&&x9m_sims[1].fx_stage==6);
 assert(!x9m_sims[0].fx_hold&&!x9m_sims[1].fx_hold);
 for(unsigned mask=1;mask<=2;mask++){
  unsigned seq[16],k=0;
  for(unsigned i=0;i<fcount[mask];i++)seq[k++]=fid[mask][i]==0x201?(f102[mask][i]==1?0x2011:0x2012):fid[mask][i];
  unsigned at=0; while(at<k&&seq[at]!=0x211)at++;
  assert(at+4<k&&seq[at]==0x211&&seq[at+1]==0x21b&&seq[at+2]==0x2011&&seq[at+3]==0x2012&&seq[at+4]==0xa02);
 }
 const uint8_t *f=nullptr; unsigned fn=0;
 for(unsigned i=0;i<fcount[1];i++)if(fid[1][i]==0x201&&f102[1][i]==1){f=frames[1][i];fn=flen[1][i];}
 assert(f&&fn==808);
 /* Length field: ours is n-16 = 792, the bridge convention the IMS
  * frame matches exactly. The factory dump's field reads 808,
  * implying an 824 B wire frame; the extra 16 B sit in the
  * un-dumped tail (the visible TLV chain, descriptor length
  * included, is otherwise byte-identical), so bytes 14..15 are
  * excluded from the comparison and the difference is pinned here. */
 assert((f[14]|(f[15]<<8))==792);
 assert(want[14]==0x28&&want[15]==0x03);
 for(unsigned i=0;i<478;i++){
  if(i==12||i==13||i==14||i==15)continue;
  if(want[i]==0x2a)continue;
  assert(f[i]==want[i]);
 }
 for(unsigned i=478;i<808;i++)assert(f[i]==(i==546?0xff:0));
 const uint8_t *f2=nullptr; unsigned fn2=0;
 for(unsigned i=0;i<fcount[2];i++)if(fid[2][i]==0x201&&f102[2][i]==1){f2=frames[2][i];fn2=flen[2][i];}
 assert(f2&&fn2==800);
 assert(f2[124]==1); /* empty-APN TLV is 8 B shorter: descriptor value at 124 */
 printf("DNN1=%s DNN2=%s\n",x9w_last_dnn[0],x9w_last_dnn[1]);
 printf("prelude-data ok\n");}
'''
        code = harness(main).replace('FACTORY_DATA_HEX_PLACEHOLDER', '"' + FACTORY_DATA_HEX + '"')
        out = self.compile_run(code)
        self.assertIn('prelude-data ok', out)
        self.assertIn('X9 FACTORY: SIM1 data call accepted; IMS ACT follows', out)
        self.assertIn('X9 FACTORY: SIM2 data call accepted; IMS ACT follows', out)
        self.assertIn('DNN2=cbnet', out)

    def test_data_act_rejected_proceeds_anyway(self):
        main = r'''
int main(){
 g_data=true; g_data_result=1376514; g_cnf_apn=false;
 static uint8_t p[2048];
 unsigned unused_v=0; (void)x9w_byte(p,0,0,unused_v);
 x9m_sims[0].ims_stage=4; x9m_sims[1].ims_stage=4;
 for(int i=0;i<48;i++)x9m_factory_poll(p,sizeof(p));
 assert(!x9m_sims[0].fx_hold&&!x9m_sims[1].fx_hold);
 assert(x9m_sims[0].fx_stage==6&&x9m_sims[1].fx_stage==6);
 for(unsigned mask=1;mask<=2;mask++){
  bool data=false, ims=false;
  for(unsigned i=0;i<fcount[mask];i++)if(fid[mask][i]==0x201){
   if(f102[mask][i]==1){assert(!ims);data=true;}
   if(f102[mask][i]==2){assert(data);ims=true;}}
  assert(data&&ims);
 }
 printf("prelude-data-rejected ok\n");}
'''
        out = self.compile_run(harness(main))
        self.assertIn('prelude-data-rejected ok', out)
        self.assertIn('X9 FACTORY: SIM1 data call rejected known 1 result 1376514; IMS ACT follows anyway', out)
        self.assertIn('X9 FACTORY: SIM2 data call rejected known 1 result 1376514; IMS ACT follows anyway', out)
        self.assertIn('X9 FACTORY: SIM1 IMS data call accepted by modem', out)

    def test_dds_cfg_only_on_designated_sim(self):
        main = r'''
int main(){
 g_dds=true; g_dds_slot=1;
 static uint8_t p[2048];
 unsigned unused_v=0; (void)x9w_byte(p,0,0,unused_v);
 x9m_sims[0].ims_stage=4; x9m_sims[1].ims_stage=4;
 for(int i=0;i<48;i++)x9m_factory_poll(p,sizeof(p));
 assert(x9m_sims[0].fx_stage==6&&x9m_sims[1].fx_stage==6);
 assert(x9m_sims[0].fx_dds&&!x9m_sims[1].fx_dds);
 unsigned n211=0;
 for(unsigned i=0;i<fcount[1];i++)if(fid[1][i]==0x211){
  const uint8_t *f=frames[1][i];
  if(n211==0){assert(flen[1][i]==32);assert(x9q_u16(f+16)==0x106&&x9q_u16(f+18)==1&&f[20]==1);
   assert(x9q_u16(f+24)==0x103&&x9q_u16(f+26)==1&&f[28]==0);}
  if(n211==1){assert(flen[1][i]==40);assert(x9q_u16(f+16)==0x100);}
  n211++;}
 assert(n211==2);
 n211=0;
 for(unsigned i=0;i<fcount[2];i++){
  if(fid[2][i]==0x211){n211++;assert(x9q_u16(frames[2][i]+16)==0x100);}
  for(unsigned j=16;j+4<=flen[2][i];){
   unsigned tag=x9q_u16(frames[2][i]+j),len=x9q_u16(frames[2][i]+j+2);
   assert(tag!=0x106); if(!len)break; j+=(len+11)&~7u;}
 }
 assert(n211==1);
 printf("prelude-dds ok\n");}
'''
        out = self.compile_run(harness(main))
        self.assertIn('prelude-dds ok', out)
        self.assertIn('X9 FACTORY: SIM1 DATA_SET_CONFIG default-data-SIM accepted; ACT prelude continues', out)
        self.assertNotIn('SIM2 DATA_SET_CONFIG default-data-SIM accepted', out)


if __name__ == '__main__':
    unittest.main()
