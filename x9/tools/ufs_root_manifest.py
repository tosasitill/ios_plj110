"""Validate and compile a read-only plaintext UFS root manifest into shim input."""
import json
from pathlib import Path


def validate(data, native=False):
    if data.get('schema')!='x9-plaintext-root-v1' or data.get('lun')!=2:
        raise ValueError('expected X9 plaintext root v1 on captured LUN2')
    size=data['bytes'];first=data['partition_first'];count=data['partition_blocks']
    limit=64*1024**3 if native else 0x8000000
    if type(size)!=int or not 0<size<=limit or size&0x3fff:
        raise ValueError('root must be 16KiB-aligned and at most 128MiB for legacy512 handoff')
    if type(first)!=int or type(count)!=int or first<=0 or count<=0 or first+count>2**64-1:
        raise ValueError('invalid userdata geometry')
    guid=bytes.fromhex(data['partition_guid_hex']);digest=bytes.fromhex(data['sha256'])
    if len(guid)!=16 or not any(guid) or len(digest)!=32:
        raise ValueError('invalid partition identity or SHA256')
    if native:
        if data.get('header_bytes')!=65536 or len(bytes.fromhex(data.get('header_sha256','')))!=32 or len(bytes.fromhex(data.get('volume_uuid_hex','')))!=8:
            raise ValueError('native root needs exact first64KiB hash and HFS volume UUID')
    runs=data['extents'];pos=0
    if not 0<len(runs)<=16384:raise ValueError('invalid extent count')
    for run in runs:
        f,p,n=(run[k] for k in ['file_block','partition_block','blocks'])
        if any(type(v)!=int for v in [f,p,n]) or f!=pos or p<0 or n<=0 or p>=count or n>count-p:
            raise ValueError('invalid extent bounds or gap')
        pos+=n
    if pos!=size//4096:raise ValueError('extent size mismatch')
    ordered=sorted(runs,key=lambda e:e['partition_block'])
    for a,b in zip(ordered,ordered[1:]):
        if a['partition_block']+a['blocks']>b['partition_block']:raise ValueError('overlapping physical extents')
    policy=data.get('write_policy')
    if policy is not None:
        if not native or policy.get('version')!=1 or policy.get('kind')!='dedicated-mutable-copy' or not policy.get('initial_content_verified'):
            raise ValueError('invalid dedicated writable-root contract')
        protected=policy.get('protected_extents',[])
        if not 1<=len(protected)<=32768:raise ValueError('writable root requires protected reservations')
        for e in protected:
            p,n=e['partition_block'],e['blocks']
            if type(p)!=int or type(n)!=int or p<0 or n<=0 or p>=count or n>count-p:
                raise ValueError('invalid protected reservation')
            for a in runs:
                if a['partition_block']<p+n and p<a['partition_block']+a['blocks']:
                    raise ValueError('writable map overlaps protected reservation')
    log=data.get('log_policy')
    if log is not None:
        if not native or not policy or log.get('version')!=1 or not log.get('immutable') or not log.get('pinned'):
            raise ValueError('invalid disk log contract')
        length=log.get('bytes',0);logruns=log.get('extents',[]);position=0
        if type(length)!=int or length<131072 or length>16777216 or length%4096 or not 1<=len(logruns)<=16:
            raise ValueError('invalid disk log size/extents')
        for a in logruns:
            f,p,n=(a[k] for k in ['file_block','partition_block','blocks'])
            if any(type(x)!=int for x in [f,p,n]) or f!=position or p<0 or n<=0 or p>=count or n>count-p:
                raise ValueError('invalid disk log extent')
            for b in runs+policy['protected_extents']:
                if p<b['partition_block']+b['blocks'] and b['partition_block']<p+n:
                    raise ValueError('disk log overlaps reserved root/extent')
            for b in logruns:
                if a is not b and p<b['partition_block']+b['blocks'] and b['partition_block']<p+n:
                    raise ValueError('disk log self-overlap')
            position+=n
        if position!=length//4096:raise ValueError('disk log map size mismatch')
    firmware=data.get('firmware_policy')
    if firmware is not None:
        if (not native or not policy or firmware.get('version')!=1 or
            firmware.get('kind')!='dedicated-readonly-bbfw' or
            firmware.get('pinned') is not True or firmware.get('immutable') is not True or
            firmware.get('initial_content_verified') is not True):
            raise ValueError('invalid external baseband firmware contract')
        length=firmware.get('bytes',0);payload=firmware.get('payload_bytes',0)
        if (type(length)!=int or not 4096<=length<=96*1024**2 or length%4096 or
            type(payload)!=int or not 16<=payload<=length or
            len(bytes.fromhex(firmware.get('sha256','')))!=32):
            raise ValueError('invalid external firmware size/hash')
        fwruns=firmware.get('extents',[]);position=0
        if not 1<=len(fwruns)<=256:raise ValueError('invalid external firmware extent count')
        reserved=runs+policy['protected_extents']+data.get('log_policy',{}).get('extents',[])
        for i,a in enumerate(fwruns):
            f,p,n=(a[k] for k in ['file_block','partition_block','blocks'])
            if any(type(x)!=int for x in [f,p,n]) or f!=position or p<0 or n<=0 or p>=count or n>count-p:
                raise ValueError('invalid external firmware extent')
            for b in reserved+fwruns[:i]:
                if p<b['partition_block']+b['blocks'] and b['partition_block']<p+n:
                    raise ValueError('external firmware overlaps root/plugin/log/reserved extent')
            position+=n
        if position!=length//4096:raise ValueError('external firmware map size mismatch')
    if policy and len(policy['protected_extents'])+len(data.get('log_policy',{}).get('extents',[]))+len(data.get('firmware_policy',{}).get('extents',[]))>32768:
        raise ValueError('combined protected extent count exceeds kernel limit')
    return data


