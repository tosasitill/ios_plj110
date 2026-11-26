#!/usr/bin/env python3
"""Host-check production poll's consumed-CNF retirement branch with stubs."""
import pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[2]
class PollHold(unittest.TestCase):
 def test_real_taken_branch(self):
  s=(ROOT/'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc').read_text()
  a=s.index('   if(taken==1){',s.index('static void x9m_poll()'))
  b=s.index('\n  }\n }\n for(unsigned i=0;i<X9M_INCOMING',a)
  code=r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#define bzero(p,n) std::memset(p,0,n)
#define X9M_SEND 1
#define X9Q_NOT_READY 16
struct Job {unsigned op=7,sim=0,step=1;uint64_t token=1,deadline=100;bool answered=false,receipt_pending=false,hold=false,follow=false;uint8_t req[16]={},stored_pdu[16]={};};
static uint64_t now;static int outcome,errors;
static bool x9m_storage(unsigned n){return n==7;}
static uint64_t x9q_ms(){return now;}
static void x9m_error(Job &j,int){errors++;j.answered=true;j.receipt_pending=true;}
static void x9m_job_done(Job &j,const uint8_t *,unsigned){
 if(outcome==1){j.hold=true;} else if(outcome==2){j.step=2;j.token=0;j.follow=true;}
 else {j.answered=true;j.receipt_pending=true;}
}
static void run(Job &j,uint64_t t,int result){now=t;outcome=result;errors=0;int taken=1;uint8_t p[32]={};unsigned n=0;
 for(int i=0;i<1;i++){
''' +s[a:b]+r'''
 }}
int main(){
 Job a;run(a,99,0);if(!a.receipt_pending||a.token||!a.answered)return 1;
 Job b;run(b,100,0);if(errors!=1||!b.receipt_pending||b.token)return 2;
 Job c;run(c,99,1);if(!c.hold||!c.token)return 3;
 Job d;run(d,99,2);if(!d.follow||d.step!=2||d.token)return 4;
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d)/'t.cpp';o=pathlib.Path(d)/'t';p.write_text(code)
   subprocess.run(['c++','-std=c++17','-Wall','-Werror',str(p),'-o',str(o)],check=True)
   subprocess.run([str(o)],check=True)
if __name__=='__main__':unittest.main()
