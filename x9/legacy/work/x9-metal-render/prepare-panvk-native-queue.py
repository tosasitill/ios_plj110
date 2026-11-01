#!/usr/bin/env python3
"""Apply the guarded X9 group-submit call-site port to the pinned Mesa export."""
from pathlib import Path
import difflib
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
relative='src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c'
p=w/'mesa-render-source'/relative
s=p.read_text()
marker='#include "X9PanVKQueueBridge.inc"'
if marker in s:
 print('X9 group-submit call-site port already present')
 raise SystemExit(0)
original=s
anchor='#define MIN_DESC_TRACEBUF_SIZE'
if s.count(anchor)!=1:raise RuntimeError('queue include anchor changed')
s=s.replace(anchor,'#ifdef X9_NATIVE_KMOD\n'+marker+'\n#endif\n\n'+anchor)
a='''   int ret = pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT,
                            &gsubmit);'''
b='''#ifdef X9_NATIVE_KMOD
   int ret = x9_panvk_group_submit(dev, &gsubmit);
#else
'''+a+'\n#endif'
a2='''   ret = pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gsubmit);'''
b2='''#ifdef X9_NATIVE_KMOD
   ret = x9_panvk_group_submit(dev, &gsubmit);
#else
'''+a2+'\n#endif'
if s.count(a)!=1 or s.count(a2)!=1:raise RuntimeError('group submit call sites changed')
s=s.replace(a,b).replace(a2,b2)
patch=''.join(difflib.unified_diff(original.splitlines(True),s.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative))
(w/'panvk-native-queue-calls.patch').write_text(patch)
p.write_text(s)
print('Guarded native init/main group-submit call sites applied; X9_NATIVE_KMOD remains opt-in')
