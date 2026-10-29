#!/usr/bin/env python3
"""Wire host-memory imports used by IOSurface textures into native PanVK."""
from pathlib import Path
import difflib
w=Path(__file__).resolve().parent;base=w/'mesa-render-source';patch=[]
def save(rel,old,new):
 if old!=new:
  patch.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+rel,tofile='b/'+rel));(base/rel).write_text(new)
rel='src/panfrost/vulkan/panvk_device_memory.c';p=base/rel;old=p.read_text();s=old
marker='/* X9 native host memory import. */'
if marker not in s:
 s='#ifdef X9_NATIVE_KMOD\n#include "X9PanVKMemory.h"\n#endif\n'+s
 a='''   if (fd_info) {
      assert('''
 b='''#ifdef X9_NATIVE_KMOD
   '''+marker+'''
   const VkImportMemoryHostPointerInfoEXT *host_info =
      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_HOST_POINTER_INFO_EXT);
   if (host_info && host_info->handleType) {
      if (fd_info || can_be_exported || host_info->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT ||
          !(type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
         result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         goto err_destroy_mem;
      }
      mem->bo = x9_pan_kmod_import_host(device->kmod.dev, device->kmod.vm,
                                        host_info->pHostPointer, pAllocateInfo->allocationSize);
      if (!mem->bo) {
         result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         goto err_destroy_mem;
      }
   } else
#endif
'''+a
 if s.count(a)!=1:raise RuntimeError('host import allocation anchor')
 s=s.replace(a,b)
 s=s.replace('''         os_munmap(mem->debug.host_mapping, pan_kmod_bo_size(mem->bo));''','''#ifdef X9_NATIVE_KMOD
         x9_pan_kmod_unmap_cpu(mem->bo, mem->debug.host_mapping);
#else
         os_munmap(mem->debug.host_mapping, pan_kmod_bo_size(mem->bo));
#endif''')
 s=s.replace('''      ASSERTED int ret = os_munmap(mem->addr.host, pan_kmod_bo_size(mem->bo));''','''#ifdef X9_NATIVE_KMOD
      ASSERTED int ret = x9_pan_kmod_unmap_cpu(mem->bo, mem->addr.host);
#else
      ASSERTED int ret = os_munmap(mem->addr.host, pan_kmod_bo_size(mem->bo));
#endif''')
 # Do not place an if body directly across a preprocessor block without braces.
 s=s.replace('''      if (mem->debug.host_mapping)
#ifdef X9_NATIVE_KMOD''','''      if (mem->debug.host_mapping) {
#ifdef X9_NATIVE_KMOD''')
 s=s.replace('''         os_munmap(mem->debug.host_mapping, pan_kmod_bo_size(mem->bo));
#endif
   }''','''         os_munmap(mem->debug.host_mapping, pan_kmod_bo_size(mem->bo));
#endif
      }
   }''')
 s+='''
#ifdef X9_NATIVE_KMOD
VKAPI_ATTR VkResult VKAPI_CALL
panvk_GetMemoryHostPointerPropertiesEXT(VkDevice _device,
 VkExternalMemoryHandleTypeFlagBits type, const void *pointer,
 VkMemoryHostPointerPropertiesEXT *properties)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   const struct panvk_physical_device *physical = to_panvk_physical_device(device->vk.physical);
   if (!properties || !pointer || ((uintptr_t)pointer & 16383) ||
       type != VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   properties->memoryTypeBits = 0;
   for (uint32_t i = 0; i < physical->memory.type_count; i++)
      if (physical->memory.types[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
         properties->memoryTypeBits |= 1u << i;
   return properties->memoryTypeBits ? VK_SUCCESS : VK_ERROR_INVALID_EXTERNAL_HANDLE;
}
#endif
'''
 save(rel,old,s)
rel='src/panfrost/vulkan/panvk_vX_physical_device.c';p=base/rel;old=p.read_text();s=old
marker='/* X9 host import alignment matches native IOMemoryDescriptor pinning. */'
if marker not in s:
 a='   /* X9 has no DRM fd sharing or Vulkan WSI integration. */'
 if s.count(a)!=1:raise RuntimeError('native extensions anchor')
 s=s.replace(a,a+'\n   ext->EXT_external_memory_host = true;')
 a='   /* VK_EXT_physical_device_drm */'
 if s.count(a)!=1:raise RuntimeError('properties anchor')
 s=s.replace(a,'#ifdef X9_NATIVE_KMOD\n   '+marker+'\n   properties->minImportedHostPointerAlignment = 16384;\n#endif\n\n'+a)
 save(rel,old,s)
rel='src/panfrost/vulkan/panvk_physical_device.c';p=base/rel;old=p.read_text();s=old
marker='/* X9 imports host allocations; fd sharing remains unsupported. */'
if marker not in s:
 a='''   const VkExternalMemoryHandleTypeFlags supported_handle_types =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;'''
 # Image properties and buffer properties each have a separate policy. Only
 # buffer host imports are advertised: IOSurface uses buffer-backed textures.
 start=s.index('panvk_GetPhysicalDeviceExternalBufferProperties(')
 before=s[:start];tail=s[start:]
 if tail.count(a)!=1:raise RuntimeError('external buffer anchor')
 tail=tail.replace(a,'''#ifdef X9_NATIVE_KMOD
   '''+marker+'''
   pExternalBufferProperties->externalMemoryProperties = (VkExternalMemoryProperties){
      .compatibleHandleTypes = pExternalBufferInfo->handleType,
      .externalMemoryFeatures = pExternalBufferInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT
         ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT : 0,
   };
   return;
#endif
'''+a)
 s=before+tail;save(rel,old,s)
if patch:(w/'panvk-native-host-import.patch').write_text(''.join(patch))
print('IOSurface host-allocation import and non-owning CPU unmap paths prepared')
