#!/usr/bin/env python3
"""Exercise the factory-init replay (IMS config writer + feature bytes)."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'


class FactoryInit(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_factory_sequence_bytes_and_hold(self):
        source = SOURCE.read_text()
        b0 = source.index('static void x9m_factory_ims_bytes(')
        b1 = source.index('static bool x9m_diag_storage_busy(', b0)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
struct x9m_sim { unsigned ims_stage=0, fx_stage=0; uint64_t fx_token=0, fx_deadline=0; bool ims_hold=false, fx_hold=false, fx_pre=false, fx_cfg=false; bool fx_act_open=false, fx_barrier_noted=false; uint64_t fx_local_since=0; bool fx_local_noted=false; };
x9m_sim x9m_sims[2];
struct x9w_pcscf_list { uint8_t v4_count=0, v6_count=0; uint8_t v4[4][4]={}; uint8_t v6[4][16]={}; };
x9w_pcscf_list x9w_pcscf_lists[2];
void x9w_pcscf_publish(unsigned, const uint8_t*, unsigned){} bool x9w_control_ready=true; bool factory_on=true; uint64_t clock_ms=1, next_token=1;
bool x9m_factory_enabled(){return factory_on;}
bool cfg211_on=true; bool barrier_on=false;
bool x9m_factory_cfg211(){return cfg211_on;}
bool x9m_factory_act_barrier(){return barrier_on;}
unsigned x9_md_sim_count(){return 2;} bool x9_md_mipc_online(){return true;} uint64_t x9q_ms(){return clock_ms;}
struct Rec { unsigned id=0, mask=0, tag102[6]={}, has102=0, word100=99, byte100=99, tlv100len=0; unsigned word102=99, byte10a=99, byte101=99, byte103=99, tlv10dlen=0; uint8_t td_head[8]={}; unsigned tlv101len=0; uint8_t apn101[8]={}; };
Rec recs[64]; unsigned count=0; uint64_t tok_req[64]={}; unsigned tok_sim[64]={}; int reject_sim=-1; unsigned reject_id=0; int fail_sim=-1; unsigned fail_id=0; int fail_times=0;
struct x9w_frame { unsigned id, mask; Rec *r;
 x9w_frame(unsigned i, unsigned m):id(i),mask(m){r=&recs[count];r->id=i;r->mask=m;}
 void tlv(unsigned tag, const void *v, unsigned len){ if(tag==0x102&&len==6){r->has102=1;memcpy(r->tag102,v,6);} if(tag==0x100)r->tlv100len=len; if(tag==0x101){r->tlv101len=len;memcpy(r->apn101,v,len<8?len:8);} if(tag==0x10d){r->tlv10dlen=len;memcpy(r->td_head,v,8);} }
 void byte(unsigned tag, uint8_t v){ if(tag==0x100)r->byte100=v; if(tag==0x101)r->byte101=v; if(tag==0x103)r->byte103=v; if(tag==0x10a)r->byte10a=v; }
 void word(unsigned tag, uint32_t v){ if(tag==0x100)r->word100=v; if(tag==0x102)r->word102=v; }
 uint64_t submit(){ if(fail_times>0&&fail_sim==(int)((mask==1)?0:1)&&fail_id==id){fail_times--;count++;return 0;} count++; uint64_t t=next_token++; tok_req[t]=id; tok_sim[t]=(mask==1)?0:1; return t; } };
uint8_t answer[64]; unsigned answer_for=0;
int x9_md_mipc_take(uint64_t token, uint8_t *p, unsigned, unsigned *n){
 unsigned req=tok_req[token]; unsigned sim=tok_sim[token]; memset(answer,0,sizeof(answer));
 answer[8]=(uint8_t)(1u<<sim); unsigned cnf=req+1; answer[10]=(uint8_t)cnf; answer[11]=(uint8_t)(cnf>>8);
 answer[2]=(reject_sim==(int)sim&&reject_id==req)?1:0;
 memcpy(p,answer,32); *n=32; return 1; }
bool x9w_wire(const uint8_t*, unsigned){return true;}
unsigned x9q_u16(const uint8_t *p){return p[0]|(p[1]<<8);}
bool x9w_word(const uint8_t *p, unsigned, unsigned, unsigned &v){v=p[2];return true;}
struct x9q_view {const uint8_t *p; unsigned n;};
uint8_t cache[6]={3,3,0,0,1,1};
x9q_view x9w_tlv(const uint8_t*, unsigned, unsigned tag){return tag==0x100?x9q_view{cache,6}:x9q_view{nullptr,0};}
''' + source[b0:b1] + r'''
int main(){uint8_t p[64]={};
 uint8_t out[6]; x9m_factory_ims_bytes(out); assert(!memcmp(out,"\x03\x03\x00\x00\x01\x01",6));
 factory_on=false; x9m_factory_poll(p,sizeof(p)); assert(count==0);
 factory_on=true; x9m_sims[0].ims_stage=4; x9m_sims[1].ims_stage=4;
 for(int i=0;i<16;i++)x9m_factory_poll(p,sizeof(p));
 unsigned ids1[8], ids2[8]; unsigned n1=0, n2=0;
 for(unsigned i=0;i<count;i++)(recs[i].mask==1?ids1[n1++]:ids2[n2++])=recs[i].id;
 assert(n1==7&&ids1[0]==0xa1c&&ids1[1]==0xa18&&ids1[2]==0xa00&&ids1[3]==0x211&&ids1[4]==0x21b&&ids1[5]==0x201&&ids1[6]==0xa02);
 assert(n2==6&&ids2[0]==0xa18&&ids2[1]==0xa00&&ids2[2]==0x211&&ids2[3]==0x21b&&ids2[4]==0x201&&ids2[5]==0xa02);
 for(unsigned i=0;i<count;i++)if(recs[i].id==0xa00){assert(recs[i].has102&&!memcmp(recs[i].tag102,"\x03\x03\x00\x00\x01\x01",6));assert(recs[i].word100==0);}
 for(unsigned i=0;i<count;i++)if(recs[i].id==0xa1c)assert(recs[i].tlv100len==64);
assert(x9m_sims[0].fx_stage==6&&x9m_sims[1].fx_stage==6);
 for(unsigned i=0;i<count;i++)if(recs[i].id==0x21b){assert(recs[i].byte100==1);assert(recs[i].tlv101len==4);assert(!memcmp(recs[i].apn101,"ims",4));}
 for(unsigned i=0;i<count;i++)if(recs[i].id==0x211){assert(recs[i].byte100==1);assert(recs[i].byte101==1);assert(recs[i].byte103==0);}
 for(unsigned i=0;i<count;i++)if(recs[i].id==0x201){assert(recs[i].word102==2);assert(recs[i].byte10a==3);assert(recs[i].tlv10dlen==672);assert(recs[i].td_head[1]==3&&!memcmp(recs[i].td_head+4,"ims",3));}
 /* Rejection of the config write holds that SIM without retry. */
 count=0; memset(recs,0,sizeof(recs)); memset(tok_req,0,sizeof(tok_req));
 x9m_sims[0]=x9m_sim{}; x9m_sims[1]=x9m_sim{}; x9m_sims[0].ims_stage=4; x9m_sims[1].ims_stage=4;
 reject_sim=1; reject_id=0xa00;
 for(int i=0;i<16;i++)x9m_factory_poll(p,sizeof(p));
 assert(x9m_sims[1].fx_hold&&x9m_sims[1].fx_stage==3);
 assert(x9m_sims[0].fx_stage==6);
 printf("factory-init ok\n");}
'''
        out = self.compile_run(code)
        self.assertIn('factory-init ok', out)
        self.assertIn('X9 FACTORY: SIM1 IMS config accepted by modem', out)
        self.assertIn('X9 FACTORY: SIM1 DATA_RETRY_TIMER ims accepted; ACT follows', out)
        self.assertIn('X9 FACTORY: SIM2 DATA_RETRY_TIMER ims accepted; ACT follows', out)
        self.assertIn('X9 FACTORY: SIM1 DATA_SET_CONFIG accepted; RETRY_TIMER follows', out)
        self.assertIn('X9 FACTORY: SIM2 DATA_SET_CONFIG accepted; RETRY_TIMER follows', out)
        self.assertIn('feature cache after write raw=030300000101', out)


    def test_local_rejection_retry_and_budget(self):
        source = SOURCE.read_text()
        b0 = source.index('static void x9m_factory_ims_bytes(')
        b1 = source.index('static bool x9m_diag_storage_busy(', b0)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
struct x9m_sim { unsigned ims_stage=0, fx_stage=0; uint64_t fx_token=0, fx_deadline=0; bool ims_hold=false, fx_hold=false, fx_pre=false, fx_cfg=false; bool fx_act_open=false, fx_barrier_noted=false; uint64_t fx_local_since=0; bool fx_local_noted=false; };
x9m_sim x9m_sims[2];
struct x9w_pcscf_list { uint8_t v4_count=0, v6_count=0; uint8_t v4[4][4]={}; uint8_t v6[4][16]={}; };
x9w_pcscf_list x9w_pcscf_lists[2];
void x9w_pcscf_publish(unsigned, const uint8_t*, unsigned){} bool x9w_control_ready=true; bool factory_on=true; uint64_t clock_ms=1, next_token=1;
bool x9m_factory_enabled(){return factory_on;}
bool cfg211_on=true; bool barrier_on=false;
bool x9m_factory_cfg211(){return cfg211_on;}
bool x9m_factory_act_barrier(){return barrier_on;}
unsigned x9_md_sim_count(){return 2;} bool x9_md_mipc_online(){return true;} uint64_t x9q_ms(){return clock_ms;}
struct Rec { unsigned id=0, mask=0, tag102[6]={}, has102=0, word100=99, byte100=99, tlv100len=0; unsigned word102=99, byte10a=99, byte101=99, byte103=99, tlv10dlen=0; uint8_t td_head[8]={}; unsigned tlv101len=0; uint8_t apn101[8]={}; };
Rec recs[64]; unsigned count=0; uint64_t tok_req[64]={}; unsigned tok_sim[64]={}; int reject_sim=-1; unsigned reject_id=0; int fail_sim=-1; unsigned fail_id=0; int fail_times=0;
struct x9w_frame { unsigned id, mask; Rec *r;
 x9w_frame(unsigned i, unsigned m):id(i),mask(m){r=&recs[count];r->id=i;r->mask=m;}
 void tlv(unsigned tag, const void *v, unsigned len){ if(tag==0x102&&len==6){r->has102=1;memcpy(r->tag102,v,6);} if(tag==0x100)r->tlv100len=len; if(tag==0x101){r->tlv101len=len;memcpy(r->apn101,v,len<8?len:8);} if(tag==0x10d){r->tlv10dlen=len;memcpy(r->td_head,v,8);} }
 void byte(unsigned tag, uint8_t v){ if(tag==0x100)r->byte100=v; if(tag==0x101)r->byte101=v; if(tag==0x103)r->byte103=v; if(tag==0x10a)r->byte10a=v; }
 void word(unsigned tag, uint32_t v){ if(tag==0x100)r->word100=v; if(tag==0x102)r->word102=v; }
 uint64_t submit(){ if(fail_times>0&&fail_sim==(int)((mask==1)?0:1)&&fail_id==id){fail_times--;count++;return 0;} count++; uint64_t t=next_token++; tok_req[t]=id; tok_sim[t]=(mask==1)?0:1; return t; } };
uint8_t answer[64]; unsigned answer_for=0;
int x9_md_mipc_take(uint64_t token, uint8_t *p, unsigned, unsigned *n){
 unsigned req=tok_req[token]; unsigned sim=tok_sim[token]; memset(answer,0,sizeof(answer));
 answer[8]=(uint8_t)(1u<<sim); unsigned cnf=req+1; answer[10]=(uint8_t)cnf; answer[11]=(uint8_t)(cnf>>8);
 answer[2]=(reject_sim==(int)sim&&reject_id==req)?1:0;
 memcpy(p,answer,32); *n=32; return 1; }
bool x9w_wire(const uint8_t*, unsigned){return true;}
unsigned x9q_u16(const uint8_t *p){return p[0]|(p[1]<<8);}
bool x9w_word(const uint8_t *p, unsigned, unsigned, unsigned &v){v=p[2];return true;}
struct x9q_view {const uint8_t *p; unsigned n;};
uint8_t cache[6]={3,3,0,0,1,1};
x9q_view x9w_tlv(const uint8_t*, unsigned, unsigned tag){return tag==0x100?x9q_view{cache,6}:x9q_view{nullptr,0};}
''' + source[b0:b1] + r'''
int main(){uint8_t p[64]={};
 x9m_sims[0].ims_stage=4; x9m_sims[1].ims_stage=4;
 /* SIM2 stage 2 (0xa18) submit fails twice locally, then succeeds:
  * the stage must be kept and retried, never held. */
 fail_sim=1; fail_id=0xa18; fail_times=2;
 for(int i=0;i<24;i++)x9m_factory_poll(p,sizeof(p));
 assert(!x9m_sims[1].fx_hold&&x9m_sims[1].fx_stage==6);
 assert(x9m_sims[0].fx_stage==6);
 unsigned att=0; for(unsigned i=0;i<count;i++)if(recs[i].id==0xa18&&recs[i].mask==2)att++;
 assert(att==3);
 printf("retry-then-succeed ok\n");
 /* Budget: SIM1 stage 1 submit never succeeds; hold only once the
  * 30 s local-retry budget is exhausted, stage unchanged. */
 count=0; memset(recs,0,sizeof(recs)); memset(tok_req,0,sizeof(tok_req)); memset(tok_sim,0,sizeof(tok_sim));
 x9m_sims[0]=x9m_sim{}; x9m_sims[1]=x9m_sim{}; x9m_sims[0].ims_stage=4; x9m_sims[1].ims_stage=4;
 fail_sim=0; fail_id=0xa1c; fail_times=1000;
 clock_ms=1000;
 for(int i=0;i<12;i++)x9m_factory_poll(p,sizeof(p));
 assert(x9m_sims[1].fx_stage==6);
 assert(!x9m_sims[0].fx_hold&&x9m_sims[0].fx_stage==1);
 count=0; memset(recs,0,sizeof(recs));
 clock_ms=1000+10000; x9m_factory_poll(p,sizeof(p));
 assert(!x9m_sims[0].fx_hold&&x9m_sims[0].fx_stage==1);
 clock_ms=1000+31000; x9m_factory_poll(p,sizeof(p));
 assert(x9m_sims[0].fx_hold&&x9m_sims[0].fx_stage==1);
 printf("budget ok\n");}
'''
        out = self.compile_run(code)
        self.assertIn('retry-then-succeed ok', out)
        self.assertIn('budget ok', out)
        self.assertEqual(out.count('SIM2 stage 2 submit rejected locally; retry continues'), 1)
        self.assertEqual(out.count('SIM1 stage 1 submit rejected locally; retry continues'), 1)
        self.assertIn('X9 FACTORY: SIM1 stage 1 submit retry budget exhausted; held', out)

    def test_retry_timer_rejection_holds(self):
        source = SOURCE.read_text()
        b0 = source.index('static void x9m_factory_ims_bytes(')
        b1 = source.index('static bool x9m_diag_storage_busy(', b0)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
struct x9m_sim { unsigned ims_stage=0, fx_stage=0; uint64_t fx_token=0, fx_deadline=0; bool ims_hold=false, fx_hold=false, fx_pre=false, fx_cfg=false; bool fx_act_open=false, fx_barrier_noted=false; uint64_t fx_local_since=0; bool fx_local_noted=false; };
x9m_sim x9m_sims[2];
struct x9w_pcscf_list { uint8_t v4_count=0, v6_count=0; uint8_t v4[4][4]={}; uint8_t v6[4][16]={}; };
x9w_pcscf_list x9w_pcscf_lists[2];
void x9w_pcscf_publish(unsigned, const uint8_t*, unsigned){} bool x9w_control_ready=true; bool factory_on=true; uint64_t clock_ms=1, next_token=1;
bool x9m_factory_enabled(){return factory_on;}
bool cfg211_on=true; bool barrier_on=false;
bool x9m_factory_cfg211(){return cfg211_on;}
bool x9m_factory_act_barrier(){return barrier_on;}
unsigned x9_md_sim_count(){return 2;} bool x9_md_mipc_online(){return true;} uint64_t x9q_ms(){return clock_ms;}
struct Rec { unsigned id=0, mask=0, tag102[6]={}, has102=0, word100=99, byte100=99, tlv100len=0; unsigned word102=99, byte10a=99, byte101=99, byte103=99, tlv10dlen=0; uint8_t td_head[8]={}; unsigned tlv101len=0; uint8_t apn101[8]={}; };
Rec recs[64]; unsigned count=0; uint64_t tok_req[64]={}; unsigned tok_sim[64]={}; int reject_sim=-1; unsigned reject_id=0; int fail_sim=-1; unsigned fail_id=0; int fail_times=0;
struct x9w_frame { unsigned id, mask; Rec *r;
 x9w_frame(unsigned i, unsigned m):id(i),mask(m){r=&recs[count];r->id=i;r->mask=m;}
 void tlv(unsigned tag, const void *v, unsigned len){ if(tag==0x102&&len==6){r->has102=1;memcpy(r->tag102,v,6);} if(tag==0x100)r->tlv100len=len; if(tag==0x101){r->tlv101len=len;memcpy(r->apn101,v,len<8?len:8);} if(tag==0x10d){r->tlv10dlen=len;memcpy(r->td_head,v,8);} }
 void byte(unsigned tag, uint8_t v){ if(tag==0x100)r->byte100=v; if(tag==0x101)r->byte101=v; if(tag==0x103)r->byte103=v; if(tag==0x10a)r->byte10a=v; }
 void word(unsigned tag, uint32_t v){ if(tag==0x100)r->word100=v; if(tag==0x102)r->word102=v; }
 uint64_t submit(){ if(fail_times>0&&fail_sim==(int)((mask==1)?0:1)&&fail_id==id){fail_times--;count++;return 0;} count++; uint64_t t=next_token++; tok_req[t]=id; tok_sim[t]=(mask==1)?0:1; return t; } };
uint8_t answer[64]; unsigned answer_for=0;
int x9_md_mipc_take(uint64_t token, uint8_t *p, unsigned, unsigned *n){
 unsigned req=tok_req[token]; unsigned sim=tok_sim[token]; memset(answer,0,sizeof(answer));
 answer[8]=(uint8_t)(1u<<sim); unsigned cnf=req+1; answer[10]=(uint8_t)cnf; answer[11]=(uint8_t)(cnf>>8);
 answer[2]=(reject_sim==(int)sim&&reject_id==req)?1:0;
 memcpy(p,answer,32); *n=32; return 1; }
bool x9w_wire(const uint8_t*, unsigned){return true;}
unsigned x9q_u16(const uint8_t *p){return p[0]|(p[1]<<8);}
bool x9w_word(const uint8_t *p, unsigned, unsigned, unsigned &v){v=p[2];return true;}
struct x9q_view {const uint8_t *p; unsigned n;};
uint8_t cache[6]={3,3,0,0,1,1};
x9q_view x9w_tlv(const uint8_t*, unsigned, unsigned tag){return tag==0x100?x9q_view{cache,6}:x9q_view{nullptr,0};}
''' + source[b0:b1] + r'''
int main(){uint8_t p[64]={};
 x9m_sims[0].ims_stage=4; x9m_sims[1].ims_stage=4;
 /* A modem CNF rejection of the 021b prelude holds stage 4 as before;
  * the ACT must never be sent for that SIM. */
 reject_sim=0; reject_id=0x21b;
 for(int i=0;i<16;i++)x9m_factory_poll(p,sizeof(p));
 assert(x9m_sims[0].fx_hold&&x9m_sims[0].fx_stage==4);
 for(unsigned i=0;i<count;i++)assert(!(recs[i].id==0x201&&recs[i].mask==1));
 assert(x9m_sims[1].fx_stage==6);
 printf("retry-timer-reject ok\n");}
'''
        out = self.compile_run(code)
        self.assertIn('retry-timer-reject ok', out)
        self.assertIn('X9 FACTORY: SIM1 stage 4 rejected known 1 result 1; held', out)

    def test_cfg211_and_act_barrier(self):
        source = SOURCE.read_text()
        b0 = source.index('static void x9m_factory_ims_bytes(')
        b1 = source.index('static bool x9m_diag_storage_busy(', b0)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
struct x9m_sim { unsigned ims_stage=0, fx_stage=0; uint64_t fx_token=0, fx_deadline=0; bool ims_hold=false, fx_hold=false, fx_pre=false, fx_cfg=false; bool fx_act_open=false, fx_barrier_noted=false; uint64_t fx_local_since=0; bool fx_local_noted=false; };
x9m_sim x9m_sims[2];
struct x9w_pcscf_list { uint8_t v4_count=0, v6_count=0; uint8_t v4[4][4]={}; uint8_t v6[4][16]={}; };
x9w_pcscf_list x9w_pcscf_lists[2];
void x9w_pcscf_publish(unsigned, const uint8_t*, unsigned){} bool x9w_control_ready=true; bool factory_on=true; uint64_t clock_ms=1, next_token=1;
bool x9m_factory_enabled(){return factory_on;}
bool cfg211_on=true; bool barrier_on=false;
bool x9m_factory_cfg211(){return cfg211_on;}
bool x9m_factory_act_barrier(){return barrier_on;}
unsigned x9_md_sim_count(){return 2;} bool x9_md_mipc_online(){return true;} uint64_t x9q_ms(){return clock_ms;}
struct Rec { unsigned id=0, mask=0, tag102[6]={}, has102=0, word100=99, byte100=99, tlv100len=0; unsigned word102=99, byte10a=99, byte101=99, byte103=99, tlv10dlen=0; uint8_t td_head[8]={}; unsigned tlv101len=0; uint8_t apn101[8]={}; };
Rec recs[64]; unsigned count=0; uint64_t tok_req[256]={}; unsigned tok_sim[256]={}; int reject_sim=-1; unsigned reject_id=0; int fail_sim=-1; unsigned fail_id=0; int fail_times=0;
int withhold_sim=-1; unsigned withhold_id=0; int withhold_left=0;
struct x9w_frame { unsigned id, mask; Rec *r;
 x9w_frame(unsigned i, unsigned m):id(i),mask(m){r=&recs[count];r->id=i;r->mask=m;}
 void tlv(unsigned tag, const void *v, unsigned len){ if(tag==0x102&&len==6){r->has102=1;memcpy(r->tag102,v,6);} if(tag==0x100)r->tlv100len=len; if(tag==0x101){r->tlv101len=len;memcpy(r->apn101,v,len<8?len:8);} if(tag==0x10d){r->tlv10dlen=len;memcpy(r->td_head,v,8);} }
 void byte(unsigned tag, uint8_t v){ if(tag==0x100)r->byte100=v; if(tag==0x101)r->byte101=v; if(tag==0x103)r->byte103=v; if(tag==0x10a)r->byte10a=v; }
 void word(unsigned tag, uint32_t v){ if(tag==0x100)r->word100=v; if(tag==0x102)r->word102=v; }
 uint64_t submit(){ if(fail_times>0&&fail_sim==(int)((mask==1)?0:1)&&fail_id==id){fail_times--;count++;return 0;} count++; uint64_t t=next_token++; tok_req[t]=id; tok_sim[t]=(mask==1)?0:1; return t; } };
uint8_t answer[64]; unsigned answer_for=0;
int x9_md_mipc_take(uint64_t token, uint8_t *p, unsigned, unsigned *n){
 unsigned req=tok_req[token]; unsigned sim=tok_sim[token];
 if(withhold_left>0&&withhold_sim==(int)sim&&withhold_id==req){withhold_left--;return 0;}
 memset(answer,0,sizeof(answer));
 answer[8]=(uint8_t)(1u<<sim); unsigned cnf=req+1; answer[10]=(uint8_t)cnf; answer[11]=(uint8_t)(cnf>>8);
 answer[2]=(reject_sim==(int)sim&&reject_id==req)?1:0;
 memcpy(p,answer,32); *n=32; return 1; }
bool x9w_wire(const uint8_t*, unsigned){return true;}
unsigned x9q_u16(const uint8_t *p){return p[0]|(p[1]<<8);}
bool x9w_word(const uint8_t *p, unsigned, unsigned, unsigned &v){v=p[2];return true;}
struct x9q_view {const uint8_t *p; unsigned n;};
uint8_t cache[6]={3,3,0,0,1,1};
x9q_view x9w_tlv(const uint8_t*, unsigned, unsigned tag){return tag==0x100?x9q_view{cache,6}:x9q_view{nullptr,0};}
''' + source[b0:b1] + r'''
static void reset_all(){count=0; memset(recs,0,sizeof(recs)); memset(tok_req,0,sizeof(tok_req)); memset(tok_sim,0,sizeof(tok_sim));
 x9m_sims[0]=x9m_sim{}; x9m_sims[1]=x9m_sim{}; x9m_sims[0].ims_stage=4; x9m_sims[1].ims_stage=4;
 reject_sim=-1; reject_id=0; fail_times=0; withhold_sim=-1; withhold_id=0; withhold_left=0;
 cfg211_on=true; barrier_on=false; clock_ms=1;}
static unsigned submitted(unsigned id, unsigned mask){unsigned k=0; for(unsigned i=0;i<count;i++)if(recs[i].id==id&&recs[i].mask==mask)k++; return k;}
int main(){uint8_t p[64]={};
 /* A: a modem rejection of the 0211 holds stage 4 before the prelude;
  * neither the 021b nor the ACT is ever sent for that SIM. */
 reset_all(); reject_sim=0; reject_id=0x211;
 for(int i=0;i<16;i++)x9m_factory_poll(p,sizeof(p));
 assert(x9m_sims[0].fx_hold&&x9m_sims[0].fx_stage==4&&!x9m_sims[0].fx_cfg);
 assert(submitted(0x21b,1)==0&&submitted(0x201,1)==0);
 assert(x9m_sims[1].fx_stage==6);
 printf("cfg211-reject ok\n");
 /* B: x9_factory_cfg211=0 restores the pre-0211 sequence exactly. */
 reset_all(); cfg211_on=false;
 for(int i=0;i<16;i++)x9m_factory_poll(p,sizeof(p));
 assert(x9m_sims[0].fx_stage==6&&x9m_sims[1].fx_stage==6);
 assert(submitted(0x211,1)==0&&submitted(0x211,2)==0);
 printf("cfg211-optout ok\n");
 /* C: with the barrier on, SIM2's open ACT blocks SIM1's ACT submit;
  * waiting consumes no submit and no local-retry budget. SIM2 reaches
  * its ACT first (its sequence is one step shorter). */
 reset_all(); barrier_on=true; withhold_sim=1; withhold_id=0x201; withhold_left=8;
 for(int i=0;i<16;i++)x9m_factory_poll(p,sizeof(p));
 assert(submitted(0x201,2)==1&&submitted(0x201,1)==0);
 assert(x9m_sims[1].fx_act_open);
 assert(x9m_sims[0].fx_stage==4&&x9m_sims[0].fx_cfg&&x9m_sims[0].fx_pre&&!x9m_sims[0].fx_act_open);
 assert(x9m_sims[0].fx_local_since==0&&!x9m_sims[0].fx_hold);
 printf("barrier-holds ok\n");
 for(int i=0;i<12;i++)x9m_factory_poll(p,sizeof(p));
 assert(x9m_sims[0].fx_stage==6&&x9m_sims[1].fx_stage==6);
 assert(submitted(0x201,1)==1&&submitted(0x201,2)==1);
 printf("barrier-releases ok\n");
 /* D: a peer whose ACT was rejected is held and must not block: the
  * other SIM still submits its ACT and completes. */
 reset_all(); barrier_on=true; reject_sim=1; reject_id=0x201;
 for(int i=0;i<24;i++)x9m_factory_poll(p,sizeof(p));
 assert(x9m_sims[1].fx_hold&&x9m_sims[1].fx_stage==4);
 assert(x9m_sims[0].fx_stage==6&&submitted(0x201,1)==1);
 printf("barrier-peer-held ok\n");
 /* E: with the barrier off (the default), the second ACT is submitted
  * while the first is still open -- the A/B contrast. */
 reset_all(); withhold_sim=1; withhold_id=0x201; withhold_left=8;
 for(int i=0;i<16;i++)x9m_factory_poll(p,sizeof(p));
 assert(submitted(0x201,1)==1&&submitted(0x201,2)==1);
 assert(x9m_sims[1].fx_act_open&&x9m_sims[0].fx_stage>=5);
 printf("barrier-off-overlap ok\n");}
'''
        out = self.compile_run(code)
        self.assertIn('cfg211-reject ok', out)
        self.assertIn('cfg211-optout ok', out)
        self.assertIn('barrier-holds ok', out)
        self.assertIn('barrier-releases ok', out)
        self.assertIn('barrier-peer-held ok', out)
        self.assertIn('barrier-off-overlap ok', out)
        self.assertEqual(out.count('X9 FACTORY: SIM1 ACT barrier: SIM2 ACT still open; waiting'), 1)
        self.assertIn('X9 FACTORY: SIM1 stage 4 rejected known 1 result 1; held', out)
        self.assertIn('X9 FACTORY: SIM2 DATA_SET_CONFIG accepted; RETRY_TIMER follows', out)



if __name__ == '__main__':
    unittest.main()
