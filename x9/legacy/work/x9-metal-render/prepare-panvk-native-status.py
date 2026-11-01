#!/usr/bin/env python3
"""Complete native queue status and remove unadvertised WSI dispatch imports."""
from pathlib import Path
import difflib
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render';base=w/'mesa-render-source';diff=[]
for relative in ['src/panfrost/vulkan/panvk_instance.c','src/panfrost/vulkan/panvk_vX_device.c']:
 p=base/relative;s=p.read_text();old=s
 kind='instance' if relative.endswith('/panvk_instance.c') else 'device'
 needle='&wsi_'+kind+'_entrypoints, false);'
 at=s.index(needle);start=s.rfind('   vk_'+kind+'_dispatch_table_from_entrypoints(',0,at)
 if start<0:raise RuntimeError('dispatch anchor absent')
 end=at+len(needle);block=s[start:end]
 if s[max(0,start-24):start].endswith('#ifndef X9_NATIVE_KMOD\n'):continue
 s=s[:start]+'#ifndef X9_NATIVE_KMOD\n'+block+'\n#endif'+s[end:]
 diff.extend(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative));p.write_text(s)
relative='src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c';p=base/relative;s=p.read_text();old=s
anchor='''   int ret = pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_GROUP_GET_STATE,
                            &state);'''
if 'int ret = x9_pan_kmod_group_check' not in s:
 if s.count(anchor)!=1:raise RuntimeError('queue status anchor absent')
 s=s.replace(anchor,'''#ifdef X9_NATIVE_KMOD
   int ret = x9_pan_kmod_group_check(dev->kmod.dev, queue->group_handle);
#else
'''+anchor+'\n#endif')
 diff.extend(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative));p.write_text(s)
if diff:(w/'panvk-native-status-no-wsi.patch').write_text(''.join(diff))
print('Native synchronous group status and WSI dispatch removal prepared')
