from pathlib import Path
import hashlib,sys,os
image=Path(sys.argv[1]);output=Path(sys.argv[2]);expected=sys.argv[3]
if image.stat().st_size!=17179869184:raise SystemExit('unexpected image size')
h=hashlib.sha256();count=0
with image.open('rb') as f,output.open('xb') as o:
 while b:=f.read(262144):
  if len(b)!=262144:raise SystemExit('unaligned input')
  h.update(b);o.write(hashlib.sha256(b).digest());count+=1
 o.flush();os.fsync(o.fileno())
if h.hexdigest()!=expected:raise SystemExit('baseline SHA mismatch')
print('verified baseline SHA',h.hexdigest(),'chunks',count,flush=True)
