#!/usr/bin/env python3
"""Package pan198 resource-contract integration candidate; offline, no device access."""
from pathlib import Path
import hashlib,json,shutil,tarfile
w=Path(__file__).resolve().parent
root=w.parents[1]
base=root/'x9/out/pan196-a1bgr5-ordered-color'
out=root/'x9/out/pan198-resource-contracts'
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
bundles=['X9GeneralMetal198.bundle','X9PanVK198.bundle']
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
resume=[r for r in records if any(a.endswith(('/kern_exec.c','/sweet_x9_ufs.c','/SweetX9RenderService.cpp')) for a in r['args'])]+records[-2:]
(out/'resume-commands.json').write_text(json.dumps(resume,indent=2)+'\n')
(out/'resume-build.py').write_text((out/'build-kernel.py').read_text().replace("out/'commands.json'","out/'resume-commands.json'"))
meta={'candidate':out.name,'base':str(base),'runtime_status':'NOT RUN',
 'purpose':'Integrate host-import VM diagnostics, raw linear shadow merge/address fixes, native depth resolve conversion and backing-format guards; preserve honest family gates',
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
meta['runtime_inputs']=json.loads((w/'pan198-runtime-inputs.json').read_text())
meta['previous_hardware_evidence']=meta['runtime_inputs']['last_observed_hardware']
meta['remaining_constraints']=meta['runtime_inputs']['remaining']
meta['all_twenty_blockers_completed']=False
meta['runtime_upstream_commit']=meta['runtime_inputs']['runtime_upstream_commit']
for name in ['pan198-runtime-local.patch','pan198-runtime-inputs.json','build-general-metal.py','package-panvk-native.py']:
    shutil.copyfile(w/name,out/name)
# Keep normal Cargo inputs and frontend sources reviewable. Upstream is pinned;
# the patch contains every local runtime change, including inherited ownership fixes.
shutil.copyfile(w/'metal2vulkan-pan198-source/Cargo.toml',out/'runtime-Cargo.toml')
for name in ['Cargo.lock','Cargo.toml']:
    shutil.copyfile(w/'general-build-inputs'/name,out/('bridge-'+name))
shutil.copyfile(w/'pan198-metal-build.log',out/'metal-build.log')
prior=json.loads((base/'reference-inputs.json').read_text())
rows=prior.get('previous_apple2_rows',[])
meta['previous_apple2_rows']=rows
meta['apple2_post_change_rows']='NOT OBSERVED: latest actual pan195 had9 strict blockers; no hardware count inferred for pan198'
meta['all_interfaces_completed']=False
meta['replay_build']={'cwd':str(root),'metal':['python3',str(w/'build-general-metal.py'),'--revision','198','--runtime-source',str(w/'metal2vulkan-pan198-source')],'panvk':['python3',str(w/'build-panvk-native-ios.py')],'panvk_package':['python3',str(w/'package-panvk-native.py'),'--revision','198'],'prepare_new_candidate':['python3',str(w/'prepare-pan198.py')],'kernel':['python3',str(out/'resume-build.py')],'boot':['bash',str(out/'build.sh')]}
meta['panvk_status']='Native fixed-10MiB tiler ICD with inherited D16+S8 split-plane representation, feature intersection and no AFBC/modifier exposure; packaged under revision198, no hardware execution'

shutil.copyfile(w/'pan194-runtime-local.patch',out/'pan194-runtime-local.patch')
shutil.copyfile(w/'pan194-runtime-inputs.json',out/'pan194-runtime-inputs.json')
meta['inherited_runtime_patch']=str(out/'pan194-runtime-local.patch')
meta['runtime_source_patch']=str(out/'pan198-runtime-local.patch')
for name in ['pan195-runtime-local.patch','pan195-runtime-inputs.json','pan196-runtime-local.patch','pan196-runtime-inputs.json']:
    shutil.copyfile(w/name,out/name)
meta['runtime_patch_order']=meta['runtime_inputs']['runtime_patch_order']
for name in ['pan197-panvk-d16-stencil.patch','pan198-panvk-build.log','pan198-panvk-package.log','build-panvk-native-ios.py','pan198-changed-files.json']:
    shutil.copyfile(w/name,out/name)
shutil.copyfile(w/'mesa-render-source/src/panfrost/vulkan/panvk_image.c',out/'panvk_image.c')
shutil.copyfile(root/'xnu/patches/0260-x9-host-import-vm-region-diagnostics.patch',out/'0260-x9-host-import-vm-region-diagnostics.patch')
meta['native_kernel_changes']='0260 host-import VM region diagnostics and descriptor length guard; recompile actual service TU, plus verified-plugin manifest consumers'
meta['verification_scope']='Normal build and built-in packaging checks only; no tests or post-build binary checks'
meta['build_inputs']={'runtime_source':str(w/'metal2vulkan-pan198-source'),'runtime_patch_base':str(base),'metal_source':str(out/'metal-source')}
meta['hardware_evidence']={'candidate':out.name,'loaded':False,'status':'NOT OBSERVED','apple2_blockers':None,'branch_execution':None,'pixel_correctness':None,'mps_device':None,'core_image_metal_context':None}
meta['deployment']={'boot':str(out/'boot.img'),'plugins':str(out/'x9-metal-plugins-ios.tar.gz'),'mailbox_update':['python3',str(root/'work/x9-metal-update/update.py'),str(out)],'boot_flash':'Device owner manual flash; not performed','mailbox_committed':False}
meta['build_evidence']={'metal_log':str(out/'metal-build.log'),'panvk_log':str(out/'pan198-panvk-build.log'),'kernel_log':str(out/'kernel-build.log'),'package_log':str(out/'package-build.log'),'driver_log':str(out/'build-driver.log'),'normal_build_completed':False,'extra_tests_run':False,'post_build_binary_checks_run':False,'mailbox_payload_bytes':offset}
shutil.copyfile(base/'build-driver.py',out/'build-driver.py')
shutil.copyfile(Path(__file__),out/'prepare-pan198.py')
(out/'reference-inputs.json').write_text(json.dumps(meta,indent=2)+'\n')
print('Prepared',out,'mailbox bytes',offset,flush=True)
