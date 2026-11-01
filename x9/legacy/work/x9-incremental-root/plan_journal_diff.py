from pathlib import Path
import subprocess,hashlib,json,re
p=Path(__file__).resolve().parent;t=(p/'current-rawdiff.txt').read_text();current={int(m.group(1)):m.group(2) for m in re.finditer(r'^([0-9]+) ([0-9a-f]{64})$',t,re.M)}
if len(current)!=77 or '65536 groups, 77 different' not in t:raise SystemExit('incomplete comparison')
j=json.loads((p/'journal-diff.json').read_text());groups=sorted(set(current)|{off//262144*262144 for off in j['offsets']});digests=(p/'baseline-256k.sha256').read_bytes();target=p/'journal-baseline.hfs';changes=[]
with target.open('rb') as f:
 for i,off in enumerate(groups):
  cmd=f'/data/local/tmp/x9-ios-pan92-20261027-1215/pinfile rawreadat /data/local/tmp/x9-ios-pan92-20261027-1215/iosroot-full.hfs /dev/block/sdc100 {off} 262144 2>/dev/null';q=subprocess.run(['adb','-s','X9_SERIAL_REQUIRED','exec-out','su','-c',cmd],capture_output=True,timeout=30)
  if q.returncode or len(q.stdout)!=262144:raise RuntimeError('read failed at '+str(off))
  h=hashlib.sha256(q.stdout).hexdigest();expected=current.get(off,digests[(off//262144)*32:(off//262144+1)*32].hex())
  if h!=expected:raise RuntimeError('current root changed at '+str(off))
  f.seek(off);want=f.read(262144)
  for n in range(64):
   old=q.stdout[n*4096:(n+1)*4096];new=want[n*4096:(n+1)*4096]
   if old!=new:changes.append({'offset':off+n*4096,'old_sha256':hashlib.sha256(old).hexdigest(),'new_sha256':hashlib.sha256(new).hexdigest()})
  if (i+1)%20==0:print('checked',i+1,'groups',flush=True)
ranges=[]
for b in changes:
 o=b['offset']
 if ranges and o==ranges[-1][0]+ranges[-1][1] and ranges[-1][1]<262144:ranges[-1][1]+=4096
 else:ranges.append([o,4096])
res={'current_group_scan':'current-rawdiff.txt','target_sha256':j['journal_sha256'],'changed_blocks':len(changes),'changed_bytes':len(changes)*4096,'ranges_up_to_256k':len(ranges),'blocks':changes};(p/'journal-recovery-plan.json').write_text(json.dumps(res,indent=2)+'\n');print('dry-run only:',len(changes),'blocks',len(changes)*4096,'bytes',len(ranges),'ranges',flush=True)
