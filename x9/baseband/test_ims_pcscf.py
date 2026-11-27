#!/usr/bin/env python3
"""Exercise the P-CSCF decode/publish/serialize chain for WDS 0x2d IMS settings."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
RESULT = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9WDSResult.inc'
WDS = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9WDS.inc'


class ImsPcscf(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_decode_publish_serialize_and_gate(self):
        result = RESULT.read_text()
        start = result.index('static inline bool x9w_pcscf_decode(')
        end = result.index('static inline bool x9wr_serialize_pcscf(', start)
        end = result.index('return p.good;', end) + len('return p.good;')
        end = result.index('}', end) + 1
        wds = WDS.read_text()
        tlv_start = wds.index('static x9q_view x9w_tlv(')
        tlv_end = wds.index('static bool x9w_byte(', tlv_start)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
struct x9q_view {const uint8_t *p; unsigned n;};
unsigned x9q_u16(const uint8_t *p){return p[0]|(p[1]<<8);}
unsigned x9q_u32(const uint8_t *p){return p[0]|(p[1]<<8)|(p[2]<<16)|((unsigned)p[3]<<24);}
struct x9w_pcscf_list { uint8_t v4_count=0, v6_count=0; uint8_t v4[4][4]={}; uint8_t v6[4][16]={}; };
x9w_pcscf_list x9w_pcscf_lists[2];
bool x9wr_address(const uint8_t *p, unsigned bytes){
 uint8_t any=0; for(unsigned i=0;i<bytes;i++)any|=p[i]; if(!any)return false;
 return bytes==4?(p[0]!=0&&p[0]!=127&&p[0]<224):p[0]!=0xff; }
bool x9wr_optional_byte(const uint8_t *p, unsigned n, unsigned tag, unsigned &value);
struct x9q_packet { bool good=true; unsigned tags[8]={}; uint8_t vals[8][128]={}; unsigned lens[8]={}; unsigned count=0;
 void tlv(unsigned tag, const void *v, unsigned len){ tags[count]=tag; lens[count]=len; memcpy(vals[count],v,len); count++; } };
''' + wds[tlv_start:tlv_end] + r'''
bool x9wr_optional_byte(const uint8_t *p, unsigned n, unsigned tag, unsigned &value){
 x9q_view t=x9w_tlv(p,n,tag); value=0; if(!t.p)return true; if(t.n!=1)return false; value=t.p[0]; return true; }
''' + result[start:end] + r'''
struct Builder { uint8_t p[1024]; unsigned n=16;
 Builder(unsigned mask){ memset(p,0,sizeof(p)); p[0]=0x84;p[1]=0x19;p[2]=0x54;p[3]=0x24; p[8]=(uint8_t)mask; p[10]=0x02; p[11]=0x02; }
 void tlv(unsigned tag, const void *v, unsigned len){ unsigned size=(len+11)&~7u; p[n]=(uint8_t)tag; p[n+1]=(uint8_t)(tag>>8); p[n+2]=(uint8_t)len; p[n+3]=(uint8_t)(len>>8); memcpy(p+n+4,v,len); n+=size; }
 void byte(unsigned tag, uint8_t v){ tlv(tag,&v,1); }
 void finish(){ p[14]=(uint8_t)(n-16); p[15]=(uint8_t)((n-16)>>8); } };
int main(){
 /* Factory-shaped IMS CNF: v6 P-CSCF x2, no v4 list. */
 Builder b(1); uint8_t result0[4]={}; b.tlv(0,result0,4); b.byte(0x100,1);
 uint8_t a1[16]={0x20,0x01,0,0,0,0,0,0,0,0,0,0,0,0,0,1}, a2[16]={0x20,0x01,0,0,0,0,0,0,0,0,0,0,0,0,0,2};
 b.byte(0x11c,2); b.tlv(0x811d,a1,16); b.tlv(0x811e,a2,16); b.finish();
 x9w_pcscf_list out; assert(x9w_pcscf_decode(b.p,b.n,out));
 assert(out.v4_count==0&&out.v6_count==2&&!memcmp(out.v6[0],a1,16)&&!memcmp(out.v6[1],a2,16));
 x9w_pcscf_publish(1,b.p,b.n);
 assert(x9w_pcscf_lists[1].v6_count==2&&x9w_pcscf_lists[0].v6_count==0);
 /* Gate: IMS APN + stored lists only. */
 assert(x9wr_ims_settings_applicable("ims",x9w_pcscf_lists[1]));
 assert(x9wr_ims_settings_applicable("IMS",x9w_pcscf_lists[1]));
 assert(!x9wr_ims_settings_applicable("cbnet",x9w_pcscf_lists[1]));
 assert(!x9wr_ims_settings_applicable("ims",x9w_pcscf_lists[0]));
 /* Serialize: exactly one 0x2e = [2][a1][a2]; no 0x23, no 0x32. */
 x9q_packet pkt; assert(x9wr_serialize_pcscf(x9w_pcscf_lists[1],pkt));
 assert(pkt.count==1&&pkt.tags[0]==0x2e&&pkt.lens[0]==33);
 assert(pkt.vals[0][0]==2&&!memcmp(pkt.vals[0]+1,a1,16)&&!memcmp(pkt.vals[0]+17,a2,16));
 /* v4 list: count tag 0x117, entry at 0x8118; QMI emission byte-reverses. */
 Builder c(2); c.tlv(0,result0,4); c.byte(0x117,1); uint8_t v4[4]={10,0,0,9}; c.tlv(0x8118,v4,4); c.finish();
 x9w_pcscf_list out4; assert(x9w_pcscf_decode(c.p,c.n,out4));
 assert(out4.v4_count==1&&out4.v6_count==0&&!memcmp(out4.v4[0],v4,4));
 x9q_packet pkt4; assert(x9wr_serialize_pcscf(out4,pkt4));
 assert(pkt4.count==1&&pkt4.tags[0]==0x23&&pkt4.lens[0]==5);
 assert(pkt4.vals[0][0]==1&&pkt4.vals[0][1]==9&&pkt4.vals[0][4]==10);
 /* Rejections: oversized count, short entry, zero address; store untouched. */
 Builder d(1); d.tlv(0,result0,4); d.byte(0x11c,5); d.finish();
 x9w_pcscf_list bad; assert(!x9w_pcscf_decode(d.p,d.n,bad));
 Builder e(1); e.tlv(0,result0,4); e.byte(0x11c,1); uint8_t zero[16]={}; e.tlv(0x811d,zero,16); e.finish();
 assert(!x9w_pcscf_decode(e.p,e.n,bad));
 x9w_pcscf_publish(0,e.p,e.n); assert(x9w_pcscf_lists[0].v6_count==0);
 printf("ims-pcscf ok\n");}
'''
        out = self.compile_run(code)
        self.assertIn('ims-pcscf ok', out)


if __name__ == '__main__':
    unittest.main()
