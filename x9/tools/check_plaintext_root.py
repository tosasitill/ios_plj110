#!/usr/bin/env python3
"""Read-only Android preflight for an existing plaintext pinned-file root.
No pin changes, file allocation, raw writes, partition writes, flash or reboot.
Checks current mapping, pin, immutable, inode, extents and raw-image SHA256.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from ufs_root_manifest import load


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--manifest',type=Path,required=True)
    ap.add_argument('--serial')
    args=ap.parse_args();raw=json.loads(args.manifest.read_text());m=load(args.manifest,native=raw.get('runtime')=='native-ufs-root')
    serial=args.serial
    if not serial:
        p=subprocess.run(['adb','devices'],check=True,capture_output=True,text=True)
        devices=[line.split()[0] for line in p.stdout.splitlines()[1:] if len(line.split())>=2 and line.split()[1]=='device']
        if len(devices)!=1:raise ValueError('expected one adb device; select --serial')
        serial=devices[0]
    def read(command,timeout=15):
        p=subprocess.run(['adb','-s',serial,'exec-out','su','-c',command],check=True,capture_output=True,timeout=timeout)
        return p.stdout.decode()
    if read('getprop ro.serialno').strip()!=m['device_serial']:raise ValueError('wrong device serial')
    path=m['device_file'];tool=str(Path(path).parent/'pinfile');quote=shlex.quote
    stat=read('stat -c "%i %s" '+quote(path)).split()
    if [int(x) for x in stat]!=[m['inode'],m['bytes']]:raise ValueError('inode or size changed')
    flags=read('lsattr '+quote(path)).split()[0]
    if 'i' not in flags:raise ValueError('immutable flag missing')
    if ': pinned (GC move failures 0)' not in read(quote(tool)+' status '+quote(path)):
        raise ValueError('file is no longer pinned or has GC move failures')
    table=json.loads(read(quote(tool)+' extents '+quote(path)+' --json'))
    if table!=m['android_extents']:raise ValueError('FIEMAP changed; stale manifest rejected')
    start=int(read('cat /sys/class/block/sdc100/start').strip());size=int(read('cat /sys/class/block/sdc100/size').strip())
    if start!=m['partition_first']*8 or size!=m['partition_blocks']*8:raise ValueError('userdata geometry changed')
    # Parse only required table fields; never log a raw key field.
    raw=read('dmctl table userdata');tokens=raw.split()
    try:i=next(i for i,x in enumerate(tokens) if x.rstrip(',')=='default-key')
    except StopIteration:raise ValueError('unexpected dm target')
    dev=read('cat /sys/class/block/sdc100/dev').strip()
    if tokens[i+1]!='aes-xts-plain64' or tokens[i+3]!='0' or tokens[i+4]!=dev or tokens[i+5]!='0':
        raise ValueError('default-key route or offsets changed')
    if m.get('log_policy'):
        log=m['log_policy'];logpath=log['device_file'];logtool=str(Path(logpath).parent/'pinfile')
        if list(map(int,read('stat -c "%i %s" '+quote(logpath)).split()))!=[log['inode'],log['bytes']]:
            raise ValueError('sidecar log inode/size changed')
        if 'i' not in read('lsattr '+quote(logpath)).split()[0] or ': pinned (GC move failures 0)' not in read(quote(logtool)+' status '+quote(logpath)):
            raise ValueError('sidecar log no longer pinned+immutable')
        if json.loads(read(quote(logtool)+' extents '+quote(logpath)+' --json'))!=log['android_extents']:
            raise ValueError('sidecar log map changed')
    if m.get('firmware_policy'):
        fw=m['firmware_policy'];fwpath=fw['device_file']
        if list(map(int,read('stat -c "%i %s" '+quote(fwpath)).split()))!=[fw['inode'],fw['bytes']]:
            raise ValueError('external firmware inode/size changed')
        if 'i' not in read('lsattr '+quote(fwpath)).split()[0] or ': pinned (GC move failures 0)' not in read(quote(tool)+' status '+quote(fwpath)):
            raise ValueError('external firmware no longer pinned+immutable')
        live=json.loads(read(quote(tool)+' extents '+quote(fwpath)+' --json'))
        if live!=fw['android_extents']:
            raise ValueError('external firmware FIEMAP changed')
        from ufs_manifest import extents
        if extents(live,m['partition_blocks'])['extents']!=fw['extents']:
            raise ValueError('external firmware raw map does not match FIEMAP')
        report=read(quote(tool)+' rawhash '+quote(fwpath)+' /dev/block/sdc100 '+str(fw['payload_bytes']),120)
        if not any(line.startswith(fw['sha256']+'  ') and line.endswith('first '+str(fw['payload_bytes'])+' bytes') for line in report.splitlines()):
            raise ValueError('external firmware raw payload hash mismatch')
        print('EXTERNAL BBFW PREFLIGHT VERIFIED: separate pinned+immutable file, live FIEMAP and full payload SHA256; read-only.')
    if m.get('write_policy'):
        # A writable HFS header changes at mount/sync; never compare against
        # the original full-image SHA after legitimate filesystem writes.
        toolcmd=quote(tool)+' rawreadat '+quote(path)+' /dev/block/sdc100 0 65536 2>/dev/null'
        p=subprocess.run(['adb','-s',serial,'exec-out','su','-c',toolcmd],check=True,capture_output=True,timeout=40)
        header=p.stdout
        import struct
        if len(header)!=65536 or header[1024:1026] not in (b'H+',b'HX'):
            raise ValueError('invalid current mutable HFS header')
        if header[1128:1136].hex()!=m['volume_uuid_hex'] or struct.unpack_from('>I',header,1064)[0]*struct.unpack_from('>I',header,1068)[0]!=m['bytes']:
            raise ValueError('mutable-root UUID or geometry changed')
        print('MUTABLE ROOT PREFLIGHT VERIFIED: device, pin+immutable, inode/map/partition, protected extents and current HFS UUID/geometry.')
        print('Original full-image SHA is provisioning provenance; XNU filesystem writes may change content. No device writes performed.')
        return
    # Upstream pinfile additionally checks single slave/size, extent bounds and
    # all direct device-mapper holders before issuing this O_DIRECT raw read.
    report=read(quote(tool)+' rawhash '+quote(path)+' /dev/block/sdc100',max(40,m['bytes']//(16*1024**2)+60))
    if not any(line.startswith(m['sha256']+'  ') for line in report.splitlines()):raise ValueError('raw root SHA256 mismatch')
    print('ROOT PREFLIGHT VERIFIED: correct device, pinned + immutable, inode/extents unchanged, raw SHA256 matches.')
    print('Root is a plaintext image in reserved file blocks. This does not flash or reboot the device.')


if __name__=='__main__':main()
