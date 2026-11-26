#!/usr/bin/env python3
"""Compile the real ModifyTag result branch; transport and parser are stubs."""
import pathlib,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[2]
class TagStages(unittest.TestCase):
 def test_real_tag_branch(self):
  text=(ROOT/'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc').read_text()
  anchor=text.index('const uint8_t *r=records.p;unsigned full=')
  start=text.index('   if(j.op==X9M_TAG){',anchor)
  end=text.index('   }else{\n    value[0]=r[3]^1u;',start)
  branch=text[start:end]+'   }\n'
  code=r'''
#include <cstdint>
#include <cstring>
#include <cstdio>
#define X9M_TAG 7
#define X9Q_NOT_READY 16
#define X9Q_INTERNAL 3
#define X9Q_UNSUPPORTED 94
struct Job{unsigned op=7,step=0,stored_len=0,slot=0,sim=0;uint64_t generation=1,deadline=100,token=9;bool hold=false,answered=false,follow=false;uint8_t stored_pdu[200]={};};
static uint64_t now=1;static bool live=true;static unsigned err;
static bool x9m_live(unsigned,uint64_t){return live;}
static uint64_t x9q_ms(){return now;}
static void x9m_error(Job &j,unsigned e){err=e;j.answered=true;}
static void apply(Job &j,const uint8_t *r,unsigned full){
''' +branch+r'''
}
int main(){
 uint8_t record[264]={};record[3]=0;record[8]=0;record[9]=4;unsigned full=3;
 Job j;err=0;apply(j,record,full);
 if(j.step!=1||!j.follow||j.token||j.stored_len!=3||j.stored_pdu[1]!=4)return 1;
 j.follow=false;j.token=10;record[3]=1;apply(j,record,full);
 if(j.step!=2||!j.follow||j.token||j.hold)return 2;
 j.follow=false;j.token=11;apply(j,record,full);if(j.hold||err)return 3;
 Job stale;stale.answered=true;err=0;apply(stale,record,full); /* already-read pre-read yields no mutation */
 if(stale.follow||stale.step)return 4;
 record[3]=0;Job expired;now=100;err=0;apply(expired,record,full);
 if(err!=X9Q_NOT_READY||expired.follow||expired.step)return 5;
 now=1;Job bad;bad.step=1;bad.stored_len=3;bad.stored_pdu[1]=5;err=0;apply(bad,record,full);
 if(!bad.hold||err!=X9Q_INTERNAL||bad.follow)return 6;
 Job unread;unread.step=2;unread.stored_len=3;std::memcpy(unread.stored_pdu,record+8,3);err=0;apply(unread,record,full);
 if(!unread.hold||err!=X9Q_INTERNAL)return 7;
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d)/'t.cpp';o=pathlib.Path(d)/'t';p.write_text(code)
   subprocess.run(['c++','-std=c++17','-Wall','-Werror',str(p),'-o',str(o)],check=True)
   subprocess.run([str(o)],check=True)
if __name__=='__main__':unittest.main()
