#!/usr/bin/env python3
"""Compile the actual Mesa v14 queue with native call-site integration enabled."""
from pathlib import Path
import json,shlex,subprocess
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
root=w.parents[1]
subprocess.run(['python3',str(w/'prepare-panvk-native-tiler.py')],check=True)
entries=json.loads((w/'build-panvk-ios/compile_commands.json').read_text())
entry=next(e for e in entries if e['file'].endswith('/csf/panvk_vX_gpu_queue.c') and '-DPAN_ARCH=14' in e['command'])
args=shlex.split(entry['command'])
out=w/'render-user-objects';out.mkdir(exist_ok=True)
obj=out/'panvk_v14_native_gpu_queue.o'
for flag,suffix in [('-o',''),('-MQ',''),('-MF','.d')]:args[args.index(flag)+1]=str(obj)+suffix
args[1:1]=['-DX9_NATIVE_KMOD=1','-I'+str(root/'MaliMetal/X9/General/panvk')]
(out/'panvk-native-queue-command.json').write_text(json.dumps({'args':args,'cwd':entry['directory']},indent=2)+'\n')
subprocess.run(args,cwd=entry['directory'],check=True)
print('Actual Mesa v14 queue compiled with native init/main group-submit calls; other Panthor paths and native engine not integrated.',flush=True)
