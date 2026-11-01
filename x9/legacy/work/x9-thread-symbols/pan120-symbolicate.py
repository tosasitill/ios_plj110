#!/usr/bin/env python3
import mmap, struct, bisect
from pathlib import Path
p=Path('/tmp/x9-symbol-root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld')
slide=0x1977c000
samples={'pan120 SpringBoard 60s': [7983388796, 8240854540, 7212689440, 7010290752, 7010981008, 7010310180, 7010980056, 7010619608, 7010168968, 7010970364], 'pan120 SpringBoard 180s': [8046185856, 8040811944, 8040812672, 8040812540, 8040813176, 8040811688, 8036719692, 8035907576, 8036445024], 'PosterBoard abort': [7983411608, 8240899576, 7100794040, 8240082772, 8240020044, 6870592252, 8240079908, 8240079828, 7099991956, 7099796956, 7712619480, 7968433932], 'Setup launch timeout': [7983389392, 7099793704, 7099793520, 7019573424, 7019571304, 7021729888, 7021729656, 7099991936, 7099796956, 7020377528, 7021777080, 7021776724], 'Widget common': [6870534628, 6870534560, 6870538216, 7480200736, 7480081108, 7480193052, 7480312864, 6870525044, 7099988928, 7099868056, 8241452944, 7480230956]}
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
