// SPDX-License-Identifier: MIT
// Original iOS16 Metal discovery factory. The rendering KMD must explicitly
// advertise readiness before any desktop renderer is offered this bundle.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#import "NativeBridgeMetalFrontend.h"
#include "bridge/NativeBackendProbe.h"
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
 // Do not substitute the fixed compute service for a rendering backend. The
 // bridge owns the native probe; this layer sees only the answers.
 struct NmbBackendProbe probe={0};
 if(!nmb_backend_probe(&probe))return nil;
 native_bridge_metal_set_shader_core_mask(probe.shader_present);
 const char *cache=geteuid()==0?"/private/var/root/Library/Caches/X9GeneralMetal":"/private/var/mobile/Library/Caches/X9GeneralMetal";
 int entry_fd=open("/dev/console",O_WRONLY|O_NOCTTY);
 if(entry_fd>=0) {
  const char *image=class_getImageName([self class]);
  dprintf(entry_fd,"X9 GENERAL METAL: begin caller=%s image=%s loader=%s\n",
   getprogname(),image?image:"unknown",nmb_backend_loader_path());close(entry_fd);
 }
 id device=nmb_backend_open(cache,port,missing_selector);
 int fd=open("/dev/console",O_WRONLY|O_NOCTTY);
 if(fd>=0){dprintf(fd,"X9 GENERAL METAL: factory caller=%s uid=%u port=%u device=%s\n",getprogname(),(unsigned)geteuid(),port,device?class_getName([device class]):"nil");close(fd);}
 return device;
}
@end
