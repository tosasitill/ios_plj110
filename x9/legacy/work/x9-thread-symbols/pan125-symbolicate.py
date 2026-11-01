#!/usr/bin/env python3
import mmap, struct, bisect
from pathlib import Path
p=Path('/tmp/x9-symbol-root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld')
slide=0x1c0e4000
samples={'SpringBoard 60s main': [8026806396, 8284272140, 7256107040, 7053708352, 7054398608, 7053727780, 7054397656, 7054037208, 7053586568, 7054387964, 7053709620, 7053763212, 7054106212], 'SpringBoard 180s main': [8026806992, 8283668800, 6913930268, 6913947380, 6932087916, 6932090552, 6931850004, 6931848680, 6931777456, 6931840576, 6931839860, 6931839392, 6931838684], 'SpringBoard 300s main': [7084728580, 7061716868, 7061716428, 7061951328, 7061950764, 7061949696, 7061717420, 7504600364, 7504600764, 7504599624, 7504789516, 7504788332, 7504512776]}
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
