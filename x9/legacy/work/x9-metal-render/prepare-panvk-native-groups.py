#!/usr/bin/env python3
"""Port guarded group lifecycle and queue-local CSF property access."""
from pathlib import Path
import difflib,subprocess
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
subprocess.run(['python3',str(w/'prepare-panvk-native-queue.py')],check=True)
relative='src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c'
p=w/'mesa-render-source'/relative
s=p.read_text();original=s
marker='/* X9 native group lifecycle and queue-local properties. */'
if marker in s:
 print('Native group lifecycle port already present');raise SystemExit(0)
def replace(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('source anchor changed: '+a[:100])
 s=s.replace(a,b)
replace('#include "X9PanVKQueueBridge.inc"','#include "X9PanVKQueueBridge.inc"\n'+marker+'\n#define panthor_kmod_get_csif_props(dev) x9_pan_kmod_csif(dev)\n/* Zero requests unconditional GPU cache flush, never a fabricated flush ID. */\n#define panthor_kmod_get_flush_id(dev) 0u')
replace('''      panthor_kmod_get_csif_props(dev->kmod.dev);

   struct cs_buffer root_cs''','''      panthor_kmod_get_csif_props(dev->kmod.dev);
#ifdef X9_NATIVE_KMOD
   if (!csif_info)
      return panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED, "Native CSF limits unavailable");
#endif

   struct cs_buffer root_cs''')
a='   int ret = pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, &gc);'
replace(a,'''#ifdef X9_NATIVE_KMOD
   int ret = x9_panvk_group_create(dev->kmod.dev, &gc,
      (uint64_t)queue->tiler_heap.chunk_size * phys_dev->csf.tiler.max_chunks);
#else
'''+a+'\n#endif')
a='''   ASSERTED int ret =
      pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &gd);'''
replace(a,'''#ifdef X9_NATIVE_KMOD
   ASSERTED int ret = x9_pan_kmod_group_destroy(dev->kmod.dev, gd.group_handle);
#else
'''+a+'\n#endif')
a='''   panvk_queue_submit_init_req_resource(&submit);
   panvk_queue_submit_init_waits(&submit, vk_submit);'''
replace(a,'''#ifdef X9_NATIVE_KMOD
   /* Keep external waits before the resource-request preamble in each stream. */
   panvk_queue_submit_init_waits(&submit, vk_submit);
   panvk_queue_submit_init_req_resource(&submit);
#else
'''+a+'\n#endif')
(w/'panvk-native-group-lifecycle.patch').write_text(''.join(difflib.unified_diff(original.splitlines(True),s.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative)))
p.write_text(s)
print('Native group create/destroy and queue-local CSF getter ports applied; guarded by X9_NATIVE_KMOD')
