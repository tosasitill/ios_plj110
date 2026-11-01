#!/usr/bin/env python3
"""Replay untouched20H392 launchd_cache_loader through cache delivery.
Executes original sysctl buffer sizing, bootarg parser, control flow, file
mapping and Mach memory-entry mapping helpers. libc/OS calls are mocked;
this proves the narrow loader flow, not PID1 reception or a hardware boot.
"""
import argparse,json,re,struct,hashlib
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X30,UC_ARM64_REG_SP,UC_ARM64_REG_PC,UC_ARM64_REG_CPACR_EL1
B=0x100000000;RAM=0x200000000;FILE_MAP=0x240000000;RECEIVER=0x250000000;PAGE=16384
class Replay:
 def __init__(self,binary,cache,bootargs,sysctl_error=0,log_open=True):
  self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.mem_map(B,0x100000);self.u.mem_map(RAM,0x200000)
  self.bootargs=bootargs.encode()+b'\0';self.cache=cache.read_bytes();self.calls=[];self.messages=[];self.sysctl_error=sysctl_error;self.log_open=log_open;self.signature_path=False;self.heap=RAM+0x30000;self.guard=RAM+0x1000;self.errno=RAM+0x1020;self.stop=RAM+0x1ff000
  data=binary.read_bytes();assert struct.unpack_from('<I',data)[0]==0xfeedfacf
  off=32
  for _ in range(struct.unpack_from('<I',data,16)[0]):
   cmd,size=struct.unpack_from('<II',data,off)
   if cmd==0x19:
    name,va,vs,fo,fs=struct.unpack_from('<16sQQQQ',data,off+8)
    if fs:self.u.mem_write(va,data[fo:fo+fs])
   off+=size
  for slot,ptr in [(0x4078,self.guard),(0x4080,RAM+0x1008),(0x40f8,RAM+0x1010),(0x41c8,RAM+0x1018)]:self.qput(B+slot,ptr)
  for p,v in [(self.guard,0x123456789abcdef),(RAM+0x1008,2),(RAM+0x1010,101),(RAM+0x1018,PAGE)]:self.qput(p,v)
  self.u.mem_write(RAM+0x10000,b'/System/Library/xpc/launchd.plist\0');self.u.mem_write(RAM+0x11000,b'/System/Library/xpc/launchd.plist.sig\0')
  self.u.mem_write(B+0x8050,struct.pack('<I',7939))
  self.u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20);self.u.reg_write(UC_ARM64_REG_SP,RAM+0x1f0000);self.u.hook_add(UC_HOOK_CODE,self.hook)
 def r(self,n):return self.u.reg_read(UC_ARM64_REG_X0+n)
 def string(self,p):return bytes(self.u.mem_read(p,2048)).split(b'\0')[0].decode()
 def q(self,p):return struct.unpack('<Q',self.u.mem_read(p,8))[0]
 def qput(self,p,v):self.u.mem_write(p,struct.pack('<Q',v))
 def alloc(self,data):p=self.heap;self.heap+=(len(data)+15)&~15;self.u.mem_write(p,data);return p
 def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v&0xffffffffffffffff);self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_X30))
 def hook(self,u,pc,size,_):
  if pc==B+0x2558:self.signature_path=True;u.emu_stop();return
  if pc==B+0x3838:self.calls.append('fopen:log');self.ret(3 if self.log_open else 0)
  elif pc in [B+0x3844,B+0x3868]:
   self.messages.append(self.string(self.r(1) if pc==B+0x3844 else self.r(0)));self.ret()
  elif pc==B+0x37d8:self.ret(self.errno)
  elif pc==B+0x3988:
   assert self.string(self.r(0))=='kern.bootargs';self.calls.append('sysctl:kern.bootargs')
   if self.sysctl_error:u.mem_write(self.errno,struct.pack('<I',self.sysctl_error));self.ret(-1);return
   if self.r(1)==0:self.qput(self.r(2),len(self.bootargs));self.ret();return
   cap=self.q(self.r(2));u.mem_write(self.r(1),self.bootargs[:cap]);self.qput(self.r(2),min(cap,len(self.bootargs)))
   if cap<len(self.bootargs):u.mem_write(self.errno,struct.pack('<I',12));self.ret(-1)
   else:self.ret()
  elif pc==B+0x3820:self.calls.append('calloc:sysctl');self.ret(self.alloc(bytes(self.r(0)*self.r(1))))
  elif pc==B+0x394c:self.ret(self.alloc(self.string(self.r(0)).encode()+b'\0'))
  elif pc==B+0x3850:self.ret()
  elif pc==B+0x3970:
   text=self.string(self.r(0));needle=self.string(self.r(1));at=text.find(needle);self.ret(self.r(0)+at if at>=0 else 0)
  elif pc==B+0x3964:self.ret(len(self.string(self.r(0))))
  elif pc==B+0x397c:
   assert self.r(2)==10;text=self.string(self.r(0));m=re.match(r'\s*[+-]?\d+',text);self.qput(self.r(1),self.r(0)+(len(m.group()) if m else 0));self.ret(int(m.group()) if m else 0)
  elif pc==B+0x3934:assert self.string(self.r(0))=='/System/Library/xpc/launchd.plist' and self.r(1)==0;self.calls.append('open:original-cache');self.ret(42)
  elif pc==B+0x385c:
   assert self.r(0)==42;u.mem_write(self.r(1),bytes(144));self.qput(self.r(1)+96,len(self.cache));self.calls.append('fstat');self.ret()
  elif pc==B+0x3898:
   assert [self.r(i) for i in range(1,6)]==[len(self.cache),1,2,42,0];u.mem_map(FILE_MAP,(len(self.cache)+4095)&~4095);u.mem_write(FILE_MAP,self.cache);self.calls.append('mmap:original-cache');self.ret(FILE_MAP)
  elif pc==B+0x3880:
   assert self.r(0)==101 and self.r(5)==7939 and self.r(2)==(len(self.cache)+PAGE-1)&~(PAGE-1)
   self.calls.append('mach_vm_map:launchd-memory-entry');u.mem_map(RECEIVER,self.r(2));self.qput(self.r(1),RECEIVER);self.ret()
  elif pc==B+0x388c:
   assert self.r(0)==RECEIVER and self.r(1)==FILE_MAP and self.r(2)==len(self.cache);u.mem_write(self.r(0),bytes(u.mem_read(self.r(1),self.r(2))));self.calls.append('memcpy:cache-to-launchd-entry');self.ret(RECEIVER)
  elif pc==B+0x38a4:assert self.r(0)==FILE_MAP;self.calls.append('munmap');self.ret()
  elif pc==B+0x382c:assert self.r(0)==42;self.calls.append('close');self.ret()
  elif pc in [B+0x3784,B+0x3778,B+0x3790,B+0x3754]:raise AssertionError('unexpected AMFI/IOKit path')
  elif pc in [B+0x37e4,B+0x3814]:raise AssertionError('unexpected stack failure/abort')
 def run(self):
  self.u.reg_write(UC_ARM64_REG_X0,RAM+0x10000);self.u.reg_write(UC_ARM64_REG_X0+1,RAM+0x11000);self.u.reg_write(UC_ARM64_REG_X30,self.stop)
  self.u.emu_start(B+0x23f8,self.stop,count=100000)
  delivered=False
  if not self.signature_path:
   assert self.u.reg_read(UC_ARM64_REG_PC)==self.stop
   delivered=bytes(self.u.mem_read(RECEIVER,len(self.cache)))==self.cache
  return {'bootargs':self.bootargs[:-1].decode(),'sysctl_error':self.sysctl_error,'log_open':self.log_open,'signature_service_path_selected':self.signature_path,'original_cache_bytes':len(self.cache),'cache_delivered_byte_identical':delivered,'calls':self.calls,'log_formats':self.messages}
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--binary',type=Path,default=Path(__file__).with_name('launchd_cache_loader.original'));ap.add_argument('--cache',type=Path,default=Path(__file__).with_name('launchd.plist.original'));ap.add_argument('--out',type=Path,required=True);ap.add_argument('--candidate-build',type=Path);a=ap.parse_args()
 cases=[('',0,True,False),('launchd_unsecure_cache=0',0,True,False),('launchd_unsecure_cache=1',0,True,True),('launchd_unsecure_cache=1',0,False,True),('launchd_unsecure_cache=bad',0,True,False),('launchd_unsecure_cache=1',5,True,False)]
 if a.candidate_build:cases.append((re.search(r"--boot-args '([^']+)'",a.candidate_build.read_text()).group(1),0,True,True))
 rows=[]
 for args,error,logging,expected in cases:
  row=Replay(a.binary,a.cache,args,error,logging).run();assert row['cache_delivered_byte_identical']==expected;assert row['signature_service_path_selected']!=expected;rows.append(row)
 a.out.write_text(json.dumps({'binary':str(a.binary),'cache_input_sha256':hashlib.sha256(a.cache.read_bytes()).hexdigest(),'cases':rows,'limits':'Original code executed, OS/libc/Mach boundaries mocked; receiving PID1 and hardware not emulated'},indent=2)+'\n');print(json.dumps([{'bootargs':r['bootargs'],'service_path':r['signature_service_path_selected'],'cache_delivered':r['cache_delivered_byte_identical']} for r in rows],indent=2))
if __name__=='__main__':main()
