#!/usr/bin/env python3
"""Compile native renderer objects using the frozen pan168 XNU environment.
Does not link a candidate, run tests, or access a device.
"""
from pathlib import Path
import json, subprocess
w=Path(__file__).resolve().parent
root=w.parents[1]
sources=root/'MaliMetal/X9/General'
commands=json.loads((root/'x9/out/pan168-desktop-workgroup-xy/commands.json').read_text())
base=next(c for c in commands if any(a.endswith('/SweetX9CSF.cpp') for a in c['args']))
out=w/'render-kernel-objects';out.mkdir(exist_ok=True)
records=[]
for name in ['SweetX9RenderService','X9RenderPageTable','X9RenderMemory','X9CSFInterfaces','X9CSFRenderQueue','X9CSFQueueAccess']:
 args=list(base['args'])
 args[0]='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++'
 for flag,suffix in [('-o','.o'),('-MF','.d')]:
  args[args.index(flag)+1]=str(out/(name+suffix))
 args=[str(sources/'kernel'/(name+'.cpp')) if a.endswith('/SweetX9CSF.cpp') else a for a in args]
 args[1:1]=['-I'+str(sources),'-I'+str(sources/'kernel')]
 records.append({'args':args,'cwd':base['cwd']})
(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
for record in records:
 subprocess.run(record['args'],cwd=record['cwd'],check=True)
print('Native render service, GPU page tables and memory/cache and CSF interface objects compiled; hardware engine not linked.',flush=True)
