// SPDX-License-Identifier: MIT
// Original iOS16 Metal discovery factory. The rendering KMD must explicitly
// advertise readiness before any desktop renderer is offered this bundle.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <objc/runtime.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#import "NativeBridgeMetalFrontend.h"
#include "../X9MetalPolicy.h"
#include "X9RenderTransport.h"

#ifndef X9_PANVK_BUNDLE_PATH
#define X9_PANVK_BUNDLE_PATH "/System/Library/Extensions/X9PanVK170.bundle/X9PanVK"
#endif
@protocol MTLDeviceSPI <MTLDevice> @end
@interface _MTLDevice : NSObject @end
@interface X9GeneralMetalFactory : _MTLDevice <MTLDevice,MTLDeviceSPI>
- (instancetype)initWithAcceleratorPort:(unsigned)port;
@end
static void missing_selector(const char *object,const char *selector) {
 int fd=open("/dev/console",O_WRONLY|O_NOCTTY);
 if(fd>=0){dprintf(fd,"X9 GENERAL METAL: missing class=%s selector=%s\n",object,selector);close(fd);}
}
@implementation X9GeneralMetalFactory
- (instancetype)initWithAcceleratorPort:(unsigned)port {
 self=[super init];if(!self)return nil;
 if(!port)return nil;
 char args[2048]={0};size_t bytes=sizeof(args)-1;bool enabled=false;
 if(sysctlbyname("kern.bootargs",args,&bytes,NULL,0))return nil;
 char *save=NULL;
 for(char *token=strtok_r(args," \t\r\n",&save);token;token=strtok_r(NULL," \t\r\n",&save))
  if(!strcmp(token,"x9_metal_render=1"))enabled=true;
 if(!enabled)return nil;
 // Do not substitute the fixed compute service for a rendering backend.
 io_service_t render=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("SweetX9Render"));
 if(!render)return nil;
 CFTypeRef abi=IORegistryEntryCreateCFProperty(render,CFSTR("X9RenderABI"),kCFAllocatorDefault,0);
 CFTypeRef ready=IORegistryEntryCreateCFProperty(render,CFSTR("X9RenderReady"),kCFAllocatorDefault,0);
 int version=0;
 bool usable=abi&&CFGetTypeID(abi)==CFNumberGetTypeID()&&
  CFNumberGetValue((CFNumberRef)abi,kCFNumberIntType,&version)&&version==1&&
  ready&&CFGetTypeID(ready)==CFBooleanGetTypeID()&&CFBooleanGetValue((CFBooleanRef)ready);
 if(abi)CFRelease(abi);if(ready)CFRelease(ready);IOObjectRelease(render);
 if(!usable)return nil;
 // Registry metadata alone does not prove the caller can open the renderer.
 struct X9RenderCaps caps={0};
 struct X9RenderConnection *connection=x9_render_open(&caps);
 if(!connection)return nil;
 x9_render_close(connection);
 if(!caps.shader_present)return nil;
 native_bridge_metal_set_shader_core_mask(caps.shader_present);
 uint64_t registry=0;
 if(IORegistryEntryGetRegistryEntryID(port,&registry)!=KERN_SUCCESS)return nil;
 const char *cache=geteuid()==0?"/private/var/root/Library/Caches/X9GeneralMetal":"/private/var/mobile/Library/Caches/X9GeneralMetal";
 int entry_fd=open("/dev/console",O_WRONLY|O_NOCTTY);
 if(entry_fd>=0) {
  const char *image=class_getImageName([self class]);
  dprintf(entry_fd,"X9 GENERAL METAL: begin caller=%s image=%s loader=%s\n",
   getprogname(),image?image:"unknown",X9_PANVK_BUNDLE_PATH);close(entry_fd);
 }
 id device=native_bridge_metal_open(X9_PANVK_BUNDLE_PATH,cache,registry,missing_selector);
 int fd=open("/dev/console",O_WRONLY|O_NOCTTY);
 if(fd>=0){dprintf(fd,"X9 GENERAL METAL: factory caller=%s uid=%u registry=%llu device=%s\n",getprogname(),(unsigned)geteuid(),registry,device?class_getName([device class]):"nil");close(fd);}
 return device;
}
@end
