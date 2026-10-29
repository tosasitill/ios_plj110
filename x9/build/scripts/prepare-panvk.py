#!/usr/bin/env python3
"""Prepare separate PanVK port sources and host compiler dependencies."""
from pathlib import Path
import tarfile,urllib.request,hashlib
w=Path(__file__).resolve().parent
commit='300b5fd7b44120b80607d1c90b40bb126e8d3ec8'
source=w/'mesa-render-source'
if not source.exists():
 staging=w/'render-source-extract';staging.mkdir(exist_ok=True)
 with tarfile.open(w/f'mesa-{commit}.tar.gz') as t:t.extractall(staging,filter='data')
 (staging/f'mesa-mirror-{commit}').rename(source);staging.rmdir()
archive=w/'libdrm-2.4.133.tar.xz'
if not archive.exists():
 with urllib.request.urlopen('https://dri.freedesktop.org/libdrm/libdrm-2.4.133.tar.xz',timeout=40) as r,archive.open('wb') as f:
  while block:=r.read(1<<20):f.write(block)
if hashlib.sha256(archive.read_bytes()).hexdigest()!='fc68f9d0ba2ea63c9432a299e14fea09fad7a8a66e8039fcd7802ca59f77b4f5':raise RuntimeError('libdrm source identity differs from Mesa wrap pin')
if not (w/'libdrm-2.4.133').exists():
 with tarfile.open(archive) as t:t.extractall(w,filter='data')
print('Separate pinned PanVK source and libdrm headers ready',flush=True)
