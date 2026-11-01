#!/usr/bin/env python3
import ctypes as C,json,os,stat,time,collections
from pathlib import Path
r=Path('/Users/tosasitill/HelioDarwin-x9-lk-preserve');root=r/'work/x9-cryptex/root-ro';out=r/'work/x9-decmpfs/all-codecs';out.mkdir(exist_ok=True)
lib=C.CDLL('/usr/lib/libSystem.B.dylib',use_errno=True);get=lib.getxattr;get.argtypes=[C.c_char_p,C.c_char_p,C.c_void_p,C.c_size_t,C.c_uint32,C.c_int];get.restype=C.c_ssize_t
counts=collections.Counter();samples={};errors=[];files=dirs=0;stack=[root];started=time.time();records=(out/'compressed-files.jsonl').open('w')
while stack:
 d=stack.pop();dirs+=1
 try:
  with os.scandir(d) as entries:
   for e in entries:
    try:
     st=e.stat(follow_symlinks=False)
     if stat.S_ISDIR(st.st_mode):stack.append(Path(e.path));continue
     if not stat.S_ISREG(st.st_mode):continue
     files+=1
     if not st.st_flags&0x20:continue
     buf=C.create_string_buffer(65536);n=get(os.fsencode(e.path),b'com.apple.decmpfs',buf,len(buf),0,0x20)
     if n<16:errors.append({'path':str(e.path),'getxattr':n,'errno':C.get_errno()});continue
     import struct
     magic,kind,size=struct.unpack_from('<IIQ',buf.raw);counts[kind]+=1
     item={'path':str(Path(e.path).relative_to(root)),'type':kind,'uncompressed_bytes':size,'attr_bytes':n}
     records.write(json.dumps(item)+'\n')
     key=str(kind);samples.setdefault(key,[])
     if len(samples[key])<10:samples[key].append(item)
    except OSError as ex:errors.append({'path':e.path,'error':str(ex)})
 except OSError as ex:errors.append({'path':str(d),'error':str(ex)})
 if dirs%500==0:print('scan',dirs,files,dict(counts),flush=True)
records.close();result={'root_image_read_only':True,'regular_files':files,'directories':dirs,'type_counts':dict(counts),'samples':samples,'errors':errors,'seconds':time.time()-started};(out/'inventory.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2),flush=True)
