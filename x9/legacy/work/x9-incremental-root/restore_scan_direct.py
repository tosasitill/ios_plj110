#!/usr/bin/env python3
"""Restore scan-marked 256KiB groups without reading root data; header last."""
from concurrent.futures import ThreadPoolExecutor,as_completed
from pathlib import Path
import re,subprocess,sys
P=Path(__file__).resolve().parent;serial='X9_SERIAL_REQUIRED';root='/data/local/tmp/x9-ios-pan92-20261027-1215/iosroot-full.hfs';dev='/dev/block/sdc100';remote='/data/local/tmp/x9-direct-restore-1726';chunk=262144
text=Path(sys.argv[1]).read_text();m=re.search(r'rawdiff complete: 17179869184 bytes, 65536 groups, (\d+) different',text)
if not m:raise ValueError('incomplete scan')
entries=re.findall(r'^([0-9]+) ([0-9a-f]{64})$',text,re.M);offsets=sorted(int(x) for x,_ in entries)
if len(offsets)!=int(m.group(1)) or len(set(offsets))!=len(offsets) or any(x%chunk or x<0 or x>=17179869184 for x in offsets):raise ValueError('invalid scan')
ranges=[]
for off in offsets:
 if ranges and off==ranges[-1][0]+ranges[-1][1] and ranges[-1][1]<1048576:ranges[-1][1]+=chunk
 else:ranges.append([off,chunk])
header=[x for x in ranges if x[0]==0];others=[x for x in ranges if x[0]!=0]
def adb(args,**kw):return subprocess.run(['adb','-s',serial]+args,check=True,capture_output=True,**kw)
adb(['shell','mkdir',remote]);adb(['push',str(P/'pinfile-direct-restore'),remote+'/pinfile']);adb(['shell','chmod','700',remote+'/pinfile'])
def write(g):
 off,n=g
 with (P/'journal-baseline.hfs').open('rb') as f:f.seek(off);b=f.read(n)
 if len(b)!=n:raise RuntimeError('short baseline')
 cmd=f'{remote}/pinfile rawwriteat {root} {dev} {off} unused --yes';p=adb(['shell','-T','su','-c',cmd],input=b,timeout=120)
 if f'wrote {n} bytes at '.encode() not in p.stdout:raise RuntimeError('missing write confirmation '+str(off)+' '+str(p.stdout[-200:]))
 return n
print('START',len(offsets),'scan groups,',len(ranges),'write ranges,',len(offsets)*chunk,'bytes',flush=True)
done=total=0
try:
 with ThreadPoolExecutor(max_workers=6) as pool:
  fs=[pool.submit(write,g) for g in others]
  for future in as_completed(fs):
   try:n=future.result()
   except Exception:
    for f in fs:f.cancel()
    raise
   total+=n;done+=1
   if done%50==0:print('wrote ranges',done,'/',len(others),'bytes',total,flush=True)
 for g in header:total+=write(g)
 print('RESTORE COMPLETE',total,'bytes; no root data read or readback',flush=True)
finally:
 try:adb(['shell','rm','-f',remote+'/pinfile']);adb(['shell','rmdir',remote])
 except Exception:pass
