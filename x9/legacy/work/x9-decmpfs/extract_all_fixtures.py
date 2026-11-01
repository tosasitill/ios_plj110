#!/usr/bin/env python3
"""Extract compressed inputs and golden hashes via read-only host HFS decoding."""
import ctypes as C,os,struct,json,hashlib,random
from pathlib import Path
r=Path('/Users/tosasitill/HelioDarwin-x9-lk-preserve');w=r/'work/x9-decmpfs/all-codecs';root=r/'work/x9-cryptex/root-ro';fixtures=w/'fixtures';fixtures.mkdir(exist_ok=True)
lib=C.CDLL('/usr/lib/libSystem.B.dylib',use_errno=True);get=lib.getxattr;get.argtypes=[C.c_char_p,C.c_char_p,C.c_void_p,C.c_size_t,C.c_uint32,C.c_int];get.restype=C.c_ssize_t
def attr(p,name):
 key=name.encode();n=get(os.fsencode(p),key,None,0,0,0x20)
 if n<0:raise OSError(C.get_errno(),str(p))
 b=C.create_string_buffer(n);got=get(os.fsencode(p),key,b,n,0,0x20);assert got==n;return b.raw
records=[json.loads(x) for x in (w/'compressed-files.jsonl').read_text().splitlines()];selected=[]
for kind in [9,14]:
 rows=sorted([x for x in records if x['type']==kind and x['uncompressed_bytes']>0],key=lambda x:x['uncompressed_bytes'])
 selected.extend(rows[:2]);selected.append(next((x for x in rows if x['uncompressed_bytes']>4096),rows[-1]))
 if kind==14:selected.append(next(x for x in rows if x['uncompressed_bytes']>65536))
for suffix in ['.28.dyldlinkedit','.42.dyldlinkedit','.symbols']:
 selected.append(next(x for x in records if x['path'].endswith('dyld_shared_cache_arm64'+suffix)))
for item in json.loads((w/'corpus-validation-first.json').read_text())['errors']:
 selected.append(next(x for x in records if x['path']==item['path']))
results=[]
for index,item in enumerate(selected):
 p=root/item['path'];folder=fixtures/('%02d-type%d'%(index,item['type']));folder.mkdir(exist_ok=True)
 if (folder/'metadata.json').exists():
  meta=json.loads((folder/'metadata.json').read_text());assert meta['path']==item['path'];results.append(meta);continue
 a=attr(p,'com.apple.decmpfs');(folder/'decmpfs.xattr').write_bytes(a)
 size=struct.unpack_from('<Q',a,8)[0];meta=dict(item,folder=folder.name)
 if item['type']%2==0:
  data=attr(p,'com.apple.ResourceFork');(folder/'resource-fork.bin').write_bytes(data);meta['resource_bytes']=len(data)
  print(folder.name,p.name,'resource',len(data),'prefix',data[:24].hex(),flush=True)
 hashes=[];whole=hashlib.sha256()
 with p.open('rb') as f:
  while b:=f.read(65536):hashes.append(hashlib.sha256(b).hexdigest());whole.update(b)
 if size<=131072:(folder/'original.bin').write_bytes(p.read_bytes())
 meta['block_sha256']=hashes;meta['sha256']=whole.hexdigest();meta['samples']=[];rnd=random.Random(200+index)
 with p.open('rb') as f:
  for n in range(12):
   off=rnd.randrange(size);length=min(rnd.randrange(1,4097),size-off);f.seek(off);b=f.read(length);name='sample-%02d.bin'%n;(folder/name).write_bytes(b);meta['samples'].append({'offset':off,'bytes':length,'file':name})
 (folder/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n');results.append(meta)
(w/'fixtures.json').write_text(json.dumps(results,indent=2)+'\n');print('Extracted',len(results),'real fixtures without root writes.',flush=True)
