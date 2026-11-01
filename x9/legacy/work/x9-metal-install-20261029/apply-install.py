#!/usr/bin/env python3
"""Apply offline HFS file installation using expected old hashes, only root extents.
Reads and verifies each changed range; never scans or reads back the full root.
"""
from pathlib import Path
import json,hashlib,subprocess,os
p=Path(__file__).resolve().parent;manifest=json.loads((p/'install-plan.json').read_text());items=manifest['blocks'];size=4096
if len(items)!=manifest['changed_blocks'] or not items:raise ValueError('bad plan count')
for i,row in enumerate(items):
 if row['offset']%size or row['offset']<0 or row['offset']>=17179869184 or (i and row['offset']<=items[i-1]['offset']):raise ValueError('plan offset')
 if len(bytes.fromhex(row['old']))!=32 or len(bytes.fromhex(row['new']))!=32:raise ValueError('plan digest')
subprocess.run(['/bin/bash','/Users/tosasitill/HelioDarwin-x9-lk-preserve/x9/out/pan97-amfi-aks-only/check-root.sh'],check=True)
tool='/data/local/tmp/x9-ios-pan92-20261027-1215/pinfile';path='/data/local/tmp/x9-ios-pan92-20261027-1215/iosroot-full.hfs';device='/dev/block/sdc100'
def adb(args,data=None):
 x=subprocess.run(['adb','-s','X9_SERIAL_REQUIRED']+args,input=data,capture_output=True,timeout=120)
 if x.returncode:raise RuntimeError((x.stderr+x.stdout)[-1000:].decode(errors='replace'))
 return x.stdout
def sha(b):return hashlib.sha256(b).hexdigest()
groups=[]
for row in items:
 if groups and row['offset']==groups[-1][-1]['offset']+size and len(groups[-1])<64:groups[-1].append(row)
 else:groups.append([row])
groups=[g for g in groups if g[0]['offset']!=0]+[g for g in groups if g[0]['offset']==0]
count=0
with (p/'current-root.hfs').open('rb') as old,(p/'edited-root.hfs').open('rb') as new:
 for group in groups:
  offset=group[0]['offset'];length=len(group)*size;old.seek(offset);a=old.read(length);new.seek(offset);b=new.read(length)
  for i,row in enumerate(group):
   if sha(a[i*size:(i+1)*size])!=row['old'] or sha(b[i*size:(i+1)*size])!=row['new']:raise RuntimeError('local plan changed')
  read=f'{tool} rawreadat {path} {device} {offset} {length} 2>/dev/null';current=adb(['exec-out','su','-c',read])
  if current==b:continue
  if current!=a:raise RuntimeError('device changed at '+str(offset)+'; stopping without overwriting')
  cmd=f'{tool} rawwriteat {path} {device} {offset} {sha(a)} --yes'
  response=adb(['shell','-T','su','-c',cmd],b).decode(errors='replace')
  if f'wrote {length} bytes' not in response or 'sha256 read back: '+sha(b) not in response:raise RuntimeError('write confirmation missing at '+str(offset))
  count+=length
  if count%(16*1024*1024)<length:print('Applied',count,'bytes',flush=True)
print('Incremental plugin install complete:',count,'bytes; every written range verified by raw helper',flush=True)
