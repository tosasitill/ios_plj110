#!/usr/bin/env python3
"""Prepare an untracked private shim header from TWO effective XTS keys.
This does not recover keys, derive fscrypt master keys, or contact Android.
Input: raw 64-byte metadata XTS key and raw 64-byte fscrypt effective mode key
for AES-256-XTS/IV_INO_LBLK_64. Policies/nonces/wrapped blobs are not keys.
Never print key bytes. Destination is exclusive and mode 0600.
"""
import argparse
import os
from pathlib import Path


def read_key(path):
    value=path.read_bytes()
    if len(value)!=64 or value[:32]==value[32:]:
        raise ValueError('requires a raw 64-byte AES-256-XTS effective key with distinct halves')
    return value


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--metadata-key',required=True,type=Path)
    p.add_argument('--file-effective-key',required=True,type=Path)
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parents[1]/'shim/ufs/ufs_keys_private.h')
    a=p.parse_args()
    keys=[('x9_private_metadata_xts_key',read_key(a.metadata_key)),('x9_private_file_xts_key',read_key(a.file_effective_key))]
    lines=['/* PRIVATE device keys. Contains secrets; keep header and built image private. */','#pragma once','#include <stdint.h>']
    for name,key in keys:
        lines.append('static const uint8_t '+name+'[64]={'+','.join('0x%02x'%b for b in key)+'};')
    fd=os.open(a.out,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'w') as stream:stream.write('\n'.join(lines)+'\n')
    print('Private effective-key header prepared; no key bytes logged.')


if __name__=='__main__':main()
