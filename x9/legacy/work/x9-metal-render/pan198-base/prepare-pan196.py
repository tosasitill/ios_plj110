#!/usr/bin/env python3
"""Package pan196 packed color and read-only barrier candidate; offline, no device access."""
from pathlib import Path
import hashlib,json,shutil,tarfile
w=Path(__file__).resolve().parent
root=w.parents[1]
base=root/'x9/out/pan195-render-boundary-paths'
out=root/'x9/out/pan196-a1bgr5-ordered-color'
out.mkdir(exist_ok=False)
for name in ['kernel-source','packaging-source','build-tools','generated','firmware','compute-source','kernel-objects']:
    shutil.copytree(base/name,out/name)
for name in ['root-manifest.json','boot-args.txt','build.sh','build-kernel.py','external-metal-verification.patch']:
    shutil.copyfile(base/name,out/name)
(out/'kernel-source/bsd/kern/x9_render_external.inc').write_text((root/'MaliMetal/X9/General/kernel/x9_render_external.inc').read_text()+'\n#include "x9_update_install.inc"\n')
for name in ['SweetX9RenderService.cpp','SweetX9RenderEngine.inc','X9CSFRenderQueue.cpp']:
    shutil.copyfile(root/'MaliMetal/X9/General/kernel'/name,out/'kernel-source/iokit/Kernel/arm'/name)
package=out/'plugin-package'
package.mkdir()
bundles=['X9GeneralMetal196.bundle','X9PanVK196.bundle']
for name in bundles:
    shutil.copytree(w/name,package/name)
