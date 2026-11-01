from pathlib import Path
import hashlib,json
p=Path(__file__).resolve().parent;a=p.parent/'x9-hfs-recovery/source-repair-copy.hfs';b=p/'journal-baseline.hfs';n=0;count=0;where=[];sha=hashlib.sha256()
with a.open('rb') as x,b.open('rb') as y:
 while True:
  aa=x.read(1048576);bb=y.read(1048576)
  if not aa and not bb:break
  if len(aa)!=len(bb):raise ValueError('size mismatch')
  sha.update(bb)
  if aa!=bb:
   for i in range(0,len(aa),4096):
    if aa[i:i+4096]!=bb[i:i+4096]:count+=1;where.append(n+i)
  n+=len(aa)
result={'bytes':n,'changed_blocks_4k':count,'changed_bytes':count*4096,'journal_sha256':sha.hexdigest(),'offsets':where};(p/'journal-diff.json').write_text(json.dumps(result,indent=2)+'\n');print('journal conversion',count,'blocks',count*4096,'bytes sha',sha.hexdigest(),flush=True)
