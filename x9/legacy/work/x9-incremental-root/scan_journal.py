#!/usr/bin/env python3
"""Read-only scan of dedicated X9 root against host journal baseline."""
from pathlib import Path
import hashlib,json,secrets,subprocess,sys
P=Path(__file__).resolve().parent;ROOT=P.parents[1];SERIAL='X9_SERIAL_REQUIRED'
def run(args,timeout=30):return subprocess.run(['adb','-s',SERIAL]+args,check=True,capture_output=True,timeout=timeout).stdout

def main():
 if len(sys.argv)!=2:raise SystemExit('usage: scan_journal.py NEW_OUTPUT_FILE')
 out=Path(sys.argv[1]);
 if out.exists():raise ValueError('refusing to overwrite scan')
 subprocess.run([str(ROOT/'x9/out/pan97-amfi-aks-only/check-root.sh')],check=True)
 raw=P/'rawdiff';digest=P/'journal-256k.sha256';manifest=json.loads((ROOT/'x9/out/pan97-amfi-aks-only/root-manifest.json').read_text());remote='/data/local/tmp/x9-rootdiff-'+secrets.token_hex(8)
 run(['shell','mkdir',remote]);created=[]
 try:
  for f,name in [(raw,'rawdiff'),(digest,'baseline.sha256')]:
   dest=remote+'/'+name;run(['push',str(f),dest],120);created.append(dest)
   if run(['shell','sha256sum',dest]).decode().split()[0]!=hashlib.sha256(f.read_bytes()).hexdigest():raise ValueError('uploaded helper or digest differs')
  run(['shell','chmod','700',remote+'/rawdiff'])
  cmd=f"{remote}/rawdiff {manifest['device_file']} /dev/block/sdc100 {remote}/baseline.sha256"
  b=run(['exec-out','su','-c',cmd],1800)
  if b'rawdiff complete: 17179869184 bytes, 65536 groups,' not in b:raise ValueError('scan did not complete')
  with out.open('xb') as f:f.write(b)
  print('read-only scan saved:',out)
 finally:
  for dest in created:
   try:run(['shell','rm','-f',dest])
   except Exception:pass
  try:run(['shell','rmdir',remote])
  except Exception:pass
if __name__=='__main__':main()