files=[]
for name in bundles:
    for p in sorted((package/name).rglob('*')):
        if p.is_file():
            files.append({'path':'/System/Library/Extensions/'+name+'/'+str(p.relative_to(package/name)),
                'source':str(p.relative_to(package)),'bytes':p.stat().st_size,
                'sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
mailbox=json.loads((root/'work/x9-metal-update/mailbox-manifest.json').read_text())
lines=['/* Exact independent plugin package; no executable bytes in kernel. */','#pragma once',
 'struct X9ExternalFile {const char *path;uint64_t bytes;unsigned char sha256[32];uint64_t offset;int mode;};',
 'static const struct X9ExternalFile x9_external_files[]={']
offset=4096
for f in files:
    f['offset']=offset
    f['mode']=0o755 if f['source'].endswith(('/X9GeneralMetal','/X9PanVK')) else 0o644
    lines.append('{'+json.dumps(f['path'])+','+str(f['bytes'])+',{'+','.join('0x'+f['sha256'][i:i+2] for i in range(0,64,2))+'},'+str(offset)+','+str(f['mode'])+'},')
    offset=(offset+f['bytes']+4095)&~4095
if offset>mailbox['logical_blocks']*4096:
    raise RuntimeError('mailbox capacity exceeded')
identity=hashlib.sha256(json.dumps(files,sort_keys=True,separators=(',',':')).encode()).digest()
lines+=['};','static const unsigned char x9_update_identity[32]={'+','.join('0x%02x'%b for b in identity)+'};']
dirs=sorted(set(str(Path(f['path']).parent) for f in files),key=lambda x:(x.count('/'),x))
lines+=['static const char *const x9_update_dirs[]={']+[json.dumps(x)+',' for x in dirs]+['};']
(out/'generated/x9_render_external_manifest.h').write_text('\n'.join(lines)+'\n')
(package/'mailbox.json').write_text(json.dumps({'identity':identity.hex(),'files':files,'payload_end':offset},indent=2)+'\n')
(package/'manifest.json').write_text(json.dumps({'files':files,'destination':'iOS writable native HFS root; never Android F2FS plaintext view','no_probe_or_test_program':True},indent=2)+'\n')
with tarfile.open(out/'x9-metal-plugins-ios.tar.gz','w:gz') as tar:
    for name in bundles+['mailbox.json','manifest.json']:
        tar.add(package/name,arcname=name)
records=json.loads((base/'commands.json').read_text().replace(str(base),str(out)))
(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
(out/'kernel-link.filelist').write_text((base/'kernel-link.filelist').read_text().replace(str(base),str(out)))
# Rebuild manifest consumers, render service and its CSF translation unit; then link and strip.
resume=[r for r in records if any(a.endswith(('/kern_exec.c','/sweet_x9_ufs.c')) for a in r['args'])]+records[-2:]
(out/'resume-commands.json').write_text(json.dumps(resume,indent=2)+'\n')
(out/'resume-build.py').write_text((out/'build-kernel.py').read_text().replace("out/'commands.json'","out/'resume-commands.json'"))
meta={'candidate':out.name,'base':str(base),'runtime_status':'NOT RUN',
 'purpose':'Raw A1BGR5 sampling, ordered blend and masked write, multisample resolve, read-only vertex barrier proof; retain honest full capability gates',
 'build_status':'PREPARED','installation_status':'NOT STAGED',
 'plugins':str(out/'x9-metal-plugins-ios.tar.gz'),
 'exact_file_manifest':str(package/'manifest.json'),
 'boot_contains_plugin_binaries':False,'boot_contains_render_probe':False,
 'console_base':'0xaec0f000','console_size':'0x200000','pstore_mirror':True}
# Freeze the adapter sources and audit findings as inputs to this candidate.
shutil.copytree(root/'MaliMetal/X9/General',out/'metal-source')
shutil.copyfile(w/'native-metal-system-api-completion.patch',out/'native-metal-system-api-completion.patch')
shutil.copyfile(w/'panvk-native-fixed-tiler-budget.patch',out/'panvk-native-fixed-tiler-budget.patch')
shutil.copyfile(w/'native-metal-strict-family-diagnostics.patch',out/'native-metal-strict-family-diagnostics.patch')
shutil.copyfile(root/'xnu/patches/0259-x9-close-drained-suspended-render-group.patch',out/'0259-x9-close-drained-suspended-render-group.patch')
shutil.copyfile(root/'xnu/patches/0258-x9-render-footprint-close-ack-diagnostics.patch',out/'0258-x9-render-footprint-close-ack-diagnostics.patch')
shutil.copyfile(w/'mesa-render-source/src/panfrost/vulkan/panvk_physical_device.c',out/'panvk_physical_device.c')
meta['runtime_inputs']=json.loads((w/'pan196-runtime-inputs.json').read_text())
meta['previous_hardware_evidence']=meta['runtime_inputs']['last_observed_hardware']
meta['remaining_constraints']=meta['runtime_inputs']['remaining']
meta['all_twenty_blockers_completed']=False
meta['runtime_upstream_commit']=meta['runtime_inputs']['runtime_upstream_commit']
for name in ['pan196-runtime-local.patch','pan196-runtime-inputs.json','build-general-metal.py','package-panvk-native.py']:
    shutil.copyfile(w/name,out/name)
# Keep normal Cargo inputs and frontend sources reviewable. Upstream is pinned;
# the patch contains every local runtime change, including inherited ownership fixes.
shutil.copyfile(w/'metal2vulkan-pan196-source/Cargo.toml',out/'runtime-Cargo.toml')
for name in ['Cargo.lock','Cargo.toml']:
    shutil.copyfile(w/'general-build-inputs'/name,out/('bridge-'+name))
shutil.copyfile(w/'pan196-metal-build.log',out/'metal-build.log')
prior=json.loads((base/'reference-inputs.json').read_text())
rows=prior.get('previous_apple2_rows',[])
meta['previous_apple2_rows']=rows
meta['apple2_post_change_rows']='NOT OBSERVED: pan195 had9 strict blockers; no hardware count inferred for pan196'
meta['all_interfaces_completed']=False
meta['replay_build']={'metal':['python3','build-general-metal.py','--revision','196','--runtime-source',str(w/'metal2vulkan-pan196-source')],'kernel':['python3',str(out/'resume-build.py')],'boot':['bash',str(out/'build.sh')]}
meta['panvk_status']='Uses existing fixed-10MiB tiler native ICD, packaged and signed under revision196; no backend feature flags widened'

shutil.copyfile(w/'pan194-runtime-local.patch',out/'pan194-runtime-local.patch')
shutil.copyfile(w/'pan194-runtime-inputs.json',out/'pan194-runtime-inputs.json')
meta['inherited_runtime_patch']=str(out/'pan194-runtime-local.patch')
meta['runtime_source_patch']=str(out/'pan196-runtime-local.patch')
for name in ['pan195-runtime-local.patch','pan195-runtime-inputs.json']:
    shutil.copyfile(w/name,out/name)
meta['runtime_patch_order']=meta['runtime_inputs']['runtime_patch_order']
shutil.copyfile(base/'build-driver.py',out/'build-driver.py')
shutil.copyfile(Path(__file__),out/'prepare-pan196.py')
(out/'reference-inputs.json').write_text(json.dumps(meta,indent=2)+'\n')
print('Prepared',out,'mailbox bytes',offset,flush=True)
