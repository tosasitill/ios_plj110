#!/usr/bin/env python3
"""Stage a built X9 plugin package into the fixed pinned mailbox from Android.
Never writes HFS metadata or boot; commit header is published last.
"""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
P=Path(__file__).resolve().parent;ROOT=P.parents[1];SERIAL=os.environ.get('X9_SERIAL','');TOOL='/data/local/tmp/x9-ios-pan92-20261027-1215/pinfile';DEV='/dev/block/sdc100';B=4096

def adb(c,data=None):
 p=subprocess.run(['adb','-s',SERIAL]+(['exec-out'] if data is None else ['shell','-T'])+['su','-c',c],input=data,capture_output=True,timeout=120)
 if p.returncode:raise RuntimeError((p.stdout+p.stderr)[-1500:].decode(errors='replace'))
 return p.stdout

def main():
 global SERIAL
 ap=argparse.ArgumentParser();ap.add_argument('candidate',type=Path);ap.add_argument('--serial',default=SERIAL)
 ap.add_argument('--root-manifest',type=Path,help='device root-manifest.json (default: root_manifest in x9/build/local.json)')
 ap.add_argument('--root-check',type=Path,help='custom root verification script instead of x9/tools/check_plaintext_root.py')
 a=ap.parse_args()
 SERIAL=a.serial
 if not SERIAL:raise SystemExit('pass --serial or set X9_SERIAL')
 # The root preflight is never skipped: check_plaintext_root.py verifies the
 # device serial, pin+immutable, inode, FIEMAP, userdata geometry, dm route and
 # the live HFS UUID/geometry against the private root manifest.
 manifest=a.root_manifest
 if manifest is None:
  local=ROOT/'x9/build/local.json'
  cfg=json.loads(local.read_text()) if local.is_file() else {}
  manifest=Path(cfg['root_manifest']).expanduser() if cfg.get('root_manifest') else None
  if manifest is not None and not manifest.is_absolute():manifest=ROOT/manifest
 if a.root_check is None and manifest is None:raise SystemExit('pass --root-manifest (or set root_manifest in x9/build/local.json) or --root-check')
 global TOOL
 if manifest is not None:
  rm=json.loads(manifest.read_text())
  # pinfile sits next to the root image on the device
  TOOL=str(Path(rm['device_file']).parent/'pinfile')
 pkg=a.candidate/'plugin-package'
 desc=json.loads((pkg/'mailbox.json').read_text());m=json.loads((P/'mailbox-manifest.json').read_text())
 if a.root_check is not None:subprocess.run(['/bin/bash',str(a.root_check.resolve())],check=True)
 else:subprocess.run(['python3','-I',str(ROOT/'x9/tools/check_plaintext_root.py'),'--manifest',str(manifest),'--serial',SERIAL],check=True)
 for f in m['files']:
  path=f['file'];status=adb(f'{TOOL} status {path}; lsattr {path}; stat -c "%i %s" {path}').decode()
  if ': pinned (GC move failures 0)' not in status or 'i' not in status.splitlines()[1].split()[0] or status.splitlines()[-1]!=f"{f['inode']} {f['bytes']}":raise ValueError('mailbox identity or protection changed: '+path)
  actual=json.loads(adb(f'{TOOL} extents {path} --json'))
  if actual!=f['android_extents']:raise ValueError('mailbox extent map changed: '+path)
 if desc['payload_end']>m['logical_blocks']*B or desc['payload_end']%B:raise ValueError('payload size')
 packet=bytearray(desc['payload_end']);packet[:8]=b'X9UPD001';packet[8:40]=bytes.fromhex(desc['identity'])
 for x in desc['files']:
  data=(pkg/x['source']).read_bytes();off=x['offset']
  if len(data)!=x['bytes'] or hashlib.sha256(data).hexdigest()!=x['sha256'] or off<B or off% B or off+len(data)>len(packet):raise ValueError('package input mismatch')
  packet[off:off+len(data)]=data
 def write_at(logical,data):
  if logical%B or len(data)%B:raise ValueError('unaligned write')
  left=len(data);idx=0
  while left:
   row=next((x for x in m['map'] if x['logical_block']*B<=logical<(x['logical_block']+x['blocks'])*B),None)
   if row is None:raise ValueError('unmapped mailbox offset')
   within=logical-row['logical_block']*B;off=row['file_block']*B+within;n=min(left,row['blocks']*B-within,256*1024);path=row['file'];chunk=data[idx:idx+n]
   old=adb(f'{TOOL} rawreadat {path} {DEV} {off} {n} 2>/dev/null')
   if len(old)!=n:raise RuntimeError('short mailbox read')
   if old!=chunk:
    out=adb(f'{TOOL} rawwriteat {path} {DEV} {off} {hashlib.sha256(old).hexdigest()} --yes',chunk).decode()
    if f'wrote {n} bytes' not in out or 'sha256 read back: '+hashlib.sha256(chunk).hexdigest() not in out:raise RuntimeError('write readback failed')
   logical+=n;idx+=n;left-=n
 # Never leave a valid commit marker while replacing the payload.
 write_at(0,bytes(B));print('Mailbox marker invalidated',flush=True)
 for off in range(B,len(packet),1024*1024):
  write_at(off,packet[off:min(off+1024*1024,len(packet))])
  if off%(8*1024*1024)==B:print('Staged',off,'/',len(packet),'bytes',flush=True)
 write_at(0,packet[:B]);print('Mailbox committed:',len(packet),'bytes; each written range read back',flush=True)

if __name__=='__main__':main()
