#!/usr/bin/env python3
"""Restore separate planned root ranges concurrently; primary HFS header last."""
from concurrent.futures import ThreadPoolExecutor,as_completed
from pathlib import Path
import hashlib,json,subprocess,sys
import restore_journal as r
if len(sys.argv)!=2:raise SystemExit('usage: restore_parallel.py PLAN')
plan=Path(sys.argv[1]);items=r.load_plan(plan,r.TARGET);m=json.loads(r.MANIFEST.read_text());d=r.Device(m)
subprocess.run([str(r.ROOT/'x9/out/pan97-amfi-aks-only/check-root.sh')],check=True)
groups=r.ranges(items);headers=[g for g in groups if g[0]['offset']==0];groups=[g for g in groups if g[0]['offset']!=0]
def restore(g):
 start=g[0]['offset'];n=len(g)*r.BLOCK;actual=d.read(start,n)
 with r.TARGET.open('rb') as f:f.seek(start);want=f.read(n)
 if len(want)!=n:raise RuntimeError('short target')
 runs=[];already=0
 for i,x in enumerate(g):
  old=actual[i*r.BLOCK:(i+1)*r.BLOCK];new=want[i*r.BLOCK:(i+1)*r.BLOCK];h=r.sha(old)
  if r.sha(new)!=x['new_sha256']:raise RuntimeError('target changed')
  if h==x['new_sha256']:already+=r.BLOCK;continue
  if h!=x['old_sha256']:raise RuntimeError('unexpected current block '+str(x['offset']))
  if runs and i==runs[-1][-1]+1:runs[-1].append(i)
  else:runs.append([i])
 written=0
 for run in runs:
  lo=run[0]*r.BLOCK;hi=(run[-1]+1)*r.BLOCK;d.write(start+lo,want[lo:hi],r.sha(actual[lo:hi]));written+=hi-lo
 return written,already
written=skipped=done=0
try:
 with ThreadPoolExecutor(max_workers=6) as pool:
  futures=[pool.submit(restore,g) for g in groups]
  for f in as_completed(futures):
   try:w,s=f.result()
   except Exception:
    for other in futures:other.cancel()
    raise
   written+=w;skipped+=s;done+=1
   if done%100==0:print('completed ranges',done,'/',len(groups),'written',written,flush=True)
 for g in headers:
  w,s=restore(g);written+=w;skipped+=s
 print('RESTORE COMPLETE',written,'bytes written,',skipped,'already correct; independent full-disk verification omitted',flush=True)
except Exception as e:
 print('RESTORE STOPPED:',repr(e),'; header phase not started unless failure occurred during header phase',flush=True);raise
