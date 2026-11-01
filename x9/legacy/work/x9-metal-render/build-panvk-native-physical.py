#!/usr/bin/env python3
"""Compile guarded native instance/physical-device enumeration objects."""
from pathlib import Path
import json,shlex,subprocess
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render';root=w.parents[1]
subprocess.run(['python3',str(w/'prepare-panvk-native-physical.py')],check=True)
entries=json.loads((w/'build-panvk-ios/compile_commands.json').read_text())
out=w/'render-user-objects';out.mkdir(exist_ok=True)
commands=[]
for name in ['panvk_instance.c','panvk_physical_device.c','panvk_vX_physical_device.c']:
 entry=next(e for e in entries if e['file'].endswith('/'+name) and ('vX' not in name or '-DPAN_ARCH=14' in e['command']))
 args=shlex.split(entry['command']);obj=out/(name.replace('vX','v14')+'.native.o')
 for flag,suffix in [('-o',''),('-MQ',''),('-MF','.d')]:args[args.index(flag)+1]=str(obj)+suffix
 args[1:1]=['-DX9_NATIVE_KMOD=1','-I'+str(root/'MaliMetal/X9/General/panvk')]
 commands.append({'args':args,'cwd':entry['directory']})
(out/'panvk-native-physical-commands.json').write_text(json.dumps(commands,indent=2)+'\n')
for c in commands:subprocess.run(c['args'],cwd=c['cwd'],check=True)
print('Native instance/physical-device objects compiled; full native loader and hardware rendering remain unverified.',flush=True)
