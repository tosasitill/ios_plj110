#!/usr/bin/env python3
"""Guarded native logical-device construction and compiler CSF queries."""
from pathlib import Path
import difflib,subprocess
w=Path(__file__).resolve().parent
subprocess.run(['python3',str(w/'prepare-panvk-native-tiler.py')],check=True)
base=w/'mesa-render-source'
files=['src/panfrost/vulkan/panvk_vX_device.c']+['src/panfrost/vulkan/csf/'+n for n in ['panvk_vX_cmd_buffer.c','panvk_vX_exception_handler.c','panvk_vX_utrace.c','panvk_vX_cmd_draw.c']]
patch=[]
for relative in files:
 p=base/relative;s=p.read_text();old=s
 marker='/* X9 native compiler/device CSF access. */'
 if marker in s:continue
 # Put declaration before includes: our header is independent of PanVK types.
 s='#ifdef X9_NATIVE_KMOD\n'+marker+'\n#include "X9PanVKSync.h"\n#endif\n'+s
 a='''   const struct drm_panthor_csif_info *csif_info =
      panthor_kmod_get_csif_props('''
 if s.count(a)!=1:raise RuntimeError('CSF getter anchor changed: '+relative)
 start=s.index(a);end=s.index(';',start)+1
 original=s[start:end]
 callarg=original.split('panthor_kmod_get_csif_props(',1)[1].rsplit(')',1)[0]
 replacement='''#ifdef X9_NATIVE_KMOD
   const struct drm_panthor_csif_info *csif_info = x9_pan_kmod_csif('''+callarg+''');
#else
'''+original+'\n#endif'
 s=s[:start]+replacement+s[end:]
 if relative.endswith('/panvk_vX_device.c'):
  a='''   device->kmod.dev = pan_kmod_dev_create(
      os_dupfd_cloexec(physical_device->kmod.dev->fd),
      physical_device->kmod.dev->flags, &device->kmod.allocator);'''
  if s.count(a)!=1:raise RuntimeError('logical device constructor anchor changed')
  s=s.replace(a,'''#ifdef X9_NATIVE_KMOD
   device->kmod.dev = x9_pan_kmod_create_native(&device->kmod.allocator);
#else
'''+a+'\n#endif')
 patch.extend(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative))
 p.write_text(s)
if patch:(w/'panvk-native-device-compiler.patch').write_text(''.join(patch))
print('Native logical device and five compiler/device CSF query sites prepared')
