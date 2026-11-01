#!/usr/bin/env python3
import mmap, struct, bisect
from pathlib import Path
p=Path('/tmp/x9-symbol-root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld')
slide=0x264a0000
samples={'SpringBoard 60s main': [8250396388, 8249029664, 8256992636, 8256983668, 8252050208, 7233322644, 7233322532, 8252048836, 8252048508, 8255042736, 7200294872, 7200936128, 7200823464], 'SpringBoard 180s main': [8198492208, 8198564632, 8198565304, 8198493476, 7314907912, 7314908820, 8456298732, 7377029428, 7377028344, 7447661072, 7447479016, 7447691480, 7445657352], 'SpringBoard 300s main': [7225196116, 7362302012, 8956730428, 8956640620, 8956686400, 8956686172, 8956708212, 8956708968, 7233237736, 7221728780, 7221804844, 7221873136, 7222058432], 'backboardd worker 180s': [7682158892, 7682158240, 7682167516, 7682156524, 7682155868, 7682153468, 7682195412, 7682104408, 7682100984, 7682215372], 'backboardd shared worker': [7200587548, 7200507132, 7200509448, 7200710348, 7200741480, 7602322280, 7602321996, 7315097472], 'PosterBoard': [8198517144, 8456005112, 7315899576, 8455188308, 8455125580, 7085697788, 8455185444, 8455185364, 7315097492, 7314902492, 7927725016, 8183539468]}
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
