#!/usr/bin/env python3
"""Replay original iOS16 keybagd bootarg detector and its real MKB_INIT call gate.
Only sysctlbyname/bzero/strnstr and the failing bag loader are mocked.
No Apple code is edited and no files/keys/bags are created.
"""
import argparse,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X30,UC_ARM64_REG_SP,UC_ARM64_REG_PC
BASE=0x100000000
DETECT=BASE+0xcee0
GATE=BASE+0x12bec
AFTER_GATE=BASE+0x12bfc
BAG_LOAD=BASE+0x12c30
GUARD_SLOT=BASE+0x2c340
class Replay:
 def __init__(self,binary,bootargs,sysctl_error=0):
  self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.calls=[];self.bootargs=bootargs.encode();self.sysctl_error=sysctl_error
  data=binary.read_bytes();assert struct.unpack_from('<I',data)[0]==0xfeedfacf
  self.u.mem_map(BASE,0x100000);self.u.mem_map(0x200000000,0x200000);self.stop=0x2001ff000
  off=32
  for _ in range(struct.unpack_from('<I',data,16)[0]):
   cmd,size=struct.unpack_from('<II',data,off)
   if cmd==0x19:
    name,va,vs,fo,fs=struct.unpack_from('<16sQQQQ',data,off+8)
    if fs:self.u.mem_write(va,data[fo:fo+fs])
   off+=size
  # One imported pointer used by the detector's stack protector. The rest of
  # its control flow uses PC-relative addresses/calls and no dynamic fixups.
  self.u.mem_write(GUARD_SLOT,struct.pack('<Q',0x200001000));self.u.mem_write(0x200001000,struct.pack('<Q',0x123456789abcdef0))
  self.u.reg_write(UC_ARM64_REG_SP,0x2001f0000);self.u.hook_add(UC_HOOK_CODE,self.hook)
 def reg(self,n):return self.u.reg_read(UC_ARM64_REG_X0+n)
 def string(self,p):return bytes(self.u.mem_read(p,1024)).split(b'\0')[0].decode()
 def ret(self,value=0):self.u.reg_write(UC_ARM64_REG_X0,value&0xffffffffffffffff);self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_X30))
 def hook(self,u,pc,size,_):
  if pc==BASE+0x200e0:
   self.calls.append('bzero');u.mem_write(self.reg(0),bytes(self.reg(1)));self.ret()
  elif pc==BASE+0x207e8:
   self.calls.append('sysctl:kern.bootargs');assert self.string(self.reg(0))=='kern.bootargs'
   if not self.sysctl_error:
    capacity=struct.unpack('<Q',u.mem_read(self.reg(2),8))[0];value=self.bootargs+b'\0';assert len(value)<=capacity
    u.mem_write(self.reg(1),value);u.mem_write(self.reg(2),struct.pack('<Q',len(value)))
   self.ret(self.sysctl_error)
  elif pc==BASE+0x207b8:
   self.calls.append('strnstr');needle=self.string(self.reg(1)).encode();assert needle==b'-apfs_shared_datavolume'
   data=bytes(u.mem_read(self.reg(0),self.reg(2)));at=data.find(needle);self.ret(self.reg(0)+at if at>=0 else 0)
  elif pc==BAG_LOAD:
   self.calls.append('fatal-path:load_devicebag');self.ret()
  elif pc==BASE+0x20068:raise AssertionError('stack protector failure')
 def run(self):
  self.u.reg_write(UC_ARM64_REG_X30,self.stop);self.u.emu_start(DETECT,self.stop,count=20000)
  assert self.u.reg_read(UC_ARM64_REG_PC)==self.stop
  detected=self.reg(0);self.u.emu_start(GATE,AFTER_GATE,count=20000);assert self.u.reg_read(UC_ARM64_REG_PC)==AFTER_GATE
  return {'bootargs':self.bootargs.decode(),'sysctl_error':self.sysctl_error,'detected':detected,'load_devicebag_called':'fatal-path:load_devicebag' in self.calls,'calls':self.calls}
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',type=Path,default=Path(__file__).with_name('keybagd.original'));p.add_argument('--out',type=Path,required=True);args=p.parse_args()
 cases=[('',0,False),('rootrw=1 x9_bootstrap_dirs=1',0,False),('-apfs_shared_datavolume rootrw=1 x9_bootstrap_dirs=1',0,True),('-apfs_shared_datavolume',1,False)]
 results=[]
 for boot,error,expected in cases:
  row=Replay(args.binary,boot,error).run();assert bool(row['detected'])==expected;assert row['load_devicebag_called']!=expected;results.append(row)
 args.out.write_text(json.dumps({'binary':str(args.binary),'profile':'original20H392 iOS16 keybagd','cases':results,'limit':'Original detector and call gate with mocked OS calls; not a whole-process boot, keybag, SEP or hardware test'},indent=2)+'\n');print(json.dumps(results,indent=2))
if __name__=='__main__':main()
