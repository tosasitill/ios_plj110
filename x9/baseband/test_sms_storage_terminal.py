#!/usr/bin/env python3
"""Host regression: compile production storage terminal/receipt helpers."""
import pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[2]
class Terminal(unittest.TestCase):
 def test_terminal(self):
  s=(ROOT/'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc').read_text()
  a=s.index('static void x9m_receipt_retry(');b=s.index('static void x9m_error(',a)
  code=r'''
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#define bzero(p,n) std::memset(p,0,n)
#define CBQ_HDR 6
#define X9M_SEND 1
#define CBQ_PORT_BASE 100
#define X9BB_NODE 300
struct x9m_job { bool answered=false,receipt_pending=false;unsigned slot=0,receipt_len=0,op=0,sim=0;uint8_t req[7]={};bool hold=false;uint64_t generation=1,bridge_generation=2,token=0;uint8_t receipt[1536]={};};
struct x9q_packet { uint8_t p[1536]={};unsigned n=14;bool good=true; };
static bool live=true,admit=false;static uint64_t identity=2;static unsigned calls=0,freed=0;
static unsigned x9q_u16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static bool x9m_live(unsigned,uint64_t){return live;}
[[maybe_unused]] static uint64_t bb_x9_sms_ack_identity(unsigned,unsigned){return identity;}
static void *IOMallocData(unsigned n){return malloc(n);}
static void IOFreeData(void *p,unsigned){freed++;free(p);}
static bool bb_x9_sms_terminal_result(unsigned,unsigned,uint64_t,const uint8_t *,unsigned,uint8_t *p,unsigned){calls++;if(admit)free(p);return admit;}
''' +s[a:b]+r'''
int main(){
 x9m_job j;j.op=X9M_SEND; x9q_packet first,late;first.p[7]=10;late.p[7]=99;
 x9m_terminal(j,first);
 if(!j.answered||!j.receipt_pending||j.receipt[7]!=10||calls!=1||freed!=1)return 1;
 x9m_terminal(j,late);if(j.receipt[7]!=10||calls!=1)return 2;
 admit=true;x9m_receipt_retry(j);
 if(j.receipt_pending||j.receipt_len||calls!=2)return 3;
 x9m_job k;admit=false;x9m_terminal(k,first);live=false;x9m_receipt_retry(k);
 if(k.receipt_pending||k.receipt_len||k.receipt[7])return 4;
 x9m_job m;live=true;identity=2;x9m_terminal(m,first);
 identity=0;x9m_receipt_retry(m);
 if(!m.receipt_pending||m.receipt[7]!=10)return 5;
 identity=3;m.token=11;m.hold=true;x9m_receipt_retry(m);
 if(m.receipt_pending||m.receipt_len||m.receipt[7]||m.token!=11||!m.hold)return 6;
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d)/'t.cpp';b=pathlib.Path(d)/'t';p.write_text(code)
   subprocess.run(['c++','-std=c++17','-Wall','-Werror',str(p),'-o',str(b)],check=True)
   subprocess.run([str(b)],check=True)
 def test_rawsend_owner_fences_reuse(self):
  s=(ROOT/'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc').read_text()
  route=(ROOT/'x9/kernel/src/iokit/Kernel/arm/X9QMIAT.inc').read_text()
  send=route[route.index('static void x9q_send('):]
  self.assertLess(send.index('x9m_send_repeated_wire('),send.index('!x9q_valid(req,len)'))
  a=s.index('static bool x9m_send_repeated_wire(')
  b=s.index('/* An active WMS storage',a)
  code=r'''
#include <cstdint>
#define CBQ_PORT_BASE 100
#define CBQ_MAX_CLIENTS 2
#define X9BB_PORT_BASE 200
#define X9BB_NODE 300
#define X9M_SEND 1
static unsigned x9q_u16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
struct x9m_job{unsigned op,slot;uint64_t generation;uint8_t req[7];};
static x9m_job x9m_jobs[1];
static struct {uint64_t generation;} x9q_clients[2];
''' +s[a:b]+r'''
int main(){
 uint8_t req[8]={0,42,0,0x20,0,8,0,0xff}; // malformed body
 x9q_clients[0].generation=1;
 auto repeats=[&]{return x9m_send_repeated_wire(100,300,205,req,sizeof(req));};
 if(repeats())return 1;
 x9m_jobs[0]={X9M_SEND,0,1,{0,42,0,0x20,0,0,0}};
 if(!repeats())return 2;
 if(x9m_send_repeated_wire(100,301,205,req,sizeof(req)))return 3;
 req[1]=43;if(repeats())return 4;
 req[1]=42;req[3]=0x37;if(repeats())return 5;
 req[3]=0x20;x9q_clients[0].generation=2;if(repeats())return 6;
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d)/'t.cpp';b=pathlib.Path(d)/'t';p.write_text(code)
   subprocess.run(['c++','-std=c++17','-Wall','-Werror',str(p),'-o',str(b)],check=True)
   subprocess.run([str(b)],check=True)
if __name__=='__main__':unittest.main()
