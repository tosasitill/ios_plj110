#!/usr/bin/env python3
"""Decode X9UFSLOG sidecar; optional read-only ADB capture from its pinned file.
CRC covers the header/cursors only. Payload is retained without repair.
"""
import argparse,binascii,json,struct,subprocess
from pathlib import Path

def decode(data):
 if len(data)<4096 or data[:8]!=b'X9UFSLOG':raise ValueError('no committed X9 disk-log header')
 version,hsize,capacity,first,total,seq,crc=struct.unpack_from('<IIQQQQI',data,8)
 if version!=1 or hsize!=4096 or capacity!=len(data)-4096 or total<first or total-first>capacity:
  raise ValueError('invalid disk-log header/range')
 if binascii.crc32(data[:48])!=crc:raise ValueError('disk-log header CRC mismatch')
 count=total-first;offset=first%capacity;take=min(count,capacity-offset)
 text=data[4096+offset:4096+offset+take]+data[4096:4096+count-take]
 return text,{'version':version,'ring_bytes':capacity,'first':first,'total':total,'sequence':seq,'header_crc_valid':True,'payload_crc':'not provided; no repair attempted'}

def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--image',type=Path);ap.add_argument('--manifest',type=Path);ap.add_argument('--serial',required=True,help='adb serial of the test device');ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
 if bool(a.image)==bool(a.manifest):ap.error('select one of --image or --manifest')
 if a.manifest:
  import shlex
  m=json.loads(a.manifest.read_text());policy=m['log_policy'];path=policy['device_file'];tool=str(Path(path).parent/'pinfile');q=shlex.quote
  def read(cmd):return subprocess.check_output(['adb','-s',a.serial,'exec-out','su','-c',cmd],timeout=40)
  if read('getprop ro.serialno').decode().strip()!=m['device_serial']:raise ValueError('wrong device')
  if list(map(int,read('stat -c "%i %s" '+q(path)).split()))!=[policy['inode'],policy['bytes']]:raise ValueError('log inode/size changed')
  if 'i' not in read('lsattr '+q(path)).decode().split()[0] or b'pinned (GC move failures 0)' not in read(q(tool)+' status '+q(path)):raise ValueError('log reservation unprotected')
  if json.loads(read(q(tool)+' extents '+q(path)+' --json'))!=policy['android_extents']:raise ValueError('log map changed')
  data=read(q(tool)+' rawread '+q(path)+' /dev/block/sdc100 '+str(policy['bytes'])+' 2>/dev/null')
  if len(data)!=policy['bytes']:raise ValueError('incomplete raw log capture')
 else:data=a.image.read_bytes()
 raw=Path(str(a.out)+'.raw');raw.parent.mkdir(parents=True,exist_ok=True)
 with raw.open('xb') as f:f.write(data)
 payload,meta=decode(data)
 with Path(str(a.out)+'.payload.raw').open('xb') as f:f.write(payload)
 with Path(str(a.out)+'.log').open('x') as f:f.write(payload.decode('utf-8','replace'))
 with Path(str(a.out)+'.json').open('x') as f:json.dump(meta,f,indent=2);f.write('\n')
 print(json.dumps(meta,indent=2))
if __name__=='__main__':main()
