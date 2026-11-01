#!/usr/bin/env python3
import sys,struct,json,argparse,hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
a=argparse.ArgumentParser(description="Inspect original backboardd's haptic group wait; static analysis only")
a.add_argument('--backboardd',type=Path,required=True);a.add_argument('--report',type=Path,required=True);args=a.parse_args()
sys.path.insert(0,'/Users/tosasitill/HelioDarwin-x9-lk-preserve/work/legacy-import/xnu_x9/kits/NativePrelink');from native_prelink import macho,backed_offset
raw=args.backboardd.read_bytes();sha=hashlib.sha256(raw).hexdigest()
print("backboard_sha",sha)
m=macho(raw)
def rd(va,n):return raw[backed_offset(m['segments'],va,n,len(raw)):][:n]
def u32(va):return struct.unpack('<I',rd(va,4))[0]
def i32(va):return struct.unpack('<i',rd(va,4))[0]
def ptr(va):
 v=struct.unpack('<Q',rd(va,8))[0]
 if not v or v>>63:return 0
 v &= (1<<36)-1
 return v if v>=0x100000000 else 0x100000000+v
def cs(va):return rd(va,256).split(b'\0')[0].decode(errors='replace')
sections={};at=32
for i in range(struct.unpack_from('<I',raw,16)[0]):
 cmd,n=struct.unpack_from('<II',raw,at)
 if cmd==0x19:
  count=struct.unpack_from('<I',raw,at+64)[0]
  for j in range(count):
   a=at+72+j*80;name=raw[a:a+16].split(b'\0')[0].decode();va,size=struct.unpack_from('<QQ',raw,a+32);sections[name]=(va,size)
 at+=n
out=[]
def methods(cls,meta=False):
 ro=ptr(cls+32)&~7
 if not ro:return
 name=cs(ptr(ro+24));lst=ptr(ro+32)
 if not lst:return
 flags,count=struct.unpack('<II',rd(lst,8));size=flags&0xffff
 assert count<10000 and size in [12,24],(name,hex(flags),count)
 for j in range(count):
  e=lst+8+j*size
  if flags&0x80000000:
   np=e+i32(e);sel=cs(np if flags&0x40000000 else ptr(np));imp=e+8+i32(e+8)
  else:sel=cs(ptr(e));imp=ptr(e+16)
  out.append((imp,('+' if meta else '-')+'['+name+' '+sel+']'))
va,size=sections['__objc_classlist']
for a in range(va,va+size,8):
 cls=ptr(a);methods(cls);meta=ptr(cls)
 if meta:methods(meta,True)
out.sort();methods=dict(out)

for addr, name in sorted(methods.items()):
 if 0x100076000 < addr < 0x10007a000: print(hex(addr),name)
