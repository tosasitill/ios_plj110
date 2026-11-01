from pathlib import Path
import subprocess,hashlib,json,re,sys
p=Path(__file__).resolve().parent;txt=(p/'current-rawdiff.txt').read_text();records={int(m.group(1)):m.group(2) for m in re.finditer(r'^([0-9]+) ([0-9a-f]{64})$',txt,re.M)}
if len(records)!=77 or '65536 groups, 77 different' not in txt:raise SystemExit('incomplete scan')
img=p.parent/'x9-hfs-recovery/source-repair-copy.hfs';ranges=[];nchanged=0
with img.open('rb') as f:
 for i,(off,expected) in enumerate(sorted(records.items())):
  cmd=f'/data/local/tmp/x9-ios-pan92-20261027-1215/pinfile rawreadat /data/local/tmp/x9-ios-pan92-20261027-1215/iosroot-full.hfs /dev/block/sdc100 {off} 262144 2>/dev/null';q=subprocess.run(['adb','-s','X9_SERIAL_REQUIRED','exec-out','su','-c',cmd],capture_output=True,timeout=30)
  if q.returncode or len(q.stdout)!=262144 or hashlib.sha256(q.stdout).hexdigest()!=expected:raise RuntimeError('short read or disk changed at '+str(off))
  f.seek(off);src=f.read(262144);assert len(src)==262144;changed=[j for j in range(64) if src[j*4096:(j+1)*4096]!=q.stdout[j*4096:(j+1)*4096]];nchanged+=len(changed)
  for j in changed:ranges.append({'offset':off+j*4096,'old_sha256':hashlib.sha256(q.stdout[j*4096:(j+1)*4096]).hexdigest(),'new_sha256':hashlib.sha256(src[j*4096:(j+1)*4096]).hexdigest()})
  if (i+1)%10==0:print('verified groups',i+1,'changed blocks',nchanged,flush=True)
(p/'refined-diff.json').write_text(json.dumps({'group_count':len(records),'block_size':4096,'changed_blocks':nchanged,'changed_bytes':nchanged*4096,'baseline_sha256':'b9acc47415a2ed8fcdb516c01cce576ab3466f99834f0df6b729864fa5647323','blocks':ranges},indent=2)+'\n');print('complete blocks',nchanged,'bytes',nchanged*4096,flush=True)
