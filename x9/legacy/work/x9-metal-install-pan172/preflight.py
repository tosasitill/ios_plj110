from pathlib import Path
import json,subprocess,hashlib
p=Path(__file__).parent;items=json.loads((p/'install-plan.json').read_text())['blocks']; groups=[]
for row in items:
 if groups and row['offset']==groups[-1][-1]['offset']+4096 and len(groups[-1])<64: groups[-1].append(row)
 else: groups.append([row])
mismatch=[]
for group in groups:
 off=group[0]['offset'];n=len(group)*4096
 cmd=f'/data/local/tmp/x9-ios-pan92-20261027-1215/pinfile rawreadat /data/local/tmp/x9-ios-pan92-20261027-1215/iosroot-full.hfs /dev/block/sdc100 {off} {n} 2>/dev/null'
 data=subprocess.run(['adb','-s','X9_SERIAL_REQUIRED','exec-out','su','-c',cmd],check=True,capture_output=True,timeout=120).stdout
 if len(data)!=n:raise RuntimeError('short read')
 for i,row in enumerate(group):
  if hashlib.sha256(data[i*4096:(i+1)*4096]).hexdigest()!=row['old']: mismatch.append(row['offset'])
print('ranges',len(groups),'blocks',len(items),'mismatches',len(mismatch),'first',mismatch[:30],flush=True)
(p/'preflight.json').write_text(json.dumps({'mismatched_offsets':mismatch,'checked_blocks':len(items)},indent=2))
