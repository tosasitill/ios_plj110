#!/usr/bin/env python3
"""Freeze/link/package pan169 from current native renderer objects, offline."""
from pathlib import Path
import json,os,shutil,subprocess,sys,shlex
w=Path(__file__).resolve().parent;root=w.parents[1]
base=root/'x9/out/pan168-desktop-workgroup-xy';out=root/'x9/out/pan169-native-render-cache'
out.mkdir(exist_ok=False)
for name in ['kernel-source','packaging-source','generated','firmware','compute-source']:
 shutil.copytree(base/name,out/name)
shutil.copyfile(base/'root-manifest.json',out/'root-manifest.json')
arm=out/'kernel-source/iokit/Kernel/arm';bsd=out/'kernel-source/bsd/dev';arm.mkdir(parents=True,exist_ok=True)
maintenance=root/'MaliMetal/X9/General'
for source in (maintenance/'kernel').iterdir():
 if source.suffix in ['.h','.cpp','.inc']:shutil.copyfile(source,arm/source.name)
shutil.copyfile(maintenance/'X9RenderABI.h',arm/'X9RenderABI.h')
shutil.copyfile(w/'render-kernel-generated/x9_render_cs.inc',arm/'x9_render_cs.inc')
for name in ['SweetSurface.h','SweetSurface.cpp','SweetFramebuffer.cpp']:shutil.copyfile(w/'render-display-inputs'/name,arm/name)
shutil.copyfile(w/'resident-kernel-inputs/SweetX9CSF.cpp',arm/'SweetX9CSF.cpp')
shutil.copyfile(w/'resident-kernel-inputs/sweet_x9_gpu_dependencies.c',bsd/'sweet_x9_gpu_dependencies.c')
obj=out/'kernel-objects';obj.mkdir();replacements={};compile_records=[]
for folder in ['render-kernel-objects','resident-kernel-inputs','render-display-inputs']:
 for record in json.loads((w/folder/'commands.json').read_text()):
  original=list(record['args']);source=Path(original[original.index('-c')+1]);old_obj=Path(original[original.index('-o')+1])
  new_source=(bsd if source.suffix=='.c' else arm)/source.name;new_obj=obj/old_obj.name
  shutil.copyfile(old_obj,new_obj);replacements[str(old_obj)]=str(new_obj)
  args=[a.replace(str(base),str(out)) for a in original]
  args[args.index('-c')+1]=str(new_source);args[args.index('-o')+1]=str(new_obj)
  if '-MF' in args:args[args.index('-MF')+1]=str(new_obj)+'.d'
  # All maintained headers/generated CS words are frozen beside the source.
  args=[a for a in args if not (a.startswith('-I') and any(a.startswith('-I'+str(p)) for p in [maintenance,w/'render-display-inputs',w/'render-kernel-generated']))]
  args[1:1]=['-I'+str(arm)]
  compile_records.append({'args':args,'cwd':record['cwd'].replace(str(base),str(out))})
entries=(w/'native-kernel/kernel-link.filelist').read_text().splitlines()
filelist=out/'kernel-link.filelist';filelist.write_text('\n'.join(replacements.get(e,e) for e in entries)+'\n')
link_records=json.loads((w/'native-kernel/commands.json').read_text())
for record in link_records:
 record['args']=[a.replace(str(w/'native-kernel'),str(out)) for a in record['args']]
 record['cwd']=record['cwd'].replace(str(base),str(out))
(out/'commands.json').write_text(json.dumps(compile_records+link_records,indent=2)+'\n')
(out/'build-kernel.py').write_text('''#!/usr/bin/env python3
"""Normal rebuild of frozen pan169 kernel sources; no tests/device access."""
from pathlib import Path
import json,os,subprocess
out=Path(__file__).resolve().parent
env=os.environ.copy();env['DEVELOPER_DIR']='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer'
for record in json.loads((out/'commands.json').read_text()):
 subprocess.run(record['args'],cwd=record['cwd'],env=env,check=True)
''')
# Replace the old experiment flag instead of adding to the607-byte cmdline.
script=(base/'build.sh').read_text().replace('x9_metal_system=2','x9_metal_render=1')
script=script.replace('/Applications/Xcode.app','/Users/tosasitill/Downloads/Xcode.app')
tools=out/'build-tools';tools.mkdir()
wrapper='#!/bin/sh\n# Bare-metal LLVM22 matches lld22; host validation uses the frozen Xcode SDK.\nfor arg in "$@"; do\n case "$arg" in\n  --target=aarch64-none-elf) exec /opt/homebrew/opt/llvm@22/bin/clang "$@" ;;\n esac\ndone\nexec /Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang -isysroot /Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk "$@"\n'
cc=tools/'x9-clang';cc.write_text(wrapper);cc.chmod(0o755)
script=script.replace('export X9_CC=/opt/homebrew/opt/llvm/bin/clang','export X9_CC="$OUT/build-tools/x9-clang"')
args=shlex.split(script.split('--boot-args ',1)[1])[0]
if len(args.encode())>607:raise RuntimeError('boot args exceed ABI limit')
if 'x9_metal_system=' in args or 'x9_metal_render=1' not in args:raise RuntimeError('render boot mode mismatch')
(out/'boot-args.txt').write_text(args+'\n');(out/'build.sh').write_text(script)
patchdir=out/'kernel-patches';patchdir.mkdir()
for number in range(243,252):
 for patch in (root/'xnu/patches').glob(f'{number:04d}-*.patch'):shutil.copyfile(patch,patchdir/patch.name)
metadata={
 'candidate':out.name,'base_candidate':str(base),'boot_image':str(out/'boot.img'),
 'runtime_status':'NOT RUN','build_status':'pending link/package',
 'kernel_objects':'Frozen native service/CSF engine/cache/display sources and changed objects; exact link filelist retains unchanged earlier frozen objects.',
 'boot_args':args,'boot_args_bytes':len(args.encode()),
 'root_manifest':str(out/'root-manifest.json'),'console_base':'0xaec0f000','console_size':'0x200000','pstore_mirror':True,
 'kernel_patches':[p.name for p in sorted(patchdir.iterdir())],
 'enabled':['startup compute campaign inherited frompan168','resident CSF/GPUEB ownership','native render service and cache-ownership capability','RAM default IOSurface and existing CPU/LK delivery'],
 'not_enabled':['legacy system Metal compute discovery/deployment','legacy desktop copy worker','general Metal plugin publication to desktop render owners'],
 'test_scope':'New kernel startup, resident capability/service publication and RAM surface/GUI compatibility. Native arbitrary shader/render/cache handoff still needs an actual client submission; this image does not install that client or claim GPU desktop rendering.',
 'hardware_actions':'None; owner flashes manually. No resets or extra tests added.',
}
(out/'reference-inputs.json').write_text(json.dumps(metadata,indent=2)+'\n')
env=os.environ.copy();env['DEVELOPER_DIR']='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer'
# Objects were normally compiled by the preceding renderer builds. Link those
# frozen copies directly; do not run a duplicate build/artifact verification.
for record in link_records:subprocess.run(record['args'],cwd=record['cwd'],env=env,check=True)
subprocess.run(['/bin/bash',str(out/'build.sh')],cwd=out,check=True)
metadata['build_status']='normal native kernel link and boot package succeeded; build-driver.log'
(out/'reference-inputs.json').write_text(json.dumps(metadata,indent=2)+'\n')
print('PAN169 READY:',out/'boot.img',flush=True)
print('boot args:',len(args.encode()),'bytes; resident render mode; no device writes or runtime tests',flush=True)
