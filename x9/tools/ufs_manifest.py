#!/usr/bin/env python3
"""Validate the captured X9 GPT and optionally a pinned-file extent manifest.
Offline only: no adb, block-device open, file allocation, flashing or writes.
Output is an assessment input, never authorization to use a stale extent map.
"""
import argparse
import json
from pathlib import Path
import struct
import zlib


def gpt(data, block=4096):
    header=data[block:2*block]
    if len(header)!=block or header[:8]!=b'EFI PART':
        raise ValueError('missing primary GPT')
    revision,size,crc,reserved=struct.unpack_from('<4I',header,8)
    current,backup,first,last=struct.unpack_from('<4Q',header,24)
    if revision!=0x10000 or not 92<=size<=block or reserved or current!=1 or first>last or last>=backup:
        raise ValueError('invalid GPT geometry')
    raw=bytearray(header[:size]);raw[16:20]=bytes(4)
    if zlib.crc32(raw)!=crc:raise ValueError('GPT header CRC mismatch')
    table,count,entry,table_crc=struct.unpack_from('<QIII',header,72)
    if not count or count>4096 or entry<128 or entry>block or entry&(entry-1) or block%entry:
        raise ValueError('invalid GPT entry geometry')
    raw=data[table*block:table*block+count*entry]
    if len(raw)!=count*entry or zlib.crc32(raw)!=table_crc:
        raise ValueError('GPT entry CRC mismatch')
    partitions=[]
    for i in range(count):
        p=raw[i*entry:(i+1)*entry]
        if not any(p[:16]):continue
        start,end=struct.unpack_from('<QQ',p,32)
        if not first<=start<=end<=last:raise ValueError('partition outside usable range')
        name=p[56:128].decode('utf-16le').rstrip('\0')
        partitions.append({'index':i+1,'name':name,'first_lba':start,'blocks':end-start+1,
                           'bytes':(end-start+1)*block,'unique_guid_hex':p[16:32].hex()})
    ordered=sorted(partitions,key=lambda p:p['first_lba'])
    for a,b in zip(ordered,ordered[1:]):
        if a['first_lba']+a['blocks']>b['first_lba']:
            raise ValueError('GPT partitions overlap')
    return {'block_bytes':block,'device_blocks':backup+1,'disk_guid_hex':header[56:72].hex(),
            'header_crc32':f'{crc:08x}','entries_crc32':f'{table_crc:08x}','partitions':partitions}


def extents(table, partition_blocks):
    size=table['size'];runs=table['extents']
    if not isinstance(size,int) or size<=0 or size%4096 or not 0<len(runs)<=16384:
        raise ValueError('invalid image size or extent count')
    result=[];next_byte=0
    for logical,physical,length,flags in runs:
        if any(type(v)!=int or v<0 for v in [logical,physical,length,flags]):
            raise ValueError('invalid extent scalar')
        allowed=0x1001|0x80|(0x8 if flags&0x80 else 0)
        if flags&~allowed or logical!=next_byte or not length or (logical|physical|length)&4095:
            raise ValueError('holes, unstable flags or unaligned extents')
        if physical//4096>=partition_blocks or length//4096>partition_blocks-physical//4096:
            raise ValueError('extent exceeds userdata')
        result.append({'file_block':logical//4096,'partition_block':physical//4096,'blocks':length//4096})
        next_byte+=length
    if next_byte!=size:raise ValueError('extents must cover exactly the image')
    ordered=sorted(result,key=lambda e:e['partition_block'])
    for a,b in zip(ordered,ordered[1:]):
        if a['partition_block']+a['blocks']>b['partition_block']:
            raise ValueError('physical extents overlap')
    return {'image_bytes':size,'block_bytes':4096,'extents':result,
            'requires_live_pin_and_mapping_validation':True,
            'requires_raw_plaintext_identity_verification':True,
            'device_write_authorized':False}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('gpt_capture',type=Path)
    parser.add_argument('--extents',type=Path)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    result=gpt(args.gpt_capture.read_bytes())
    partitions=[p for p in result['partitions'] if p['name']=='userdata']
    if len(partitions)!=1:raise ValueError('expected exactly one userdata partition')
    result['userdata']=partitions[0];result['source']=str(args.gpt_capture.resolve())
    if args.extents:result['pinned_image']=extents(json.loads(args.extents.read_text()),partitions[0]['blocks'])
    args.out.parent.mkdir(parents=True,exist_ok=True)
    if args.out.exists():raise ValueError('refusing to overwrite a captured assessment')
    args.out.write_text(json.dumps(result,indent=2)+'\n')
    print('Validated GPT:',len(result['partitions']),'partitions; userdata',partitions[0]['bytes'],'bytes')


if __name__=='__main__':main()
