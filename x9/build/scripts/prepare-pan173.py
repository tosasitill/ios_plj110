#!/usr/bin/env python3
"""Prepare independent iOS plugin package and a small boot candidate.
Normal link/package only; no device access or test program in boot/root package.
"""
from pathlib import Path
import hashlib,json,shutil,subprocess,os,tarfile,difflib
w=Path(__file__).resolve().parent;root=w.parents[1];base=root/'x9/out/pan169-native-render-cache';out=root/'x9/out/pan173-cache-granularity';out.mkdir(exist_ok=False)
for name in ['kernel-source','packaging-source','generated','firmware','compute-source','build-tools']:shutil.copytree(base/name,out/name)
for name in ['root-manifest.json','boot-args.txt','build.sh']:shutil.copyfile(base/name,out/name)
package=out/'plugin-package';package.mkdir();bundles=['X9GeneralMetal173.bundle','X9PanVK173.bundle']
for name in bundles:shutil.copytree(w/name,package/name)
files=[]
for name in bundles:
 for p in sorted((package/name).rglob('*')):
  if p.is_file():
   target='/System/Library/Extensions/'+name+'/'+str(p.relative_to(package/name))
   files.append({'path':target,'source':str(p.relative_to(package)),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
# Generated allowlist is a build input, not a post-build artifact check.
lines=['/* Exact independent plugin package; no executable bytes in kernel. */','#pragma once',
 'struct X9ExternalFile {const char *path;uint64_t bytes;unsigned char sha256[32];};','static const struct X9ExternalFile x9_external_files[]={']
for f in files:
 lines.append('{'+json.dumps(f['path'])+','+str(f['bytes'])+',{'+','.join('0x'+f['sha256'][i:i+2] for i in range(0,64,2))+'}},')
lines+=['};'];(out/'generated/x9_render_external_manifest.h').write_text('\n'.join(lines)+'\n')
(package/'manifest.json').write_text(json.dumps({'files':files,'destination':'iOS writable native HFS root; never Android F2FS plaintext view','no_probe_or_test_program':True},indent=2)+'\n')
installer='''#!/bin/sh
# Run as root on the mounted iOS root volume, from this extracted package.
# This creates only two versioned plugin directories; it never changes launchd.
set -eu
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "root required" >&2; exit 1; }
command -v shasum >/dev/null || { echo "shasum required" >&2; exit 1; }
DEST=/System/Library/Extensions
[ -d "$DEST" ] && [ -w "$DEST" ] || { echo "iOS Extensions directory unavailable or not writable" >&2; exit 1; }
for name in X9GeneralMetal173.bundle X9PanVK173.bundle; do
 [ ! -e "$DEST/$name" ] && [ ! -L "$DEST/$name" ] || { echo "existing destination preserved: $DEST/$name" >&2; exit 1; }
done
shasum -a 256 -c SHA256SUMS
for name in X9GeneralMetal173.bundle X9PanVK173.bundle; do
 stage="$DEST/.$name.installing"
 [ ! -e "$stage" ] && [ ! -L "$stage" ] || { echo "staging conflict preserved: $stage" >&2; exit 1; }
 mkdir "$stage"
 cp -Rp "$name/." "$stage/"
 chmod 755 "$stage"
 # A conflicting directory is never overwritten; mv must create this new name.
 [ ! -e "$DEST/$name" ] && [ ! -L "$DEST/$name" ] || { echo "destination appeared: $DEST/$name" >&2; exit 1; }
 mv "$stage" "$DEST/$name"
 done
sync
echo "Installed versioned Metal and PanVK bundles. Restart XNU to verify exact payload and enable discovery."
'''
(package/'install-on-ios.sh').write_text(installer);(package/'install-on-ios.sh').chmod(0o755)
(package/'SHA256SUMS').write_text(''.join(f['sha256']+'  '+f['source']+'\n' for f in files))
archive=out/'x9-metal-plugins-ios.tar.gz'
with tarfile.open(archive,'w:gz',compresslevel=6) as tar:
 for p in sorted(package.iterdir()):tar.add(p,arcname=p.name)
arm=out/'kernel-source/iokit/Kernel/arm';kern=out/'kernel-source/bsd/kern';bsd=out/'kernel-source/bsd/dev';maintenance=root/'MaliMetal/X9/General'
for source in (maintenance/'kernel').iterdir():
 if source.suffix in ['.h','.cpp','.inc']:shutil.copyfile(source,arm/source.name)
shutil.copyfile(maintenance/'X9RenderABI.h',arm/'X9RenderABI.h');shutil.copyfile(w/'render-kernel-generated/x9_render_cs.inc',arm/'x9_render_cs.inc')
for name in ['SweetSurface.h','SweetSurface.cpp','SweetFramebuffer.cpp']:shutil.copyfile(w/'render-display-inputs'/name,arm/name)
shutil.copyfile(w/'resident-kernel-inputs/SweetX9CSF.cpp',arm/'SweetX9CSF.cpp');shutil.copyfile(w/'resident-kernel-inputs/sweet_x9_gpu_dependencies.c',bsd/'sweet_x9_gpu_dependencies.c')
shutil.copyfile(maintenance/'kernel/x9_render_external.inc',kern/'x9_render_external.inc')
p=kern/'kern_exec.c';old=p.read_text();new=old.replace('#include "x9_metal_deploy.inc"','#include "x9_render_external.inc"')
anchor='''	{
	 int metal_error=sweet_x9_metal_deploy(p,scratch_addr+3*sizeof(user64_addr_t),scratch_end);
	 extern int sweet_x9_metal_payload_ready;
	 sweet_x9_metal_payload_ready=metal_error==0;
	 printf("X9 METAL DEPLOY: probe/cache installation errno=%d; original launchd continues\\n",metal_error);
	}'''
if new.count(anchor)!=1:raise RuntimeError('PID1 old compute payload hook anchor')
new=new.replace(anchor,'''	{
	 int external_error=sweet_x9_render_external_ready();
	 if(external_error)printf("X9 RENDER EXTERNAL: not armed errno=%d\\n",external_error);
	}''')
p.write_text(new)
obj=out/'kernel-objects';obj.mkdir();mapping={};records=[]
for folder in ['render-kernel-objects','resident-kernel-inputs','render-display-inputs']:
 for record in json.loads((w/folder/'commands.json').read_text()):
  args=list(record['args']);source=Path(args[args.index('-c')+1]);oldobj=Path(args[args.index('-o')+1]);newobj=obj/oldobj.name
  shutil.copyfile(oldobj,newobj);mapping[str(oldobj)]=str(newobj)
  args=[a.replace(str(root/'x9/out/pan168-desktop-workgroup-xy'),str(out)) for a in args]
  args[args.index('-c')+1]=str((bsd if source.suffix=='.c' else arm)/source.name);args[args.index('-o')+1]=str(newobj)
  if '-MF' in args:args[args.index('-MF')+1]=str(newobj)+'.d'
  args=[a for a in args if not(a.startswith('-I') and any(a.startswith('-I'+str(t)) for t in [maintenance,w/'render-display-inputs',w/'render-kernel-generated']))]
  args[1:1]=['-I'+str(arm)];records.append({'args':args,'cwd':record['cwd'].replace(str(root/'x9/out/pan168-desktop-workgroup-xy'),str(out))})
oldbase=root/'x9/out/pan168-desktop-workgroup-xy';oldcmd=json.loads((oldbase/'commands.json').read_text());e=next(v for v in oldcmd if any(a.endswith('/kern_exec.c') for a in v['args']))
args=[a.replace(str(oldbase),str(out)) for a in e['args']];args[0]='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang';args[1:1]=['-I'+str(out/'generated'),'-I'+str(arm)]
exec_record={'args':args,'cwd':e['cwd']};records.append(exec_record)
entries=(w/'native-kernel/kernel-link.filelist').read_text().splitlines();entries=[str(obj/'kern_exec.o') if Path(e).name=='kern_exec.o' else mapping.get(e,e) for e in entries]
(out/'kernel-link.filelist').write_text('\n'.join(entries)+'\n')
link=json.loads((w/'native-kernel/commands.json').read_text())
for e in link:e['args']=[a.replace(str(w/'native-kernel'),str(out)) for a in e['args']];e['cwd']=e['cwd'].replace(str(oldbase),str(out))
(out/'commands.json').write_text(json.dumps(records+link,indent=2)+'\n');shutil.copyfile(base/'build-kernel.py',out/'build-kernel.py')
parts=[]
for rel,a,b in [('bsd/kern/kern_exec.c',old,new),('bsd/kern/x9_render_external.inc','',(kern/'x9_render_external.inc').read_text())]:
 parts.append('diff --git a/'+rel+' b/'+rel+'\n')
 if not a:parts.append('new file mode 100644\n')
 parts.extend(difflib.unified_diff(a.splitlines(True),b.splitlines(True),fromfile='a/'+rel if a else '/dev/null',tofile='b/'+rel))
patch=root/'xnu/patches/0253-x9-external-metal-plugin-verification.patch';(out/'external-metal-verification.patch').write_text(''.join(parts))
(out/'reference-inputs.json').write_text(json.dumps({'candidate':out.name,'base':str(base),'runtime_status':'NOT RUN','plugins':str(archive),'target':'iOS writable root System/Library/Extensions','boot_contains_plugin_binaries':False,'boot_contains_render_probe':False,'installation_status':'NOT INSTALLED; adb device not attached','exact_file_manifest':str(package/'manifest.json'),'console_base':'0xaec0f000','console_size':'0x200000','pstore_mirror':True},indent=2)+'\n')
env=os.environ.copy();env['DEVELOPER_DIR']='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer'
subprocess.run(exec_record['args'],cwd=exec_record['cwd'],env=env,check=True)
for e in link:subprocess.run(e['args'],cwd=e['cwd'],env=env,check=True)
subprocess.run(['/bin/bash',str(out/'build.sh')],cwd=out,check=True)
print('PAN173 small boot + separate plugin archive built; NOT INSTALLED / NOT RUN',flush=True)
