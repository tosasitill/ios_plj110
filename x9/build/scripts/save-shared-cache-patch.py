#!/usr/bin/env python3
"""Retain cache-ownership source deltas and the source-only replay recipe."""
from pathlib import Path
import difflib,json
w=Path(__file__).resolve().parent;root=w.parents[1]
base=w/'shared-cache-inputs';sections=[];runtime=[];mesa=[];kernel=[];files=[]
for before in sorted(base.rglob('*')):
 if not before.is_file():continue
 rel=before.relative_to(base);after=root/rel
 old=before.read_text();new=after.read_text()
 if old==new:continue
 path=str(rel);files.append(path)
 def diff(path):return 'diff --git a/'+path+' b/'+path+'\n'+''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+path,tofile='b/'+path))
 sections.append(diff(path))
 if path.startswith('work/x9-metal-render/metal2vulkan-source/'):
  runtime.append(diff(path.removeprefix('work/x9-metal-render/metal2vulkan-source/')))
 if path.startswith('work/x9-metal-render/mesa-render-source/'):
  mesa.append(diff(path.removeprefix('work/x9-metal-render/mesa-render-source/')))
 if path.startswith('MaliMetal/X9/General/kernel/'):
  kernel.append(diff('iokit/Kernel/arm/'+after.name))
 if path=='MaliMetal/X9/General/X9RenderABI.h':kernel.append(diff('iokit/Kernel/arm/X9RenderABI.h'))
(w/'native-shared-cache-ownership.patch').write_text(''.join(sections))
(w/'runtime-submission-cache-ownership.patch').write_text(''.join(runtime))
(w/'panvk-native-cache-ownership.patch').write_text(''.join(mesa))
(root/'xnu/patches/0250-x9-submission-cache-ownership.patch').write_text(''.join(kernel))
(w/'shared-cache-reference-inputs.json').write_text(json.dumps({
 'references':str(root/'work/references/UniXNU-gpu/reference-inputs.json'),
 'reference_contracts':['Adreno740/NativeAdrenoMemory.cpp:publishForDevice/acquireDeviceWrites','Adreno740/NativeAdrenoVm.cpp:publishMapped/acquireImported','Adreno740/NativeAdrenoCpu.cpp:explicit cache instructions'],
 'hardware_scope':'MT6993 CPU cache handoff and existing stock v14 GPU flush; no Qualcomm register/SMMU/4KiB-host implementation imported',
 'runtime_base':'720ef1ce9ae2a2068999a8675717418b9416283e',
 'mesa_base':'300b5fd7b44120b80607d1c90b40bb126e8d3ec8',
 'cache_atom':256,'hardware_snooping_enabled':False,'vulkan_host_coherent_advertised':False,
 'scope':'synchronous CPU/GPU ownership at cache atoms, including imported surface backing; no simultaneous CPU/GPU writes in one owned atom',
 'source_files':files,'kernel_patch':'0250-x9-submission-cache-ownership.patch',
 'normal_builds':['build-general-metal.py','build-panvk-native-ios.py','build-native-kernel-link.py','package-panvk-native.py'],
 'runtime_status':'NOT RUN; no device writes or extra tests',
},indent=2)+'\n')
print('Cache ownership source patches and pinned replay inputs saved',flush=True)
