#!/usr/bin/env python3
"""Generate a read-only 4KiB recovery plan after rawdiff against journal baseline."""
from pathlib import Path
import subprocess,hashlib,json,re,sys
P=Path(__file__).resolve().parent
ROOT=P.parents[1];CHUNK=262144;BLK=4096
TARGET=P/'journal-baseline.hfs';DIGEST=P/'journal-256k.sha256';MANIFEST=ROOT/'x9/out/pan97-amfi-aks-only/root-manifest.json'
SERIAL='X9_SERIAL_REQUIRED'
def sha(b):return hashlib.sha256(b).hexdigest()
def main():
 if len(sys.argv)!=3:raise SystemExit('usage: plan_from_scan.py SCAN_OUTPUT PLAN_OUTPUT (never overwrites)')
 scan=Path(sys.argv[1]);out=Path(sys.argv[2]);text=scan.read_text();match=re.search(r'rawdiff complete: 17179869184 bytes, 65536 groups, (\d+) different',text)
 if not match:raise ValueError('raw scan did not complete')
 entries=re.findall(r'^([0-9]+) ([0-9a-f]{64})$',text,re.M);current={int(a):h for a,h in entries}
 if len(current)!=len(entries) or len(entries)!=int(match.group(1)):raise ValueError('invalid raw scan records')
 digests=DIGEST.read_bytes()
 if len(digests)!=65536*32 or TARGET.stat().st_size!=17179869184:raise ValueError('baseline geometry')
 subprocess.run([str(ROOT/'x9/out/pan97-amfi-aks-only/check-root.sh')],check=True)
 m=json.loads(MANIFEST.read_text());device=m['device_file'];tool=str(Path(device).parent/'pinfile');changes=[]
 with TARGET.open('rb') as f:
  for i,(off,expected) in enumerate(sorted(current.items())):
   if off%CHUNK or off<0 or off>=17179869184:raise ValueError('invalid group offset')
   f.seek(off);want=f.read(CHUNK)
   if len(want)!=CHUNK or hashlib.sha256(want).digest()!=digests[off//CHUNK*32:(off//CHUNK+1)*32]:raise ValueError('baseline digest mismatch')
   cmd=f'{tool} rawreadat {device} /dev/block/sdc100 {off} {CHUNK} 2>/dev/null';q=subprocess.run(['adb','-s',SERIAL,'exec-out','su','-c',cmd],capture_output=True,timeout=30)
   if q.returncode or len(q.stdout)!=CHUNK or sha(q.stdout)!=expected:raise RuntimeError('raw group changed or read failed at '+str(off))
   for n in range(CHUNK//BLK):
    old=q.stdout[n*BLK:(n+1)*BLK];new=want[n*BLK:(n+1)*BLK]
    if old!=new:changes.append({'offset':off+n*BLK,'old_sha256':sha(old),'new_sha256':sha(new)})
   if (i+1)%50==0:print('checked',i+1,'groups',flush=True)
 plan={'target_sha256':'9d32f53c49c00a0f64f151d237b435bf30b410de6e5982392bb00a323c2cb644','changed_blocks':len(changes),'changed_bytes':len(changes)*BLK,'blocks':changes}
 with out.open('x') as f:json.dump(plan,f,indent=2);f.write('\n')
 print('read-only plan:',len(changes),'blocks',len(changes)*BLK,'bytes')
if __name__=='__main__':main()
