#!/usr/bin/env python3
"""Exercise the actual queue-admission function against host bridge stubs."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT=pathlib.Path(__file__).resolve().parents[2]
class StorageReceipt(unittest.TestCase):
 def test_receipt(self):
  source=(ROOT/'x9/baseband/qcom/baseband/coachz_baseband.cpp').read_text()
  start=source.index('static __attribute__((unused)) bool bb_x9_sms_terminal_result(')
  end=source.index('// CALL replies/events use',start)
  code=r'''
#include <cstdint>
#include "x9_sms_receipt.h"
#define CBQ_PORT_BASE 100
#define CBQ_MAX_CLIENTS 2
#define CBQ_HDR 6
#define BB_FRAME_MAX 65536
#define BB_QUEUE 2
#define CBQ_TO_HOST 1
#define CBH_QMUX1 0
#define CBH_QMUX2 1
struct cbq_client {bool used,release_pending,private_service; unsigned service,channel; uint64_t generation;};
struct cbq_server {unsigned node;}; struct Q {cbq_client client[2];}; static Q bb_qmux;
struct cbq_result {unsigned kind,service,len,channel;};
struct Frame {unsigned len; uint8_t *p;};
struct bb_channel {Frame frames[2]; unsigned head,count,frames_out,bytes_out,frames_dropped,reads_waiting;};
static bb_channel bb_ch[2]; static int lock; static int *bb_lock=&lock; static void *bb_complete_call=nullptr;
static cbq_server srv={300}; static bool server=true; static unsigned calls;
static cbq_server *cbq_server_find(Q *,unsigned) {return server?&srv:nullptr;}
static cbq_result cbq_from_modem(Q *,uint32_t,uint32_t,const uint8_t *,uint32_t n,uint8_t *,uint32_t) {calls++;return {1,5,n+6,0};}
static void IOLockLock(int *) {} static void IOLockUnlock(int *) {}
static void IOLockWakeup(int *,bb_channel *,bool) {} static void thread_call_enter(void *) {}
''' +source[start:end]+r'''
int main(){
 uint8_t sdu[14]={2,42,0,0x31,0,7,0,2,4,0,0,0,0,0},owned[20]={};
 auto &c=bb_qmux.client[0];c={true,false,false,5,0,7};
 if(bb_x9_sms_terminal_result(100,300,8,sdu,14,owned,20)||calls)return 1;
 if(bb_x9_sms_terminal_result(100,301,7,sdu,14,owned,20)||calls)return 2;
 bb_ch[0].count=2;if(bb_x9_sms_terminal_result(100,300,7,sdu,14,owned,20)||bb_ch[0].count!=2)return 3;
 bb_ch[0].count=0;if(!bb_x9_sms_terminal_result(100,300,7,sdu,14,owned,20)||bb_ch[0].frames[0].p!=owned||bb_ch[0].count!=1)return 4;
 sdu[10]=1;if(bb_x9_sms_terminal_result(100,300,7,sdu,14,owned,20))return 5;
 sdu[10]=0;sdu[3]=0x37;if(bb_x9_sms_terminal_result(100,300,7,sdu,14,owned,20))return 6;
 // RawSend rejects truncated success and admits exactly one real result.
 uint8_t mo[19]={2,42,0,0x20,0,12,0,2,4,0,0,0,0,0,1,2,0,3,0},mo_owned[25]={};
 bb_ch[0].count=0;
 if(bb_x9_sms_terminal_result(100,300,8,mo,19,mo_owned,25))return 7;
 if(bb_x9_sms_terminal_result(100,300,7,mo,14,mo_owned,20))return 8;
 if(!bb_x9_sms_terminal_result(100,300,7,mo,19,mo_owned,25)||bb_ch[0].count!=1)return 9;
 bb_ch[0].count=0;mo[10]=1;mo[12]=3;
 if(bb_x9_sms_terminal_result(100,300,7,mo,19,mo_owned,25))return 10;
 mo[5]=7;
 if(!bb_x9_sms_terminal_result(100,300,7,mo,14,mo_owned,20)||bb_ch[0].count!=1)return 11;
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as tmp:
   src=pathlib.Path(tmp)/'t.cpp';out=pathlib.Path(tmp)/'t';src.write_text(code)
   subprocess.run(['c++','-std=c++17','-Wall','-Werror','-I',str(ROOT/'x9/kernel/src/bsd/dev'),str(src),'-o',str(out)],check=True)
   subprocess.run([str(out)],check=True)
if __name__=='__main__':unittest.main()
