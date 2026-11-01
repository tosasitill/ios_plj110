#!/usr/bin/env python3
"""Restore a validated dedicated X9 root using a reviewed 4KiB diff plan."""
from pathlib import Path
import hashlib,json,shlex,subprocess,sys
P=Path(__file__).resolve().parent
ROOT=P.parents[1]
PLAN=P/'journal-recovery-plan.json'
TARGET=P/'journal-baseline.hfs'
MANIFEST=ROOT/'x9/out/pan97-amfi-aks-only/root-manifest.json'
SERIAL='X9_SERIAL_REQUIRED'; DEV='/dev/block/sdc100'
BLOCK=4096; MAX=64*BLOCK

def sha(b):return hashlib.sha256(b).hexdigest()

def load_plan(plan,target):
 m=json.loads(plan.read_text()); items=m['blocks']; n=target.stat().st_size
 if n!=17179869184 or len(items)!=m['changed_blocks'] or m['changed_bytes']!=len(items)*BLOCK or m['target_sha256']!='9d32f53c49c00a0f64f151d237b435bf30b410de6e5982392bb00a323c2cb644':raise ValueError('plan/target identity or geometry mismatch')
 previous=-1
 with target.open('rb') as f:
  for x in items:
   off=x['offset'];old=x['old_sha256'];new=x['new_sha256']
   if type(off)!=int or off%BLOCK or off<=previous or off>n-BLOCK or len(old)!=64 or len(new)!=64:raise ValueError('invalid plan entry')
   bytes.fromhex(old);bytes.fromhex(new);f.seek(off)
   if sha(f.read(BLOCK))!=new:raise ValueError('target block does not match plan at '+str(off))
   previous=off
 return items

def ranges(items):
 groups=[]
 for item in items:
  if groups and item['offset']==groups[-1][-1]['offset']+BLOCK and len(groups[-1])<64:groups[-1].append(item)
  else:groups.append([item])
 # The primary volume header remains dirty until every other block succeeds.
 return [g for g in groups if g[0]['offset']!=0]+[g for g in groups if g[0]['offset']==0]

def adb(args,inp=None,timeout=60):
 p=subprocess.run(['adb','-s',SERIAL]+args,input=inp,capture_output=True,timeout=timeout)
 if p.returncode:raise RuntimeError('adb operation failed: '+p.stderr.decode(errors='replace')[-500:]+p.stdout.decode(errors='replace')[-500:])
 return p.stdout

class Device:
 def __init__(self,m):
  self.path=m['device_file'];self.tool=str(Path(self.path).parent/'pinfile')
  if m['device_serial']!=SERIAL or self.path!='/data/local/tmp/x9-ios-pan92-20261027-1215/iosroot-full.hfs':raise ValueError('unexpected manifest target')
 def read(self,off,n):
  cmd=f'{shlex.quote(self.tool)} rawreadat {shlex.quote(self.path)} {DEV} {off} {n} 2>/dev/null'
  data=adb(['exec-out','su','-c',cmd],timeout=60)
  if len(data)!=n:raise RuntimeError('short/error rawread at '+str(off))
  return data
 def write(self,off,data,old):
  cmd=f'{shlex.quote(self.tool)} rawwriteat {shlex.quote(self.path)} {DEV} {off} {old} --yes'
  output=adb(['shell','-T','su','-c',cmd],data,timeout=120).decode(errors='replace')
  if 'sha256 read back: '+sha(data) not in output or f'wrote {len(data)} bytes' not in output:raise RuntimeError('unexpected write confirmation at '+str(off)+': '+output[-400:])

def apply(items,target,device):
 written=skipped=0
 with target.open('rb') as f:
  for group in ranges(items):
   start=group[0]['offset'];n=len(group)*BLOCK;actual=device.read(start,n);f.seek(start);want=f.read(n)
   if len(want)!=n:raise RuntimeError('short target')
   # Every block must still be the preflight state or already restored.
   pending=[]
   for i,x in enumerate(group):
    old=actual[i*BLOCK:(i+1)*BLOCK];new=want[i*BLOCK:(i+1)*BLOCK];h=sha(old)
    if sha(new)!=x['new_sha256']:raise RuntimeError('target changed')
    if h==x['new_sha256']:skipped+=BLOCK
    elif h==x['old_sha256']:pending.append(i)
    else:raise RuntimeError('unexpected current block at '+str(x['offset']))
   # Split around blocks already restored, so resume never rewrites them.
   runs=[]
   for i in pending:
    if runs and i==runs[-1][-1]+1:runs[-1].append(i)
    else:runs.append([i])
   for run in runs:
    lo=run[0]*BLOCK;hi=(run[-1]+1)*BLOCK;new=want[lo:hi];old=actual[lo:hi]
    device.write(start+lo,new,sha(old))
    if device.read(start+lo,len(new))!=new:raise RuntimeError('independent readback mismatch at '+str(start+lo))
    written+=len(new)
   print('checked offset',start,'blocks',len(group),'written',written,'already_correct',skipped,flush=True)
 return written,skipped

def main():
 if len(sys.argv) not in (2,3) or sys.argv[1]!='--apply-reviewed-plan':raise SystemExit('usage: restore_journal.py --apply-reviewed-plan [PLAN]')
 plan=Path(sys.argv[2]) if len(sys.argv)==3 else PLAN
 items=load_plan(plan,TARGET);m=json.loads(MANIFEST.read_text());d=Device(m)
 subprocess.run([str(ROOT/'x9/out/pan97-amfi-aks-only/check-root.sh')],check=True)
 written,skipped=apply(items,TARGET,d)
 print('RESTORE BLOCKS COMPLETE',written,'bytes written',skipped,'bytes already correct; full rawhash still required',flush=True)
if __name__=='__main__':main()
