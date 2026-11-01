#!/usr/bin/env python3
"""Guarded IOKit physical enumeration and non-coherent native heap policy."""
from pathlib import Path
import difflib,subprocess
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
subprocess.run(['python3',str(w/'prepare-panvk-native-device.py')],check=True)
base=w/'mesa-render-source';changes=[]
def save(relative,old,new):
 if old!=new:
  changes.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative))
  (base/relative).write_text(new)
def one(s,a,b):
 if s.count(a)!=1:raise RuntimeError('anchor changed: '+a[:100])
 return s.replace(a,b)
relative='src/panfrost/vulkan/panvk_physical_device.c'
p=base/relative;old=p.read_text();s=old
marker='/* X9 IOKit physical device path. */'
if marker not in s:
 s='#ifdef X9_NATIVE_KMOD\n'+marker+'\n#include "X9PanVKSync.h"\n#endif\n'+s
 anchor='''   const char *path = drm_device->nodes[DRM_NODE_RENDER];'''
 start=s.index(anchor);end=s.index('\n}\n',start)
 original=s[start:end]
 native='''#ifdef X9_NATIVE_KMOD
   (void)drm_device;
   device->kmod.dev = x9_pan_kmod_create_native(&instance->kmod.allocator);
   if (!device->kmod.dev)
      return errno == ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_INCOMPATIBLE_DRIVER;
   return VK_SUCCESS;
#else
'''+original+'\n#endif'
 s=s[:start]+native+s[end:]
 anchor='''   int host_coherent_not_cached_idx = -1;'''
 start=s.index(anchor);end=s.index('\n}\n',start);original=s[start:end]
 native='''#ifdef X9_NATIVE_KMOD
   (void)instance;
   struct X9RenderCaps caps;
   if (x9_pan_kmod_caps(device->kmod.dev, &caps))
      return VK_ERROR_INITIALIZATION_FAILED;
   device->memory.heap_count = 1;
   device->memory.heaps[0] = (VkMemoryHeap){.size = caps.max_client_bytes,
      .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
   device->memory.type_count = 2;
   device->memory.types[0] = (VkMemoryType){.propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, .heapIndex = 0};
   device->memory.types[1] = (VkMemoryType){.propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, .heapIndex = 0};
   /* No HOST_COHERENT: native CPU maps require explicit cache maintenance. */
   device->memory.max_supported_va = caps.va_end;
   /* Single native GPU, per-process shared budget record; no DRM identity. */
   device->memory.budget = get_memory_budget(0);
   return device->memory.budget ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
#else
'''+original+'\n#endif'
 s=s[:start]+native+s[end:]
 a='''   result = get_drm_device_ids(device, instance, drm_device);
   if (result != VK_SUCCESS)
      goto fail;'''
 s=one(s,a,'#ifndef X9_NATIVE_KMOD\n'+a+'\n#endif')
 a='''      if (!os_get_option("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER")) {'''
 s=one(s,a,'''#ifdef X9_NATIVE_KMOD
      /* Native open accepts only the exact X9 v14 GPU and ready render ABI. */
      if (arch != 14 && !os_get_option("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER")) {
#else
'''+a+'\n#endif')
 a='''   result = panvk_wsi_init(device);
   if (result != VK_SUCCESS)
      goto fail;'''
 s=one(s,a,'#ifndef X9_NATIVE_KMOD\n'+a+'\n#endif')
 s=one(s,'   panvk_wsi_finish(device);','#ifndef X9_NATIVE_KMOD\n   panvk_wsi_finish(device);\n#endif')
 save(relative,old,s)
relative='src/panfrost/vulkan/panvk_instance.c';p=base/relative;old=p.read_text();s=old
marker='/* X9 IOKit enumeration; no synthetic DRM device/node. */'
if marker not in s:
 anchor='static void\npanvk_destroy_physical_device'
 helper='''#ifdef X9_NATIVE_KMOD
'''+marker+'''
static VkResult
panvk_enumerate_native_devices(struct vk_instance *instance)
{
   struct vk_physical_device *device = NULL;
   VkResult result = panvk_physical_device_try_create(instance, NULL, &device);
   if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
      return VK_SUCCESS;
   if (result != VK_SUCCESS)
      return result;
   list_addtail(&device->link, &instance->physical_devices.list);
   return VK_SUCCESS;
}
#endif

'''
 s=one(s,anchor,helper+anchor)
 a='''   instance->vk.physical_devices.try_create_for_drm =
      panvk_physical_device_try_create;'''
 s=one(s,a,'''#ifdef X9_NATIVE_KMOD
   instance->vk.physical_devices.enumerate = panvk_enumerate_native_devices;
#else
'''+a+'\n#endif')
 s=one(s,'#ifndef VK_USE_PLATFORM_WIN32_KHR\n   .EXT_headless_surface = true,','#if !defined(VK_USE_PLATFORM_WIN32_KHR) && !defined(X9_NATIVE_KMOD)\n   .EXT_headless_surface = true,')
 save(relative,old,s)
relative='src/panfrost/vulkan/panvk_vX_physical_device.c';p=base/relative;old=p.read_text();s=old
marker='/* X9 has no DRM fd sharing or Vulkan WSI integration. */'
if marker not in s:
 anchor='''      .ARM_rasterization_order_attachment_access = PAN_ARCH >= 10,
   };'''
 fields=['KHR_external_fence_fd','KHR_external_memory_fd','KHR_external_semaphore_fd','EXT_external_memory_dma_buf','EXT_physical_device_drm','KHR_swapchain','KHR_swapchain_mutable_format','KHR_swapchain_maintenance1','EXT_memory_budget']
 addition='\n#ifdef X9_NATIVE_KMOD\n   '+marker+'\n'+''.join('   ext->'+f+' = false;\n' for f in fields)+'#endif'
 s=one(s,anchor,anchor+addition);save(relative,old,s)
if changes:(w/'panvk-native-physical-discovery.patch').write_text(''.join(changes))
print('Native physical enumeration/heap/extension policy prepared; guarded and not deployed')
