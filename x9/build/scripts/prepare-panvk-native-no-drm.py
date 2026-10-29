#!/usr/bin/env python3
"""Exclude Linux-only providers from the X9 native link; never fake DRM success."""
from pathlib import Path
import difflib
w=Path(__file__).resolve().parent;base=w/'mesa-render-source';patch=[]
def apply(rel,fn):
 p=base/rel;old=p.read_text();new=fn(old)
 if old!=new:
  patch.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+rel,tofile='b/'+rel));p.write_text(new)
def region(s,start,end,native):
 a=s.index(start);b=s.index(end,a)
 return s[:a]+'#ifdef X9_NATIVE_KMOD\n'+native+'\n#else\n'+s[a:b]+'\n#endif\n'+s[b:]
def kmod(s):
 if 'X9 native constructor routing' in s:return s
 s=s.replace('#include "pan_kmod_backend.h"','#include "pan_kmod_backend.h"\n#ifdef X9_NATIVE_KMOD\n#include "X9PanVKSync.h"\n#endif')
 a='extern const struct pan_kmod_ops panthor_kmod_ops;';b='DEBUG_GET_ONCE_OPTION'
 start=s.index(a);end=s.index(b,start)
 s=s[:start]+'#ifndef X9_NATIVE_KMOD\n'+s[start:end]+'#endif\n'+s[end:]
 a='   const char *selected_backends = debug_get_option_enabled_backends();';start=s.index(a);end=s.index('\n}\n',start)
 old=s[start:end];s=s[:start]+'''#ifdef X9_NATIVE_KMOD
   /* X9 native constructor routing: no DRM descriptor ownership. */
   if (fd >= 0 || (flags & PAN_KMOD_DEV_FLAG_OWNS_FD)) { errno = ENOTSUP; return NULL; }
   return x9_pan_kmod_create_native(allocator ? allocator : &default_allocator);
#else
'''+old+'\n#endif'+s[end:];return s
apply('src/panfrost/lib/kmod/pan_kmod.c',kmod)
def instance(s):
 if 'X9 never enumerates DRM nodes' in s:return s
 return region(s,'static VkResult\nenumerate_drm_physical_devices_locked','static VkResult\nenumerate_physical_devices_locked', '''/* X9 never enumerates DRM nodes; the instance installs a native callback. */
static VkResult enumerate_drm_physical_devices_locked(struct vk_instance *instance)
{ (void)instance; return VK_ERROR_INCOMPATIBLE_DRIVER; }
''')
apply('src/vulkan/runtime/vk_instance.c',instance)
def provider(s):
 if 'X9 has no DRM sync provider' in s:return s
 start=s.index('struct util_sync_provider_drm {')
 return s[:start]+'''#ifdef X9_NATIVE_KMOD
/* X9 has no DRM sync provider. Native callers construct X9PanVKSync. */
struct util_sync_provider *util_sync_provider_drm(int fd)
{ (void)fd; return NULL; }
#else
'''+s[start:]+'\n#endif\n'
apply('src/util/u_sync_provider.c',provider)
def wsi(s):
 if 'X9 does not advertise Vulkan WSI' in s:return s
 start=s.index('static VKAPI_PTR');return s[:start]+'''#ifdef X9_NATIVE_KMOD
/* X9 does not advertise Vulkan WSI; Metal presentation has its own path. */
VkResult panvk_wsi_init(struct panvk_physical_device *device)
{ (void)device; return VK_ERROR_EXTENSION_NOT_PRESENT; }
void panvk_wsi_finish(struct panvk_physical_device *device) { (void)device; }
#else
'''+s[start:]+'\n#endif\n'
apply('src/panfrost/vulkan/panvk_wsi.c',wsi)
def physical(s):
 a='''   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_physical_device_entrypoints, false);'''
 if '#ifndef X9_NATIVE_KMOD\n'+a in s:return s
 return s.replace(a,'#ifndef X9_NATIVE_KMOD\n'+a+'\n#endif')
apply('src/panfrost/vulkan/panvk_physical_device.c',physical)
def macros(s):
 if 'X9 only dispatches the verified GPU architecture' in s:return s
 a=s.index('#define panvk_arch_dispatch(');b=s.index('#ifdef PAN_ARCH',a)
 native='''#ifdef X9_NATIVE_KMOD
/* X9 only dispatches the verified GPU architecture. */
#define panvk_arch_dispatch(arch, name, ...) do { \\
 if ((arch) != 14) UNREACHABLE("non-v14 native GPU"); \\
 panvk_arch_name(name, v14)(__VA_ARGS__); } while (0)
#define panvk_arch_dispatch_ret(arch, name, ret, ...) do { \\
 if ((arch) != 14) UNREACHABLE("non-v14 native GPU"); \\
 ret = panvk_arch_name(name, v14)(__VA_ARGS__); } while (0)
#else
'''
 return s[:a]+native+s[a:b]+'#endif\n\n'+s[b:]
apply('src/panfrost/vulkan/panvk_macros.h',macros)
def meson(s):
 if 'x9_link_frameworks' in s:return s
 s=s.replace('foreach arch : [6, 7, 10, 11, 12, 13, 14]',"x9_archs = get_option('x9-native-kmod') ? [14] : [6, 7, 10, 11, 12, 13, 14]\nx9_link_frameworks = get_option('x9-native-kmod') ? ['-framework', 'IOKit', '-framework', 'CoreFoundation'] : []\nforeach arch : x9_archs")
 s=s.replace('link_args : [vulkan_icd_link_args,','link_args : [x9_link_frameworks, vulkan_icd_link_args,')
 return s
apply('src/panfrost/vulkan/meson.build',meson)
if patch:(w/'panvk-native-no-linux-backends.patch').write_text(''.join(patch))
print('Native build excludes legacy DRM construction/enumeration/providers and non-v14 dispatch; IOKit framework linked')
