#!/usr/bin/env python3
"""Materialize a prepared full HFS root ONLY in a newly-created pinned file.
Explicit data-a-live workflow: no repartition, flash, reboot or unrelated writes.
Keeps old bootstrap file. Every raw write goes through upstream pinfile checks.
"""
from pathlib import Path
import subprocess
import json
import hashlib
import struct
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[3]/'x9/tools'))
from ufs_manifest import gpt,extents

OUT=Path(__file__).resolve().parent
ROOT=OUT.parents[2]
SERIAL='X9_SERIAL_REQUIRED'
REMOTE='/data/local/tmp/x9-ios-full-20261026-222518'
FILE=REMOTE+'/iosroot-full.hfs'
BASE=['adb','-s',SERIAL]

def command(name,cmd,timeout=60):
    print('DEVICE',name,flush=True)
    p=subprocess.run(BASE+['exec-out','su','-c',cmd],capture_output=True,timeout=timeout)
    (OUT/(name+'.stdout')).write_bytes(p.stdout);(OUT/(name+'.stderr')).write_bytes(p.stderr)
    if p.returncode:raise RuntimeError(name+' failed: '+p.stdout.decode(errors='replace')[-1000:]+p.stderr.decode(errors='replace')[-1000:])
    return p.stdout


def main():
    source=json.loads((OUT/'full-root.json').read_text());image=Path(source['image'])
    if image.stat().st_size!=source['volume_bytes'] or source['volume_bytes']!=16*1024**3:raise ValueError('unexpected full image geometry')
    if command('device-id','getprop ro.serialno').decode().strip()!=SERIAL:raise ValueError('wrong device')
    command('install-helper','umask 077; mkdir '+REMOTE+' && cp /data/local/tmp/x9-iosroot-20261026-202513/pinfile '+REMOTE+'/pinfile && chmod 700 '+REMOTE+'/pinfile')
    command('create-full-file',REMOTE+'/pinfile create '+FILE+' 16384',300)
    command('pin-status',REMOTE+'/pinfile status '+FILE)
    table=json.loads(command('full-extents',REMOTE+'/pinfile extents '+FILE+' --json'))
    rawgpt=command('current-gpt','dd if=/dev/block/sdc bs=4096 count=8 status=none')
    if len(rawgpt)!=32768:raise ValueError('bad GPT capture')
    part=next(p for p in gpt(rawgpt)['partitions'] if p['name']=='userdata');validated=extents(table,part['blocks'])
    (OUT/'full-extents.json').write_text(json.dumps(table,indent=2)+'\n')
    command('protect-full-file','chattr +i '+FILE)
    before=command('first-block-backup',REMOTE+'/pinfile rawreadat '+FILE+' /dev/block/sdc100 0 4096 2>'+REMOTE+'/rawread-diagnostics.txt')
    if len(before)!=4096:raise ValueError('invalid initial raw block backup')
    canary=b'X9 FULL ROOT CANARY v1\0'+bytes(4096-len(b'X9 FULL ROOT CANARY v1\0'))
    (OUT/'canary.bin').write_bytes(canary)
    uploaded=subprocess.run(BASE+['push',str(OUT/'canary.bin'),REMOTE+'.canary-upload'],capture_output=True,timeout=30)
    if uploaded.returncode:raise RuntimeError('canary upload failed')
    command('move-canary','mv '+REMOTE+'.canary-upload '+REMOTE+'/canary.bin')
    old=hashlib.sha256(before).hexdigest()
    report=command('full-canary',REMOTE+'/pinfile rawwriteat '+FILE+' /dev/block/sdc100 0 '+old+' --yes < '+REMOTE+'/canary.bin').decode()
    if hashlib.sha256(canary).hexdigest() not in report:raise ValueError('canary readback mismatch')
    # O_DIRECT file read avoids stale encrypted-zero cache, reads only the first
    # two dedicated test blocks, and never touches unrelated Android files.
    check=command('filesystem-canary','dd if='+FILE+' bs=4096 count=2 iflag=direct status=none',30)
    if len(check)!=8192 or not any(check[:4096]) or any(check[4096:]):raise ValueError('raw/file correspondence not proven')
    print('UPLOAD full image with zstd; old 64MiB root untouched',flush=True)
    with (OUT/'full-adb-upload.log').open('xb') as log:
        p=subprocess.run(BASE+['push','-z','zstd',str(image),REMOTE+'.input-upload'],stdout=log,stderr=subprocess.STDOUT,timeout=3600)
    if p.returncode:raise RuntimeError('full image upload failed')
    command('move-upload','mv '+REMOTE+'.input-upload '+REMOTE+'/input.hfs && chmod 600 '+REMOTE+'/input.hfs')
    copied=command('upload-sha256','sha256sum '+REMOTE+'/input.hfs',600).decode()
    if copied.split()[0]!=source['sha256']:raise ValueError('uploaded source hash mismatch')
    if json.loads(command('extents-before-write',REMOTE+'/pinfile extents '+FILE+' --json'))!=table:raise ValueError('extent map changed')
    report=command('write-full-image',REMOTE+'/pinfile rawwrite '+FILE+' /dev/block/sdc100 all --yes < '+REMOTE+'/input.hfs',1800).decode()
    if source['sha256'] not in report or 'wrote 4194304 blocks (17179869184 bytes)' not in report:raise ValueError('full raw write mismatch')
    report=command('readback-full-sha256',REMOTE+'/pinfile rawhash '+FILE+' /dev/block/sdc100',1800).decode()
    if not any(line.startswith(source['sha256']+'  ') for line in report.splitlines()):raise ValueError('full raw readback mismatch')
    if json.loads(command('extents-after-write',REMOTE+'/pinfile extents '+FILE+' --json'))!=table:raise ValueError('extent map changed after write')
    status=command('final-file-status','stat -c "%i %s" '+FILE+'; lsattr '+FILE+'; '+REMOTE+'/pinfile status '+FILE).decode()
    if ': pinned (GC move failures 0)' not in status or 'i' not in status.splitlines()[1].split()[0]:raise ValueError('pin or immutable lost')
    with image.open('rb') as stream:header=stream.read(65536)
    if header[1024:1026]!=b'HX':raise ValueError('expected complete HFSX source')
    manifest={'schema':'x9-plaintext-root-v1','runtime':'native-ufs-root','device_serial':SERIAL,'device_file':FILE,
              'inode':int(status.split()[0]),'bytes':source['volume_bytes'],'sha256':source['sha256'],'lun':2,
              'partition_first':part['first_lba'],'partition_blocks':part['blocks'],'partition_guid_hex':part['unique_guid_hex'],
              'extents':validated['extents'],'android_extents':table,'immutable':True,'raw_readback_verified':True,
              'header_bytes':65536,'header_sha256':hashlib.sha256(header).hexdigest(),'volume_uuid_hex':header[1128:1136].hex(),
              'root_kind':'complete OS + SystemOS + AppOS original files; raw HFSX, original executable bytes unchanged',
              'source_files_verified':source['source_files_verified'],'boot_mode':'native XNU ufsroot0 on-demand, no whole-image RAM copy',
              'source_image':str(image)}
    (OUT/'native-root-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    command('remove-own-upload','rm '+REMOTE+'/input.hfs')
    print('FULL ROOT STORED AND VERIFIED',json.dumps({'file':FILE,'bytes':manifest['bytes'],'extents':len(manifest['extents']),'sha256':manifest['sha256']}),flush=True)


if __name__=='__main__':main()
