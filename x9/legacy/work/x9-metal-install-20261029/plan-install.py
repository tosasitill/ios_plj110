#!/usr/bin/env python3
"""Build a 4KiB incremental device-write plan from offline HFS edits.
Current root clone is an exact raw reconstruction; edited clone is unmounted.
"""
from pathlib import Path
import hashlib,json,os
p=Path(__file__).resolve().parent;old=p/'current-root.hfs';new=p/'edited-root.hfs';block=4096;chunk=1<<20
if old.stat().st_size!=17179869184 or new.stat().st_size!=old.stat().st_size:raise ValueError('HFS geometry')
items=[]
with old.open('rb') as a,new.open('rb') as b:
 for at in range(0,old.stat().st_size,chunk):
  x=a.read(chunk);y=b.read(chunk)
  if len(x)!=len(y):raise ValueError('short file')
  if x==y:continue
  for i in range(0,len(x),block):
   before=x[i:i+block];after=y[i:i+block]
   if before!=after:items.append({'offset':at+i,'old':hashlib.sha256(before).hexdigest(),'new':hashlib.sha256(after).hexdigest()})
if not items:raise ValueError('no offline edits')
(p/'install-plan.json').write_text(json.dumps({'block_bytes':block,'blocks':items,'changed_blocks':len(items),'device_writes':'pending'},indent=2)+'\n')
print('Offline incremental installation plan:',len(items),'blocks',len(items)*block,'bytes',flush=True)
