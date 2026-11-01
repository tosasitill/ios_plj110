#!/usr/bin/env python3
"""Compile native logical-device and dependent v14 command-generation objects."""
from pathlib import Path
import json,shlex,subprocess
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
root=w.parents[1]
subprocess.run(['python3',str(w/'prepare-panvk-native-device.py')],check=True)
entries=json.loads((w/'build-panvk-ios/compile_commands.json').read_text())
files=['panvk_vX_device.c','panvk_vX_cmd_buffer.c','panvk_vX_exception_handler.c','panvk_vX_utrace.c','panvk_vX_cmd_draw.c']
out=w/'render-user-objects';out.mkdir(exist_ok=True)
commands=[]
for name in files:
 entry=next(e for e in entries if e['file'].endswith('/'+name) and '-DPAN_ARCH=14' in e['command'])
 args=shlex.split(entry['command']);obj=out/(name.replace('vX','v14_native')+'.o')
 for flag,suffix in [('-o',''),('-MQ',''),('-MF','.d')]:args[args.index(flag)+1]=str(obj)+suffix
 args[1:1]=['-DX9_NATIVE_KMOD=1','-I'+str(root/'MaliMetal/X9/General/panvk')]
 commands.append({'args':args,'cwd':entry['directory']})
(out/'panvk-native-device-commands.json').write_text(json.dumps(commands,indent=2)+'\n')
for command in commands:subprocess.run(command['args'],cwd=command['cwd'],check=True)
print('Native logical-device and four v14 command-generation objects compiled; physical discovery and kernel engine not integrated.',flush=True)
