#!/usr/bin/env python3
"""Read-only corpus validation for every type9 and type14 file in this root."""
from pathlib import Path
import importlib.util,ctypes as C,json,os,struct,time
r=Path('/Users/tosasitill/HelioDarwin-x9-lk-preserve');w=r/'work/x9-decmpfs/all-codecs';root=r/'work/x9-cryptex/root-ro'
s=importlib.util.spec_from_file_location('all_tests',r/'x9/tools/test_x9_all_codecs.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
m.AllCodecTests.setUpClass();t=m.AllCodecTests('test_inline_all_codecs_and_raw_markers');t.setup_io(b'');scratch=C.create_string_buffer(262144);out=C.create_string_buffer(65536)
lib=C.CDLL('/usr/lib/libSystem.B.dylib',use_errno=True);get=lib.getxattr;get.argtypes=[C.c_char_p,C.c_char_p,C.c_void_p,C.c_size_t,C.c_uint32,C.c_int];get.restype=C.c_ssize_t
attrbuf=C.create_string_buffer(65536)
def attr(p,name):
 n=get(os.fsencode(p),name,None,0,0,0x20)
 if n<0:raise OSError(C.get_errno(),str(p))
 b=C.create_string_buffer(n);assert get(os.fsencode(p),name,b,n,0,0x20)==n;return b.raw
records=[json.loads(x) for x in (w/'compressed-files.jsonl').read_text().splitlines()];counts={9:0,14:0};errors=[];started=time.time();total=0
try:
 for item in records:
  kind=item['type']
  if kind not in counts:continue
  p=root/item['path'];size=item['uncompressed_bytes']
  try:
   if kind==9:
    n=get(os.fsencode(p),b'com.apple.decmpfs',attrbuf,len(attrbuf),0,0x20);assert n>=17
    result=t.lib.x9d_decode(C.byref(t.io),9,C.addressof(attrbuf)+16,n-16,out,size,scratch)
    assert result==0,('decode',result)
    assert out.raw[:size]==p.read_bytes(),'content mismatch'
   else:
    blob=attr(p,b'com.apple.ResourceFork');assert t.open_resource(14,blob,size)==0
    with p.open('rb') as f:
     for off in range(0,size,65536):
      n=min(65536,size-off);result,decoded=t.read(off,n);assert result==0,('decode',result,'block',off//65536)
      assert decoded==f.read(n),('content mismatch',off)
   counts[kind]+=1;total+=size
  except Exception as ex:
   errors.append({'path':item['path'],'type':kind,'error':str(ex)})
   print('FAIL',errors[-1],flush=True)
   if len(errors)>=5:break
  if sum(counts.values())%10000==0:print('corpus',counts,'elapsed',round(time.time()-started,1),flush=True)
finally:m.AllCodecTests.tearDownClass()
result={'read_only_root':True,'validated_files_by_type':counts,'uncompressed_bytes_compared':total,'errors':errors,'seconds':time.time()-started,'hardware_or_kernel_VFS_emulated':False};(w/'corpus-validation.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2),flush=True)
