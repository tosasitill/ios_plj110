#!/usr/bin/env python3
"""Freeze native desktop Metal payload, compile deploy hook, link/package pan170.
Only normal build operations. No host/device test execution or device writes.
"""
from pathlib import Path
import json,os,shutil,subprocess,sys,difflib
w=Path(__file__).resolve().parent;root=w.parents[1]
base=root/'x9/out/pan169-native-render-cache';out=root/'x9/out/pan170-native-metal-desktop'
out.mkdir(exist_ok=False)
for name in ['kernel-source','packaging-source','generated','firmware','compute-source','build-tools']:
 shutil.copytree(base/name,out/name)
for name in ['root-manifest.json','boot-args.txt','build.sh']:shutil.copyfile(base/name,out/name)
arm=out/'kernel-source/iokit/Kernel/arm';bsd=out/'kernel-source/bsd/dev';kern=out/'kernel-source/bsd/kern'
maintenance=root/'MaliMetal/X9/General'
for source in (maintenance/'kernel').iterdir():
 if source.suffix in ['.h','.cpp','.inc']:shutil.copyfile(source,arm/source.name)
shutil.copyfile(maintenance/'kernel/x9_render_deploy.inc',kern/'x9_render_deploy.inc')
shutil.copyfile(maintenance/'X9RenderABI.h',arm/'X9RenderABI.h')
shutil.copyfile(w/'render-kernel-generated/x9_render_cs.inc',arm/'x9_render_cs.inc')
for name in ['SweetSurface.h','SweetSurface.cpp','SweetFramebuffer.cpp']:shutil.copyfile(w/'render-display-inputs'/name,arm/name)
shutil.copyfile(w/'resident-kernel-inputs/SweetX9CSF.cpp',arm/'SweetX9CSF.cpp')
shutil.copyfile(w/'resident-kernel-inputs/sweet_x9_gpu_dependencies.c',bsd/'sweet_x9_gpu_dependencies.c')
shutil.copyfile(w/'desktop-deploy-inputs/x9_render_payload.h',out/'generated/x9_render_payload.h')
payload_inputs=out/'payload-inputs';payload_inputs.mkdir()
# Keep all data referenced by .incbin in this candidate's frozen inputs.
assembly=(w/'desktop-deploy-inputs/x9_render_payload.S').read_text();lines=[]
for index,line in enumerate(assembly.splitlines()):
 if line.startswith('.incbin '):
  source=Path(json.loads(line[len('.incbin '):]));target=payload_inputs/(str(index)+'-'+source.name)
  shutil.copyfile(source,target);line='.incbin '+json.dumps(str(target))
 lines.append(line)
(out/'generated/x9_render_payload.S').write_text('\n'.join(lines)+'\n')
shutil.copytree(maintenance/'probe',out/'probe-source')
shutil.copyfile(w/'desktop-deploy-inputs/payload-inputs.json',payload_inputs/'payload-inputs.json')
# Invoke the full native deployment after the earlier compute-only guarded
# deploy; that older function is disabled by absence of x9_metal_system.
p=kern/'kern_exec.c';before=p.read_text();after=before.replace('#include "x9_metal_deploy.inc"','#include "x9_metal_deploy.inc"\n#include "x9_render_deploy.inc"')
anchor='\t sweet_x9_metal_payload_ready=metal_error==0;'
if after.count(anchor)!=1:raise RuntimeError('PID1 native deployment hook anchor')
after=after.replace(anchor,anchor+'''
	 int render_error=sweet_x9_render_deploy(p,scratch_addr+3*sizeof(user64_addr_t),scratch_end);
	 printf("X9 RENDER DEPLOY: full desktop payload errno=%d; original launchd continues\\n",render_error);''')
p.write_text(after)
obj=out/'kernel-objects';obj.mkdir();replacements={};compile_records=[]
for folder in ['render-kernel-objects','resident-kernel-inputs','render-display-inputs']:
 for record in json.loads((w/folder/'commands.json').read_text()):
  original=list(record['args']);source=Path(original[original.index('-c')+1]);old_obj=Path(original[original.index('-o')+1])
  new_source=(bsd if source.suffix=='.c' else arm)/source.name;new_obj=obj/old_obj.name
  shutil.copyfile(old_obj,new_obj);replacements[str(old_obj)]=str(new_obj)
  args=[a.replace(str(root/'x9/out/pan168-desktop-workgroup-xy'),str(out)) for a in original]
  args[args.index('-c')+1]=str(new_source);args[args.index('-o')+1]=str(new_obj)
  if '-MF' in args:args[args.index('-MF')+1]=str(new_obj)+'.d'
  args=[a for a in args if not (a.startswith('-I') and any(a.startswith('-I'+str(p)) for p in [maintenance,w/'render-display-inputs',w/'render-kernel-generated']))]
  args[1:1]=['-I'+str(arm)]
  compile_records.append({'args':args,'cwd':record['cwd'].replace(str(root/'x9/out/pan168-desktop-workgroup-xy'),str(out))})
old_base=root/'x9/out/pan168-desktop-workgroup-xy'
old_records=json.loads((old_base/'commands.json').read_text())
old_exec=next(s for s in old_records if any(a.endswith('/kern_exec.c') for a in s['args']))
exec_args=[a.replace(str(old_base),str(out)) for a in old_exec['args']]
exec_args[0]='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
exec_args[1:1]=['-I'+str(out/'generated'),'-I'+str(arm)]
exec_record={'args':exec_args,'cwd':old_exec['cwd']}
compile_records.append(exec_record)
entries=(w/'native-kernel/kernel-link.filelist').read_text().splitlines();linked=[]
for entry in entries:
 if Path(entry).name=='kern_exec.o':linked.append(str(obj/'kern_exec.o'))
 else:linked.append(replacements.get(entry,entry))
