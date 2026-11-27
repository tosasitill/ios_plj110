#!/usr/bin/env python3
"""Exercise the no-ack SMS indication path (4606 family) at frame level."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'


class SmsInd(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_event_routing_and_registration(self):
        source = SOURCE.read_text()
        e0 = source.index('static void x9m_event(')
        e1 = source.index('static bool x9m_request(', e0)
        p0 = source.index('static void x9m_ind_poll(')
        p1 = source.index('static int x9m_publish(', p0)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
struct x9q_view {const uint8_t *p; unsigned n;};
struct x9m_incoming { uint32_t token=0; unsigned sim=0,len=0,slot=0; uint64_t generation=0,next_delivery=0;
 bool delivered=false,replying=false,held=false,ind=false; uint64_t ack_wait=0; uint8_t pdu[255]={}; };
x9m_incoming x9m_incoming_sms[16];
struct x9m_sim { unsigned reg_epoch=0, init=0, ind_stage=0; uint64_t ind_token=0, ind_deadline=0; bool ind_hold=false; };
x9m_sim x9m_sims[2];
enum { X9M_CFG_IND=0, X9M_READY_QUERY, X9M_WAIT_READY, X9M_HOST_READY, X9M_FACTORY_CFG, X9M_INIT_DONE };
bool ind_on=false, control_ready=true; uint64_t clock_ms=1;
bool x9m_ind_enabled(){return ind_on;}
bool x9m_diag_enabled(){return false;}
unsigned x9_md_sim_count(){return 2;} bool x9_md_mipc_online(){return true;} uint64_t x9q_ms(){return clock_ms;}
bool x9w_control_ready=true;
unsigned x9q_u16(const uint8_t *p){return p[0]|(p[1]<<8);}
bool x9w_wire(const uint8_t*, unsigned){return true;}
static bool walk(const uint8_t *p, unsigned n, unsigned tag, const uint8_t **out, unsigned *len){
 unsigned at=16; while(at+4<=n){unsigned t=x9q_u16(p+at)&0x7fff, k=x9q_u16(p+at+2);
  if(t==(tag&0x7fff)){*out=p+at+4; *len=k; return true;} at+=k?((k+11)&~7u):16;} return false;}
x9q_view x9w_tlv(const uint8_t *p, unsigned n, unsigned tag){const uint8_t *v=nullptr; unsigned k=0;
 return walk(p,n,tag,&v,&k)?x9q_view{v,k}:x9q_view{nullptr,0};}
bool x9w_byte(const uint8_t *p, unsigned n, unsigned tag, unsigned &v){const uint8_t *o=nullptr; unsigned k=0;
 if(!walk(p,n,tag,&o,&k)||k!=1)return false; v=o[0]; return true;}
bool x9w_word(const uint8_t *p, unsigned n, unsigned tag, unsigned &v){const uint8_t *o=nullptr; unsigned k=0;
 if(!walk(p,n,tag,&o,&k)||k!=4)return false; v=o[0]|(o[1]<<8)|(o[2]<<16)|((unsigned)o[3]<<24); return true;}
void x9m_md_ready(unsigned, bool){}
void x9m_reg(unsigned, unsigned){}
bool x9m_pdu(const uint8_t*, unsigned n, unsigned){return n>=3;}
struct Deliv { bool ind=false; uint32_t token=0; unsigned sim=0, mti=0, len=0; uint8_t pdu[255]={}; };
Deliv delivs[8]; unsigned ndeliv=0;
void x9m_deliver(x9m_incoming &in, unsigned mti){Deliv &d=delivs[ndeliv++]; d.ind=in.ind; d.token=in.token;
 d.sim=in.sim; d.mti=mti; d.len=in.len; memcpy(d.pdu,in.pdu,in.len);}
/* registration poll stubs */
struct FRec { unsigned id=0, mask=0, reg=0; }; FRec frecs[16]; unsigned nfrec=0; uint64_t next_tok=1; uint64_t tok_reg[64]={}; unsigned tok_sim[64]={};
struct x9w_frame { unsigned id, mask;
 x9w_frame(unsigned i, unsigned m):id(i),mask(m){}
 void tlv(unsigned tag, const void *v, unsigned len){if(tag==0x100&&len==2){const uint8_t *b=(const uint8_t*)v;
  frecs[nfrec].reg=b[0]|(b[1]<<8);}}
 uint64_t submit(){frecs[nfrec].id=id; frecs[nfrec].mask=mask; nfrec++; uint64_t t=next_tok++; tok_reg[t]=frecs[nfrec-1].reg; tok_sim[t]=(mask==2)?1:0; return t;} };
int reject_reg=0; uint8_t ans[64];
int x9_md_mipc_take(uint64_t token, uint8_t *p, unsigned, unsigned *n){memset(ans,0,sizeof(ans));
 unsigned reg=tok_reg[token]; ans[8]=(uint8_t)(1u<<tok_sim[token]); ans[10]=0x08; ans[11]=0x03;
 /* result TLV tag0 len4 at offset16 */ ans[16]=0; ans[17]=0; ans[18]=4; ans[19]=0;
 ans[20]=(reject_reg==(int)reg&&tok_sim[token]==1)?1:0; memcpy(p,ans,32); *n=32; return 1;}
''' + source[e0:e1] + source[p0:p1] + r'''
static unsigned build(uint8_t *f, unsigned mask, unsigned id){
 memset(f,0,256); f[0]=0x84; f[1]=0x19; f[2]=0x54; f[3]=0x24; f[8]=(uint8_t)mask;
 f[10]=(uint8_t)id; f[11]=(uint8_t)(id>>8); return 16;}
static void addtlv(uint8_t *f, unsigned &at, unsigned tag, const uint8_t *v, unsigned len){
 f[at]=(uint8_t)tag; f[at+1]=(uint8_t)(tag>>8); f[at+2]=(uint8_t)len; f[at+3]=(uint8_t)(len>>8);
 memcpy(f+at+4,v,len); at+=((len+11)&~7u);}
static void finish(uint8_t *f, unsigned at){f[14]=(uint8_t)(at-16); f[15]=(uint8_t)((at-16)>>8);}
int main(){uint8_t f[256]; unsigned at;
 uint8_t fmt=3; uint8_t list[13]={}; list[2]=0; list[8]=4; list[9]=0x11; list[10]=0x22; list[11]=0x33; list[12]=0x44;
 uint8_t sr[6]={1,2,3,4,5,6};
 /* gate off: 4602 with no token is dropped */
 ind_on=false; at=build(f,1,0x4602); addtlv(f,at,0x100,&fmt,1); addtlv(f,at,0x8104,list,13); finish(f,at);
 x9m_event(f,at,0); assert(ndeliv==0);
 /* gate on: 4602 delivers like 8601, with synthetic ind token */
 ind_on=true; x9m_event(f,at,0); assert(ndeliv==1); assert(delivs[0].ind); assert(delivs[0].token&0x80000000u);
 assert(delivs[0].mti==0&&delivs[0].sim==0&&delivs[0].len==5); assert(delivs[0].pdu[0]==4&&delivs[0].pdu[4]==0x44);
 /* gate on: 4606 status report on SIM2 */
 at=build(f,2,0x4606); addtlv(f,at,0x8100,sr,6); finish(f,at);
 x9m_event(f,at,0); assert(ndeliv==2); assert(delivs[1].ind); assert(delivs[1].token&0x1000000u);
 assert(delivs[1].mti==2&&delivs[1].sim==1&&delivs[1].len==6);
 /* gate on: 4603 is evidence only, never delivered */
 uint8_t flag=1; uint8_t ix[2]={7,0}; at=build(f,1,0x4603); addtlv(f,at,0x100,&flag,1); addtlv(f,at,0x101,ix,2); finish(f,at);
 x9m_event(f,at,0); assert(ndeliv==2);
 /* CMD path unchanged: real token preserved, ind clear */
 at=build(f,1,0x8601); addtlv(f,at,0x100,&fmt,1); addtlv(f,at,0x8104,list,13); finish(f,at);
 x9m_event(f,at,4242); assert(ndeliv==3); assert(!delivs[2].ind&&delivs[2].token==4242);
 at=build(f,2,0x8603); addtlv(f,at,0x8100,sr,6); finish(f,at);
 x9m_event(f,at,4243); assert(ndeliv==4); assert(!delivs[3].ind&&delivs[3].token==4243&&delivs[3].mti==2);
 /* malformed 4602 (short PDU_LIST) dropped */
 uint8_t short_list[8]={}; at=build(f,1,0x4602); addtlv(f,at,0x8104,short_list,8); finish(f,at);
 x9m_event(f,at,0); assert(ndeliv==4);
 /* registration: per-SIM 4602 then 4603 then 4606 via 0x307 */
 x9m_sims[0].init=1; x9m_sims[1].init=1;
 for(int i=0;i<10;i++){uint8_t p[64]; x9m_ind_poll(p,sizeof(p));}
 assert(nfrec==6);
 assert(frecs[0].reg==0x4602&&frecs[1].reg==0x4602&&frecs[2].reg==0x4603&&frecs[3].reg==0x4603&&frecs[4].reg==0x4606&&frecs[5].reg==0x4606);
 assert(x9m_sims[0].ind_stage==4&&x9m_sims[1].ind_stage==4);
 /* rejection of the 4606 registration holds SIM2 at stage 3 without retry */
 nfrec=0; memset(frecs,0,sizeof(frecs)); x9m_sims[0]=x9m_sim{}; x9m_sims[1]=x9m_sim{};
 x9m_sims[0].init=1; x9m_sims[1].init=1; reject_reg=0x4606;
 for(int i=0;i<10;i++){uint8_t p[64]; x9m_ind_poll(p,sizeof(p));}
 assert(x9m_sims[1].ind_hold&&x9m_sims[1].ind_stage==3); assert(x9m_sims[0].ind_stage==4);
 /* rejection of the new 4603 registration holds SIM2 at stage 2 */
 nfrec=0; memset(frecs,0,sizeof(frecs)); x9m_sims[0]=x9m_sim{}; x9m_sims[1]=x9m_sim{};
 x9m_sims[0].init=1; x9m_sims[1].init=1; reject_reg=0x4603;
 for(int i=0;i<10;i++){uint8_t p[64]; x9m_ind_poll(p,sizeof(p));}
 assert(x9m_sims[1].ind_hold&&x9m_sims[1].ind_stage==2); assert(x9m_sims[0].ind_stage==4);
 printf("sms-ind ok\n");}
'''
        out = self.compile_run(code)
        self.assertIn('sms-ind ok', out)
        self.assertIn('store status flag 1 index 7', out)
        self.assertIn('no-ack indications registered (4602/4603/4606)', out)


    def test_defaults_on_source(self):
        """x9_sms_ind / x9_sms_srr are default-on since the 2026-11-28
        defaults pass; =0 is the only opt-out. Source-asserted because
        the behaviour tests stub the enabled accessors."""
        messages = SOURCE.read_text()
        bridge = (ROOT / 'x9/kernel/src/bsd/dev/x9_mipc_messages.inc').read_text()
        self.assertIn('sweet_x9_arg("x9_sms_ind",1)!=0', messages)
        self.assertIn('sweet_x9_arg("x9_sms_srr",1)!=0', messages)
        self.assertNotIn('sweet_x9_arg("x9_sms_ind",0)', messages)
        self.assertNotIn('sweet_x9_arg("x9_sms_srr",0)', messages)
        self.assertIn('int v=1;enabled=PE_parse_boot_argn("x9_sms_ind",&v,sizeof(v))?v!=0:1', bridge)

if __name__ == '__main__':
    unittest.main()

