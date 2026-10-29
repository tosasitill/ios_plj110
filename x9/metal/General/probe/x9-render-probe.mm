// SPDX-License-Identifier: MIT
// Owner-run boot probe; never executed by the host build.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <objc/runtime.h>
#include "x9_render_metallib.inc"
static bool finish(id<MTLCommandBuffer> cb,const char *phase) {
 [cb commit];[cb waitUntilCompleted];
 if(cb.status!=MTLCommandBufferStatusCompleted||cb.error) {
  fprintf(stderr,"X9 RENDER PROBE: FAIL phase=%s status=%lu error=%s\n",phase,(unsigned long)cb.status,cb.error.description.UTF8String);return false;
 }
 return true;
}
int main(void) {@autoreleasepool {
 setbuf(stdout,NULL);setbuf(stderr,NULL);
 fprintf(stderr,"X9 RENDER PROBE: BEGIN uid=%u native system Metal/AIR/PanVK render, no CPU expected-output writes\n",(unsigned)geteuid());
 id<MTLDevice> device=MTLCreateSystemDefaultDevice();
 if(!device){fprintf(stderr,"X9 RENDER PROBE: FAIL system default Metal device nil\n");return 1;}
 fprintf(stderr,"X9 RENDER PROBE: factory class=%s name=%s\n",class_getName([device class]),device.name.UTF8String);
 if(strcmp(class_getName([device class]),"X9GeneralMetalDevice")){fprintf(stderr,"X9 RENDER PROBE: FAIL unexpected device\n");return 2;}
 dispatch_data_t data=dispatch_data_create(x9_render_metallib,sizeof(x9_render_metallib),dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),DISPATCH_DATA_DESTRUCTOR_DEFAULT);
 NSError *error=nil;id<MTLLibrary> library=[device newLibraryWithData:data error:&error];
 if(!library){fprintf(stderr,"X9 RENDER PROBE: FAIL metallib %s\n",error.description.UTF8String);return 3;}
 id<MTLCommandQueue> queue=[device newCommandQueue];
 id<MTLComputePipelineState> compute=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"x9_cache_roundtrip"] error:&error];
 id<MTLBuffer> input=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
 id<MTLBuffer> output=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
 if(!queue||!compute||!input||!output){fprintf(stderr,"X9 RENDER PROBE: FAIL compute allocation/pipeline %s\n",error.description.UTF8String);return 4;}
 for(unsigned round=0;round<3;round++) {
  uint32_t *a=(uint32_t *)input.contents,*b=(uint32_t *)output.contents;
  if(!a||!b){fprintf(stderr,"X9 RENDER PROBE: FAIL host mapping\n");return 5;}
  for(unsigned i=0;i<256;i++){a[i]=round*0x10001u+i*17u;b[i]=0xdeadbeefu;}
  id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[cb computeCommandEncoder];
  if(!cb||!encoder){fprintf(stderr,"X9 RENDER PROBE: FAIL compute command encoder\n");return 6;}
  [encoder setComputePipelineState:compute];[encoder setBuffer:input offset:0 atIndex:0];[encoder setBuffer:output offset:0 atIndex:1];
  [encoder dispatchThreadgroups:MTLSizeMake(8,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[encoder endEncoding];
  if(!finish(cb,"compute-cache-roundtrip"))return 7;
  unsigned bad=0;for(unsigned i=0;i<256;i++)if(b[i]!=(round*0x10001u+i*17u)*3u+7u)bad++;
  fprintf(stderr,"X9 RENDER PROBE: cache round=%u correct=%u/256\n",round,256-bad);if(bad)return 8;
 }
 NSDictionary *properties=@{(id)kIOSurfaceWidth:@64,(id)kIOSurfaceHeight:@64,(id)kIOSurfaceBytesPerElement:@4,
  (id)kIOSurfaceBytesPerRow:@256,(id)kIOSurfaceAllocSize:@16384,(id)kIOSurfacePixelFormat:@((uint32_t)0x42475241)};
 IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)properties);
 if(!surface){fprintf(stderr,"X9 RENDER PROBE: FAIL IOSurfaceCreate\n");return 9;}
 void *base=IOSurfaceGetBaseAddress(surface);size_t stride=IOSurfaceGetBytesPerRow(surface);
 if(!base||stride!=256||IOSurfaceGetAllocSize(surface)<16384){CFRelease(surface);return 10;}
 memset(base,0x5a,16384);
 MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
 desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
 id<MTLTexture> target=[device newTextureWithDescriptor:desc iosurface:surface plane:0];
 MTLRenderPipelineDescriptor *pipeline=[MTLRenderPipelineDescriptor new];
 pipeline.vertexFunction=[library newFunctionWithName:@"x9_fullscreen_vertex"];
 pipeline.fragmentFunction=[library newFunctionWithName:@"x9_magenta_fragment"];
 pipeline.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
 id<MTLRenderPipelineState> render=[device newRenderPipelineStateWithDescriptor:pipeline error:&error];
 if(!target||!render){fprintf(stderr,"X9 RENDER PROBE: FAIL imported render texture/pipeline %s\n",error.description.UTF8String);CFRelease(surface);return 11;}
 MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
 pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
 pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
 id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLRenderCommandEncoder> encoder=[cb renderCommandEncoderWithDescriptor:pass];
 if(!cb||!encoder){CFRelease(surface);fprintf(stderr,"X9 RENDER PROBE: FAIL render command encoder\n");return 12;}
 [encoder setRenderPipelineState:render];[encoder setViewport:(MTLViewport){0,0,64,64,0,1}];
 [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];
 if(!finish(cb,"triangle-to-IOSurface")){CFRelease(surface);return 13;}
 unsigned bad=0;const uint32_t *pixels=(const uint32_t *)base;
 for(unsigned i=0;i<4096;i++)if(pixels[i]!=0xffff00ffu)bad++;
 fprintf(stderr,"X9 RENDER PROBE: triangle IOSurface=%u correct=%u/4096 expected=ffff00ff\n",IOSurfaceGetID(surface),4096-bad);
 CFRelease(surface);
 if(bad)return 14;
 fprintf(stderr,"X9 RENDER PROBE: PASS 3 cache roundtrips +4096 GPU-rasterized imported pixels; desktop rendering checked separately\n");
 return 0;
}}
