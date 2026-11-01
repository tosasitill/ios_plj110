#!/usr/bin/env python3
"""Normal build preparation for the X9 AIR/NIR/v14 compiler. Never runs probes."""
from pathlib import Path
import json, subprocess, tarfile, urllib.request, shutil
work=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
root=work.parents[1]
commit='300b5fd7b44120b80607d1c90b40bb126e8d3ec8'
archive=work/f'mesa-{commit}.tar.gz'
source=work/'mesa-source'
if not (source/'src/compiler/meson.build').exists():
 if not archive.exists():
  print('Downloading pinned complete Mesa source',flush=True)
  urllib.request.urlretrieve(f'https://codeload.github.com/chaotic-cx/mesa-mirror/tar.gz/{commit}',archive)
 with tarfile.open(archive) as t:
  t.extractall(work,filter='data')
 unpacked=work/f'mesa-mirror-{commit}'
 if source.exists():
  if any(source.iterdir()): raise RuntimeError('incomplete source tree; preserve before retry')
  source.rmdir()
 unpacked.rename(source)
 print('Complete pinned Mesa source prepared',flush=True)
venv=work/'build-env'
if not venv.exists():subprocess.run(['python3','-m','venv',str(venv)],check=True)
subprocess.run([str(venv/'bin/python'),'-m','pip','install','meson','mako','pyyaml','packaging','ply','pycparser'],check=True)
(work/'reference-inputs.json').write_text(json.dumps({'mesa_commit':commit,'air_frontend_commit':'c84b4634277a70beac73921029b1fb6afc2530af','runtime_status':'NOT RUN','purpose':'AIR to NIR to Mali v14 compiler; build preparation only'},indent=2)+'\n')
print('Compiler build dependencies ready',flush=True)
