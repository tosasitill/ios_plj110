#!/usr/bin/env python3
"""Read-only ARM64 cache inspection; addresses are unslid virtual addresses."""
import struct, mmap, sys
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
root=Path('/tmp/x9-symbol-root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld')
maps=[];files=[]
for p in sorted(root.glob('dyld_shared_cache_arm64*')):
 if p.name.endswith('.symbols'):continue
 f=p.open('rb'); b=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ);files.append((f,b))
 off,n=struct.unpack_from('<II',b,16)
 for i in range(n):
  va,size,pos=struct.unpack_from('<QQQ',b,off+32*i);maps.append((va,va+size,b,pos))
def read(va,n):
 for start,end,b,pos in maps:
  if start<=va and va+n<=end:return b[pos+va-start:pos+va-start+n]
 raise ValueError(hex(va))
if __name__=='__main__':
 va=int(sys.argv[1],0);n=int(sys.argv[2],0) if len(sys.argv)>2 else 256
 for i in Cs(CS_ARCH_ARM64,CS_MODE_ARM).disasm(read(va,n),va):print(f'{i.address:#x}: {i.mnemonic:8} {i.op_str}')
