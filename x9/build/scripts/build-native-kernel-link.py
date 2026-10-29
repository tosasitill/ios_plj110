#!/usr/bin/env python3
"""Normal compile/link of native renderer into a separate frozen XNU kernel.
No boot image, device access, tests, disassembly or post-link artifact checks.
"""
from pathlib import Path
import json,subprocess,sys,os
w=Path(__file__).resolve().parent;root=w.parents[1]
base=root/'x9/out/pan168-desktop-workgroup-xy'
for script in ['build-render-kernel.py','build-resident-kernel.py','build-render-display.py']:
 subprocess.run([sys.executable,str(w/script)],check=True)
out=w/'native-kernel';out.mkdir(exist_ok=True)
replacements={
 'SweetX9CSF.cpo':w/'resident-kernel-inputs/SweetX9CSF.o',
 'sweet_x9_gpu_dependencies.o':w/'resident-kernel-inputs/sweet_x9_gpu_dependencies.o',
 'SweetSurface.cpo':w/'render-display-inputs/SweetSurface.o',
 'SweetFramebuffer.cpo':w/'render-display-inputs/SweetFramebuffer.o',
}
entries=(base/'kernel-link.filelist').read_text().splitlines();seen=set();linked=[]
for entry in entries:
 name=Path(entry).name
 if name in replacements:seen.add(name);entry=str(replacements[name])
 linked.append(entry)
if seen!=set(replacements):raise RuntimeError('frozen replacement object chain incomplete')
for name in ['SweetX9RenderService','X9RenderPageTable','X9RenderMemory','X9CSFInterfaces','X9CSFRenderQueue','X9CSFQueueAccess']:
 linked.append(str(w/'render-kernel-objects'/(name+'.o')))
filelist=out/'kernel-link.filelist';filelist.write_text('\n'.join(linked)+'\n')
steps=json.loads((base/'commands.json').read_text());link=next(s for s in steps if '-filelist' in s['args'])
args=list(link['args']);args[0]='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++'
args[args.index('-filelist')+1]=str(filelist)
args[args.index('-o')+1]=str(out/'kernel.development.sweet.unstripped')
strip=next(s for s in steps if s['args'][0].endswith('/strip'))
strip_args=list(strip['args']);strip_args[strip_args.index('-S')+1]=str(out/'kernel.development.sweet.unstripped');strip_args[strip_args.index('-o')+1]=str(out/'kernel.development.sweet')
records=[{'args':args,'cwd':link['cwd']},{'args':strip_args,'cwd':strip['cwd']}]
(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
env=os.environ.copy();env['DEVELOPER_DIR']='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer'
for record in records:subprocess.run(record['args'],cwd=record['cwd'],env=env,check=True)
print('Native renderer service/engine/display kernel compiled and linked. x9_metal_render=1 is opt-in; no boot packaging or execution.',flush=True)
