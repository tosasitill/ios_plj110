#!/usr/bin/env python3
"""Wire the split native tiler allocation into the guarded Mesa queue path."""
from pathlib import Path
import difflib,subprocess
w=Path(__file__).resolve().parent
subprocess.run(['python3',str(w/'prepare-panvk-native-groups.py')],check=True)
relative='src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c'
header='src/panfrost/vulkan/csf/panvk_queue.h'
p=w/'mesa-render-source'/relative;h=w/'mesa-render-source'/header
s=p.read_text();t=h.read_text();before=s;before_h=t
marker='/* X9 native split tiler heap. */'
if marker in s:
 print('Native tiler port already present');raise SystemExit(0)
a='''struct panvk_tiler_heap {
   uint32_t chunk_size;'''
b='''struct panvk_tiler_heap {
#ifdef X9_NATIVE_KMOD
   struct X9PanVKTilerHeap *x9_heap;
   uint64_t x9_address, x9_bytes;
#endif
   uint32_t chunk_size;'''
if t.count(a)!=1:raise RuntimeError('tiler struct anchor changed')
t=t.replace(a,b)
a='''   int ret = pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE,
                            &thc);'''
b='''#ifdef X9_NATIVE_KMOD
   '''+marker+'''
   int ret = x9_panvk_tiler_create(dev, tiler_heap, &thc);
#else
'''+a+'\n#endif'
if s.count(a)!=1:raise RuntimeError('tiler create anchor changed')
s=s.replace(a,b)
a='''   ASSERTED int ret =
      pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &thd);
   assert(!ret);'''
b='''#ifdef X9_NATIVE_KMOD
   (void)thd;
   if (x9_panvk_tiler_destroy(dev, tiler_heap)) {
      mesa_loge("X9 tiler teardown uncertain; heap and descriptor references retained");
      return;
   }
#else
'''+a+'\n#endif'
if s.count(a)!=1:raise RuntimeError('tiler destroy anchor changed')
s=s.replace(a,b)
patch=''.join(difflib.unified_diff(before_h.splitlines(True),t.splitlines(True),fromfile='a/'+header,tofile='b/'+header))+''.join(difflib.unified_diff(before.splitlines(True),s.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative))
(w/'panvk-native-tiler-calls.patch').write_text(patch)
h.write_text(t);p.write_text(s)
print('Native split tiler allocation wired to actual PanVK queue init/cleanup; no runtime execution')