shutil.copyfile(w/'desktop-deploy-inputs/x9_render_payload.o',obj/'x9_render_payload.o');linked.append(str(obj/'x9_render_payload.o'))
filelist=out/'kernel-link.filelist';filelist.write_text('\n'.join(linked)+'\n')
link_records=json.loads((w/'native-kernel/commands.json').read_text())
for record in link_records:
 record['args']=[a.replace(str(w/'native-kernel'),str(out)) for a in record['args']]
 record['cwd']=record['cwd'].replace(str(old_base),str(out))
asm_record={'args':['/Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang','-target','arm64-apple-macos14.0','-c',str(out/'generated/x9_render_payload.S'),'-o',str(obj/'x9_render_payload.o')],'cwd':str(out)}
(out/'commands.json').write_text(json.dumps(compile_records+[asm_record]+link_records,indent=2)+'\n')
shutil.copyfile(base/'build-kernel.py',out/'build-kernel.py')
patchdir=out/'kernel-patches';patchdir.mkdir()
for number in range(243,252):
 for patch in (root/'xnu/patches').glob(f'{number:04d}-*.patch'):shutil.copyfile(patch,patchdir/patch.name)
# Numbered source patch excludes generated executable bytes, which remain
# ordinary assembler inputs with their build recipe and pinned source list.
parts=[]
for rel,old,new in [
 ('bsd/kern/kern_exec.c',before,after),
 ('bsd/kern/x9_render_deploy.inc','',(kern/'x9_render_deploy.inc').read_text()),
 ('iokit/Kernel/arm/SweetFramebuffer.cpp',(base/'kernel-source/iokit/Kernel/arm/SweetFramebuffer.cpp').read_text(),(arm/'SweetFramebuffer.cpp').read_text()),
 ('iokit/Kernel/arm/SweetX9RenderService.cpp',(w/'desktop-render-inputs/MaliMetal/X9/General/kernel/SweetX9RenderService.cpp').read_text(),(arm/'SweetX9RenderService.cpp').read_text()),
]:
 parts.append('diff --git a/'+rel+' b/'+rel+'\n')
 if not old:parts.append('new file mode 100644\n')
 parts.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+rel if old else '/dev/null',tofile='b/'+rel))
patch=root/'xnu/patches/0252-x9-native-desktop-metal-discovery-and-deployment.patch';patch.write_text(''.join(parts));shutil.copyfile(patch,patchdir/patch.name)
metadata={
 'candidate':out.name,'base_candidate':str(base),'boot_image':str(out/'boot.img'),
 'runtime_status':'NOT RUN','build_status':'pending normal deploy-hook compile/kernel link/boot packaging',
 'boot_args':(out/'boot-args.txt').read_text().strip(),'boot_args_bytes':len((out/'boot-args.txt').read_text().strip().encode()),
 'root_manifest':str(out/'root-manifest.json'),'console_base':'0xaec0f000','console_size':'0x200000','pstore_mirror':True,
 'desktop_path':'original Metal discovery -> X9GeneralMetalFactory -> Rust metal2vulkan -> native PanVK v14 -> CSF streams -> imported RAM IOSurface -> existing LK delivery',
 'deployment':'Guard exact original/pan166/pan167/pan168/pan170 launchd caches; keep hardlink backup; exclusive/fsynced custom files; atomic cache rename; Apple executable bytes untouched.',
 'boot_probe':'RunAtLoad root executable;3 real compute/cache roundtrips and a vertex/fragment triangle into imported IOSurface,4096 expected pixels. Probe NOT RUN by host.',
 'remaining_limits':['8 resident physical CSG slots','single Vulkan GPU queue per device,3 simultaneous CS subqueues','fixed128MiB tiler heap;OOM is a fault,not dynamic growth','no fd/sync-file external synchronization','iOS16 private Metal ABI and actual desktop workloads not hardware validated'],
 'native_payload':json.loads((w/'desktop-deploy-inputs/payload-inputs.json').read_text()),
 'pan169_evidence':'/Users/tosasitill/HelioDarwin/logs/x9-pan169-no-desktop-20261029-144053-174508.raw',
 'pan169_actual':'resident startup/caps0x3f successful;8 SpringBoard null GL dispatch crashes;GUI stops atswap6;not a cache roundtrip failure',
}
if metadata['boot_args_bytes']>607:raise RuntimeError('boot args exceed607bytes')
(out/'reference-inputs.json').write_text(json.dumps(metadata,indent=2)+'\n')
env=os.environ.copy();env['DEVELOPER_DIR']='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer'
# Existing native objects were compiled immediately before this freeze. Only
# the new PID1 hook needs compilation here, then normal full link and package.
subprocess.run(exec_record['args'],cwd=exec_record['cwd'],env=env,check=True)
for record in link_records:subprocess.run(record['args'],cwd=record['cwd'],env=env,check=True)
subprocess.run(['/bin/bash',str(out/'build.sh')],cwd=out,check=True)
metadata['build_status']='normal deploy-hook compile,full kernel link and boot packaging succeeded'
(out/'reference-inputs.json').write_text(json.dumps(metadata,indent=2)+'\n')
print('PAN170 native desktop Metal candidate packaged:',out/'boot.img','; NOT RUN, no device writes',flush=True)
