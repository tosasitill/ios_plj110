#!/usr/bin/env python3
"""Reconstruct the exact current pinned root from the validated raw scan.
Read-only on device; retain raw differing groups and full local baseline clone.
"""
from pathlib import Path
import re,subprocess,hashlib,os,json
p=Path(__file__).resolve().parent;text=(p/'current-scan.txt').read_text()
m=re.search(r'rawdiff complete: 17179869184 bytes, 65536 groups, (\d+) different',text)
if not m:raise RuntimeError('raw scan not complete')
items=[(int(a),b) for a,b in re.findall(r'^(\d+) ([0-9a-f]{64})$',text,re.M)]
if len(items)!=int(m.group(1)) or len(set(a for a,_ in items))!=len(items):raise RuntimeError('scan records invalid')
groups=p/'original-groups';groups.mkdir(exist_ok=False);root=p/'current-root.hfs';size=262144
with root.open('r+b',buffering=0) as out:
 for i,(offset,expected) in enumerate(items):
  if offset%size or offset>=17179869184:raise RuntimeError('bad offset')
  cmd=f'/data/local/tmp/x9-ios-pan92-20261027-1215/pinfile rawreadat /data/local/tmp/x9-ios-pan92-20261027-1215/iosroot-full.hfs /dev/block/sdc100 {offset} {size} 2>/dev/null'
  data=subprocess.run(['adb','-s','X9_SERIAL_REQUIRED','exec-out','su','-c',cmd],check=True,capture_output=True,timeout=40).stdout
  if len(data)!=size or hashlib.sha256(data).hexdigest()!=expected:raise RuntimeError('raw group changed since scan at '+str(offset))
  (groups/(str(offset)+'.raw')).write_bytes(data);out.seek(offset);out.write(data)
  if i%32==0:print('Reconstructed',i,'/',len(items),'groups',flush=True)
 out.flush();os.fsync(out.fileno())
(p/'reconstruction.json').write_text(json.dumps({'groups':len(items),'bytes_per_group':size,'source_scan':'current-scan.txt','device_write':False},indent=2)+'\n')
print('Exact current root clone reconstructed, no device writes',flush=True)
