// SPDX-License-Identifier: MIT
// Native backend discovery for the Metal runtime: the only file at the Metal
// boundary that knows the native render service and the installed loader.
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include "NativeBackendProbe.h"
#include "../NativeBridgeMetalFrontend.h"
#include "../X9RenderTransport.h"

#ifndef X9_PANVK_BUNDLE_PATH
#define X9_PANVK_BUNDLE_PATH "/System/Library/Extensions/X9PanVK170.bundle/X9PanVK"
#endif

extern "C" bool nmb_backend_probe(struct NmbBackendProbe *out) {
 if(!out)return false;
 bzero(out,sizeof(*out));
 io_service_t render=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("SweetX9Render"));
 if(!render)return false;
 CFTypeRef abi=IORegistryEntryCreateCFProperty(render,CFSTR("X9RenderABI"),kCFAllocatorDefault,0);
 CFTypeRef ready=IORegistryEntryCreateCFProperty(render,CFSTR("X9RenderReady"),kCFAllocatorDefault,0);
 int version=0;
 bool usable=abi&&CFGetTypeID(abi)==CFNumberGetTypeID()&&
  CFNumberGetValue((CFNumberRef)abi,kCFNumberIntType,&version)&&version==1&&
  ready&&CFGetTypeID(ready)==CFBooleanGetTypeID()&&CFBooleanGetValue((CFBooleanRef)ready);
 if(abi)CFRelease(abi);if(ready)CFRelease(ready);IOObjectRelease(render);
 if(!usable)return false;
 // Registry metadata alone does not prove the caller can open the renderer.
 struct X9RenderCaps caps={0};
 struct X9RenderConnection *connection=x9_render_open(&caps);
 if(!connection)return false;
 x9_render_close(connection);
 if(!caps.shader_present)return false;
 out->shader_present=caps.shader_present;
 return true;
}

extern "C" const char *nmb_backend_loader_path(void) { return X9_PANVK_BUNDLE_PATH; }

extern "C" id nmb_backend_open(const char *cache,unsigned accelerator_port,
                               void (*missing)(const char *,const char *)) {
 uint64_t registry=0;
 if(IORegistryEntryGetRegistryEntryID(accelerator_port,&registry)!=KERN_SUCCESS)return nil;
 return native_bridge_metal_open(X9_PANVK_BUNDLE_PATH,cache,registry,missing);
}