def header(manifest, native=False):
    data=validate(manifest,native)
    lines=['/* Generated frozen root identity; no keys, no storage-write authority. */',
           '#pragma once','#include "ufs_root.h"',
           '#define X9_ROOT_BYTES %dUL'%data['bytes'],
           '#define X9_ROOT_MUTABLE %dU'%bool(data.get('write_policy')),
           '#define X9_ROOT_LUN %dU'%data['lun'],
           '#define X9_ROOT_PART_FIRST %dUL'%data['partition_first'],
           '#define X9_ROOT_PART_BLOCKS %dUL'%data['partition_blocks'],
           'static const struct x9u_extent x9_root_extents[]={']
    for e in data['extents']:lines.append(' {%dUL,%dUL,%dUL},'%(e['file_block'],e['partition_block'],e['blocks']))
    lines+=['};']
    for name,field in [('x9_root_guid','partition_guid_hex'),('x9_root_sha256','sha256')]:
        raw=bytes.fromhex(data[field]);lines.append('static const uint8_t %s[%d]={%s};'%(name,len(raw),','.join('0x%02x'%b for b in raw)))
    if native:
        for name,field in [('x9_root_header_sha256','header_sha256'),('x9_root_volume_uuid','volume_uuid_hex')]:
            raw=bytes.fromhex(data[field]);lines.append('static const uint8_t %s[%d]={%s};'%(name,len(raw),','.join('0x%02x'%b for b in raw)))
    return '\n'.join(lines)+'\n' 


def load(path, native=False):return validate(json.loads(Path(path).read_text()),native)

def add_native_adt(tree,manifest):
    import struct
    data=validate(manifest,True)
    chosen=next(n for n in tree.children if n.prop('name')==b'chosen\0')
    props=[('x9-ufs-native-ready',struct.pack('<6Q',1,0x16810000,0x5000,0x14900000,0x63,1)),
           ('x9-ufs-root-bytes',struct.pack('<Q',data['bytes'])),
           ('x9-ufs-root-partition',struct.pack('<2Q',data['partition_first'],data['partition_blocks'])),
           ('x9-ufs-root-guid',bytes.fromhex(data['partition_guid_hex'])),
           ('x9-ufs-root-sha256',bytes.fromhex(data['sha256'])),
           ('x9-ufs-root-header-sha256',bytes.fromhex(data['header_sha256'])),
           ('x9-ufs-root-header-bytes',struct.pack('<Q',65536)),
           ('x9-ufs-root-volume-uuid',bytes.fromhex(data['volume_uuid_hex'])),
           ('x9-ufs-root-extents',b''.join(struct.pack('<3Q',e['file_block'],e['partition_block'],e['blocks']) for e in data['extents']))]
    if data.get('write_policy'):
        props += [('x9-ufs-root-write-contract',struct.pack('<2Q',1,data['bytes'])),
                  ('x9-ufs-protected-extents',b''.join(struct.pack('<2Q',e['partition_block'],e['blocks']) for e in data['write_policy']['protected_extents']+data.get('log_policy',{}).get('extents',[])+data.get('firmware_policy',{}).get('extents',[])))]
    if data.get('log_policy'):
        log=data['log_policy']
        props += [('x9-ufs-log-bytes',struct.pack('<Q',log['bytes'])),
                  ('x9-ufs-log-extents',b''.join(struct.pack('<3Q',e['file_block'],e['partition_block'],e['blocks']) for e in log['extents']))]
    if data.get('firmware_policy'):
        firmware=data['firmware_policy']
        props += [('x9-ufs-bbfw-bytes',struct.pack('<Q',firmware['bytes'])),
                  ('x9-ufs-bbfw-payload-bytes',struct.pack('<Q',firmware['payload_bytes'])),
                  ('x9-ufs-bbfw-extents',b''.join(struct.pack('<3Q',e['file_block'],e['partition_block'],e['blocks']) for e in firmware['extents']))]
    for name,value in props:chosen.props.append((name,0,value))
    return tree

