#!/usr/bin/env python3
import mmap, struct, bisect
from pathlib import Path
p=Path('/tmp/x9-symbol-root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld')
slide=0x43938000
samples={
'lsd t0':[0x205f49030,0x205f5ab18,0x205f5adb8,0x205f49524,0x1ca794148,0x1ca7952e0,0x1ca799d20,0x1ca7d8bec,0x1c5624cc0,0x1c5624bf4,0x1e7f5c344],
'backboardd t0':[0x205f49030,0x205f5ab18,0x205f5adb8,0x205f49524,0x1ca794148,0x1ca7952e0,0x1ca799d20,0x1023ade40,0x1e7f5c344],
'backboardd t4':[0x205f49030,0x205f5ab18,0x205f5adb8,0x205f49524,0x1d14a2308,0x1d14a2694,0x2155260ec,0x20b242334,0x20b247de0,0x20b24660c,0x20b244cc0,0x10241c1f0,0x1023ad654],
'backboardd t9':[0x205f49ad0,0x1d14a0128,0x1d14a04a0,0x1023bca78,0x1d14cf7a8,0x1d14d0780,0x1d14ab6fc,0x1d14ac1e4,0x1d14b5f14,0x2154d2bd0,0x2154d2720],
'SpringBoard t0':[0x205f49030,0x205f5ab18,0x205f5adb8,0x205f49524,0x1d14a2308,0x1d14a2694,0x2155260ec,0x1df2e7128,0x1df2e9e78,0x1df2e9554,0x1d1630ce0,0x1e5fd4304,0x20919d5c8],
'SpringBoard t1':[0x205f49ad0,0x1d14a0128,0x1d149ff10,0x1d14b2cfc,0x1d14b28fc,0x1e02fc980,0x1d14cf7a8,0x1d14d0780,0x1d14ab6fc,0x1d14ac1e4,0x1d14b5f14,0x2154d2bd0,0x2154d2720]}
with (p/'dyld_shared_cache_arm64').open('rb') as f, mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as b, (p/'dyld_shared_cache_arm64.symbols').open('rb') as sf, mmap.mmap(sf.fileno(),0,access=mmap.ACCESS_READ) as sm:
 if sm[88:104]!=b[400:416]: raise ValueError('symbol UUID mismatch')
 off,num=struct.unpack_from('<2Q',b,136); region=struct.unpack_from('<Q',b,224)[0]
 images=[]
 for i in range(num):
  _,va,size,path=struct.unpack_from('<16sQ2I',b,off+32*i)
  images.append((va,size,b[path:b.find(b'\0',path)].decode()))
 lo=struct.unpack_from('<Q',sm,72)[0];no,nc,so,ss,eo,ec=struct.unpack_from('<6I',sm,lo)
 entries={struct.unpack_from('<Q',sm,lo+eo+i*16)[0]:struct.unpack_from('<2I',sm,lo+eo+i*16+8) for i in range(ec)}
 for name,pcs in samples.items():
  print('\n'+name)
  for idx,pc in enumerate(pcs):
   va=pc-slide-(4 if idx>=2 else 0)
   img=next((im for im in images if im[0]<=va<im[0]+im[1]),None)
   if not img: print(hex(pc),'not in cache (may be executable)');continue
   entry=entries.get(img[0]-region);best=None
   if entry:
    start,cnt=entry
    if start+cnt>nc: raise ValueError('bad symbol range')
    for i in range(start,start+cnt):
     st,kind,sect,_,v=struct.unpack_from('<IBBHQ',sm,lo+no+i*16)
     if not kind&0xe0 and kind&0xe==0xe and sect and img[0]<=v<=va and (best is None or v>best[0]):best=(v,st)
   symbol='no local symbol'
   if best and best[1]<ss:
    pos=lo+so+best[1];symbol=sm[pos:sm.find(b'\0',pos)].decode(errors='replace')+' + '+hex(va-best[0])
   print(hex(pc), img[2], symbol)
