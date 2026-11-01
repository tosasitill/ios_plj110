#!/usr/bin/env python3
"""Symbolicate shared-cache stackshot frames using this build's matching local symbols."""
import mmap,struct,bisect,json
from pathlib import Path
root=Path('/tmp/x9-symbol-root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld')
inputs=Path('/Users/tosasitill/HelioDarwin/logs/x9-about-hfs-20261028-012105')
with (root/'dyld_shared_cache_arm64').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as b,(root/'dyld_shared_cache_arm64.symbols').open('rb') as sf,mmap.mmap(sf.fileno(),0,access=mmap.ACCESS_READ) as sm:
 if sm[88:104]!=b[400:416]:raise ValueError('symbol UUID mismatch')
 cache_uuid=b[88:104].hex();off,num=struct.unpack_from('<2Q',b,136);region=struct.unpack_from('<Q',b,224)[0];images=[]
 for i in range(num):
  _,va,size,path=struct.unpack_from('<16sQ2I',b,off+32*i);images.append((va,size,b[path:b.find(b'\0',path)].decode()))
 images.sort();starts=[v[0] for v in images]
 lo=struct.unpack_from('<Q',sm,72)[0];no,nc,so,ss,eo,ec=struct.unpack_from('<6I',sm,lo)
 entries={struct.unpack_from('<Q',sm,lo+eo+i*16)[0]:struct.unpack_from('<2I',sm,lo+eo+i*16+8) for i in range(ec)};symbols={}
 def resolve(va):
  i=bisect.bisect_right(starts,va)-1
  if i<0:return 'not in cache'
  img=images[i]
  if va>=img[0]+img[1]:return 'outside image range'
  if i not in symbols:
   entry=entries.get(img[0]-region);rows=[]
   if entry:
    start,count=entry
    if start+count>nc:raise ValueError('symbol bounds')
    for j in range(start,start+count):
     st,kind,sect,_,v=struct.unpack_from('<IBBHQ',sm,lo+no+j*16)
     if not kind&0xe0 and kind&0xe==0xe and sect and st<ss and img[0]<=v<img[0]+img[1]:
      pos=lo+so+st;rows.append((v,sm[pos:sm.find(b'\0',pos)].decode(errors='replace')))
   rows.sort();symbols[i]=([v for v,_ in rows],rows)
  addresses,rows=symbols[i];j=bisect.bisect_right(addresses,va)-1
  return img[2]+' '+(rows[j][1]+' + '+hex(va-rows[j][0]) if j>=0 else 'no local symbol')
 output=['shared cache UUID '+cache_uuid]
 for p in sorted(inputs.glob('file-*-Preferences*.ips')):
  s=p.read_text();_,n=json.JSONDecoder().raw_decode(s);o=json.loads(s[n:]);st=o['stackshot'];uuid=st['binaryImages'][2][0].replace('-','')
  if uuid!=cache_uuid:raise ValueError('stackshot/cache UUID mismatch')
  proc=st['processByPid'][str(o['pid'])];output+=['','FILE '+p.name,'PID '+str(o['pid'])+' '+o['captureTime'],'TERMINATION '+json.dumps(o['termination']),'WAITS '+json.dumps(proc.get('waitInfo'))]
  for tid,t in proc.get('threadById',{}).items():
   label=t.get('dispatch_queue_label','')
   if label not in ['com.apple.main-thread','com.apple.PreferencesUI.PSUIPrefsListController.Loading']:continue
   output+=['','THREAD '+tid+' '+label]
   for index,(imgidx,offset) in enumerate(t.get('userFrames',[])):
    uuid,base,kind=st['binaryImages'][imgidx];va=base+offset
    output.append(hex(va)+' '+(resolve(va) if kind=='S' else 'non-cache image '+uuid+' + '+hex(offset)))
 dest=inputs/'preferences-stackshot-symbols.txt';dest.write_text('\n'.join(output)+'\n');print('\n'.join(output));print('SAVED',dest)
