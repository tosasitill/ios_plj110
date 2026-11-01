#!/usr/bin/env python3
"""Reconstruct changed 256KiB scan groups via contiguous bounded reads."""
from pathlib import Path
import re,subprocess,hashlib,os,json,concurrent.futures
p=Path(__file__).resolve().parent;text=(p/'current-scan.txt').read_text();m=re.search(r'rawdiff complete: 17179869184 bytes, 65536 groups, (\d+) different',text)
if not m:raise RuntimeError('incomplete scan')
items=[(int(a),b) for a,b in re.findall(r'^(\d+) ([0-9a-f]{64})$',text,re.M)];size=262144
if len(items)!=int(m.group(1)) or len(set(a for a,_ in items))!=len(items):raise RuntimeError('bad records')
groups=[]
for off,digest in sorted(items):
 if off%size or off>=17179869184:raise RuntimeError('bad offset')
 if groups and off==groups[-1][-1][0]+size and len(groups[-1])<32:groups[-1].append((off,digest))
 else:groups.append([(off,digest)])
out=p/'current-root.hfs';fd=os.open(out,os.O_RDWR)
def read(group):
 start=group[0][0];n=len(group)*size
 cmd=f'/data/local/tmp/x9-ios-pan92-20261027-1215/pinfile rawreadat /data/local/tmp/x9-ios-pan92-20261027-1215/iosroot-full.hfs /dev/block/sdc100 {start} {n} 2>/dev/null'
 x=subprocess.run(['adb','-s','X9_SERIAL_REQUIRED','exec-out','su','-c',cmd],check=True,capture_output=True,timeout=120).stdout
 if len(x)!=n:raise RuntimeError('short read '+str(start))
 for i,(off,digest) in enumerate(group):
  if hashlib.sha256(x[i*size:(i+1)*size]).hexdigest()!=digest:raise RuntimeError('device changed after scan '+str(off))
 if os.pwrite(fd,x,start)!=n:raise RuntimeError('local write short')
 return n
try:
 total=0
 with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
  for i,n in enumerate(pool.map(read,groups)):
   total+=n
   if i%50==0:print('Reconstructed',i,'/',len(groups),'ranges, bytes',total,flush=True)
 os.fsync(fd)
finally:os.close(fd)
(p/'reconstruction.json').write_text(json.dumps({'groups':len(items),'ranges':len(groups),'bytes':total,'source_scan':'current-scan.txt','device_write':False},indent=2)+'\n')
print('Exact current root reconstructed:',total,'bytes read, no device writes',flush=True)
