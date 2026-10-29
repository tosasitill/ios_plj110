#import "X9Metal.h"
#import "X9MetalABI.h"
#import "X9MetalPolicy.h"
#import <IOSurface/IOSurfaceRef.h>
#import <IOKit/IOKitLib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#ifdef X9_SYSTEM_PLUGIN
@protocol MTLDeviceSPI <MTLDevice> @end
@interface _MTLDevice : NSObject @end
#define X9DeviceSuperclass _MTLDevice
#define X9DeviceProtocols MTLDevice,MTLDeviceSPI
#else
#define X9DeviceSuperclass NSObject
#define X9DeviceProtocols MTLDevice
#endif

static NSError *X9Error(NSInteger code,NSString *message) {
 return [NSError errorWithDomain:@"org.heliodarwin.X9Metal" code:code
  userInfo:@{NSLocalizedDescriptionKey:message}];
}
@class X9Device,X9Queue,X9CommandBuffer;
@interface X9Buffer : NSObject <MTLBuffer>
@property(nonatomic,strong) X9Device *owner;
@property(nonatomic,strong) NSMutableData *data;
@property(atomic,copy) NSString *label;
@end
@implementation X9Buffer
- (id<MTLDevice>)device { return (id)_owner; }
- (void *)contents { return _data.mutableBytes; }
- (NSUInteger)length { return _data.length; }
- (NSUInteger)allocatedSize { return _data.length; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeTracked; }
- (MTLResourceOptions)resourceOptions { return MTLResourceStorageModeShared; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return MTLPurgeableStateNonVolatile; }
- (uint64_t)gpuAddress { return 0; } // No user-visible GPU VA is exported.
@end
@interface X9Function : NSObject <MTLFunction>
@property(nonatomic,strong) X9Device *owner;
@property(atomic,copy) NSString *label;
@end
@implementation X9Function
- (id<MTLDevice>)device { return (id)_owner; }
- (NSString *)name { return @"add_u32"; }
- (MTLFunctionType)functionType { return MTLFunctionTypeKernel; }
- (NSDictionary *)functionConstantsDictionary { return @{}; }
- (NSArray *)stageInputAttributes { return @[]; }
- (NSArray *)vertexAttributes { return @[]; }
@end
@interface X9Library : NSObject <MTLLibrary>
@property(nonatomic,strong) X9Device *owner;
@property(atomic,copy) NSString *label;
@end
@implementation X9Library
- (id<MTLDevice>)device { return (id)_owner; }
- (NSArray<NSString *> *)functionNames { return @[@"add_u32"]; }
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
 if(![name isEqualToString:@"add_u32"])return nil;
 X9Function *f=[X9Function new];f.owner=_owner;return f;
}
- (MTLLibraryType)type { return MTLLibraryTypeExecutable; }
- (NSString *)installName { return nil; }
@end
@interface X9Pipeline : NSObject <MTLComputePipelineState>
@property(nonatomic,strong) X9Device *owner;
@property(nonatomic,strong) NSString *label;
@end
@implementation X9Pipeline
- (id<MTLDevice>)device { return (id)_owner; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 128; }
- (NSUInteger)threadExecutionWidth { return 16; }
- (NSUInteger)staticThreadgroupMemoryLength { return 0; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
- (MTLSize)maxTotalThreadsPerThreadgroupSize { return MTLSizeMake(128,1,1); }
@end
@interface X9Device : X9DeviceSuperclass <X9DeviceProtocols>
@property(nonatomic,assign) io_connect_t connection;
@property(nonatomic,assign) uint64_t registryIdentity;
@property(nonatomic,strong) NSLock *lock;
@property(nonatomic,strong) NSDictionary *computeLimits;
@property(nonatomic,strong) NSDictionary *computeCapabilities;
@property(atomic,copy) NSString *label;
- (BOOL)execute:(NSDictionary *)op error:(NSError **)error;
@end
@interface X9Queue : NSObject <MTLCommandQueue>
@property(nonatomic,strong) X9Device *owner;
@property(atomic,copy) NSString *label;
@end
@interface X9CommandBuffer : NSObject <MTLCommandBuffer>
@property(nonatomic,strong) X9Queue *queue;
@property(atomic,copy) NSString *label;
@property(nonatomic,assign) MTLCommandBufferStatus status;
@property(nonatomic,strong) NSError *error;
@property(nonatomic,strong) NSMutableArray *operations;
@property(nonatomic,strong) NSMutableArray *scheduled;
@property(nonatomic,strong) NSMutableArray *completed;
@property(nonatomic,assign) BOOL encoding;
- (void)fail:(NSString *)message;
@end
@interface X9Encoder : NSObject <MTLComputeCommandEncoder>
@property(nonatomic,strong) X9CommandBuffer *command;
@property(nonatomic,strong) X9Pipeline *pipeline;
@property(nonatomic,strong) NSMutableDictionary *bindings;
@property(atomic,copy) NSString *label;
@property(nonatomic,assign) BOOL ended;
@end
@implementation X9Device
/* Original20H392 MTLAddDevice invokes these private driver initializers.
 * The inherited implementation derives a full Apple profile via featureProfile;
 * this compute-only backend owns its capabilities and implements its own limits.
 * No inherited Apple limits/feature table is exposed as hardware support. */
- (void)initLimits {
 self.computeLimits=@{@"maxBufferLength":@4096,@"maxElements":@256,@"maxLocalX":@128};
 int fd=open("/dev/console",O_WRONLY|O_NOCTTY);
 if(fd>=0){const char line[]="X9 METAL DEVICE: initLimits buffer4096 elements256 local128 compute-only\n";write(fd,line,sizeof(line)-1);close(fd);}
}
- (void)initFeatureQueries {
 self.computeCapabilities=@{@"nativeAddU32":@YES,@"sharedStaging":@YES,@"render":@NO,@"MSL":@NO,@"AIR":@NO};
 int fd=open("/dev/console",O_WRONLY|O_NOCTTY);
 if(fd>=0){const char line[]="X9 METAL DEVICE: initFeatureQueries built-in compute; no Apple GPU family/profile\n";write(fd,line,sizeof(line)-1);close(fd);}
}
#ifdef X9_SYSTEM_PLUGIN
- (instancetype)initWithAcceleratorPort:(unsigned)port {
 self=[super init];if(!self)return nil;
 /* Parent init must precede every ARC nil-return: its dealloc releases
  * dispatch objects created by init. Pan159 crashed backboardd here.
  * Metal discovery is system-wide for GPU-permitted compute clients.
  * Keep rendering owners on their existing fallback until render is implemented. */
 if(!port||x9_metal_render_owner(getprogname()))return nil;
 char args[2048]={0};size_t bytes=sizeof(args)-1;unsigned mode=0;
 if(sysctlbyname("kern.bootargs",args,&bytes,NULL,0))return nil;
 char *save=NULL;for(char *word=strtok_r(args," \t\r\n",&save);word;word=strtok_r(NULL," \t\r\n",&save))
  {unsigned parsed=x9_metal_mode_from_token(word);if(parsed)mode=parsed;}
 if(!mode)return nil;
 int diagnostic=open("/dev/console",O_WRONLY|O_NOCTTY);
 if(diagnostic>=0){dprintf(diagnostic,"X9 METAL DEVICE: system mode=%u accepted caller=%s\n",mode,getprogname());close(diagnostic);}
 NSError *error=nil;X9Device *connected=(id)X9MetalCreateDevice(&error);
 if(!connected) {
  int diagnostic=open("/dev/console",O_WRONLY|O_NOCTTY);
  if(diagnostic>=0){dprintf(diagnostic,"X9 METAL DEVICE: native connection failed reason=%s\n",error.localizedDescription.UTF8String);close(diagnostic);}
  return nil;
 }
 _connection=connected.connection;connected.connection=0;_lock=connected.lock;
 (void)IORegistryEntryGetRegistryEntryID(port,&_registryIdentity);
 return self;
}
#endif
- (void)dealloc { if(_connection)IOServiceClose(_connection); }
- (NSString *)name { return @"MT6993 X9 system compute (add_u32)"; }
- (BOOL)hasUnifiedMemory { return YES; }
- (uint64_t)registryID { return _registryIdentity; }
- (BOOL)isLowPower { return NO; }
- (BOOL)isRemovable { return NO; }
- (BOOL)isHeadless { return YES; }
- (NSUInteger)maxBufferLength { return 4096; }
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(128,1,1); }
- (uint64_t)recommendedMaxWorkingSetSize { return 4096*16; }
- (BOOL)supportsFamily:(MTLGPUFamily)family { return NO; }
- (BOOL)supportsFeatureSet:(MTLFeatureSet)featureSet { return NO; }
- (id<MTLCommandQueue>)newCommandQueue {
 X9Queue *q=[X9Queue new];q.owner=self;return q;
}
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)count {
 return count?[self newCommandQueue]:nil;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
 if(!length||length>4096||options!=MTLResourceStorageModeShared)return nil;
 X9Buffer *b=[X9Buffer new];b.owner=self;b.data=[NSMutableData dataWithLength:length];return b;
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
 if(!bytes)return nil;
 id<MTLBuffer> b=[self newBufferWithLength:length options:options];
 if(b)memcpy(b.contents,bytes,length);return b;
}
- (id<MTLLibrary>)newDefaultLibrary {
 X9Library *l=[X9Library new];l.owner=self;return l;
}
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error {
 if(error)*error=X9Error(1,@"MSL compilation is not implemented; use the built-in native add_u32 library.");return nil;
}
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
 if(error)*error=X9Error(1,@"AIR/metallib input is not implemented by this native compute bridge.");return nil;
}
/* System callers receive explicit unsupported results for pipelines and
 * resources outside the native compute subset, never placeholder success. */
- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error {
 if(error)*error=X9Error(1,@"File-based metallib/AIR compilation is not implemented.");return nil;
}
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error {
 if(error)*error=X9Error(1,@"URL-based metallib/AIR compilation is not implemented.");return nil;
}
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error {
 if(error)*error=X9Error(1,@"Application default.metallib is not implemented; native library exposes add_u32 only.");return nil;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error {
 if(error)*error=X9Error(6,@"Rendering is not implemented by the native compute device.");return nil;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
 options:(MTLPipelineOption)options reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 return [self newRenderPipelineStateWithDescriptor:descriptor error:error];
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor { return nil; }
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane { return nil; }
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor { return nil; }
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)descriptor { return nil; }
- (BOOL)supportsTextureSampleCount:(NSUInteger)count { return NO; }
- (NSUInteger)maxThreadgroupMemoryLength { return 0; }
- (MTLReadWriteTextureTier)readWriteTextureSupport { return MTLReadWriteTextureTierNone; }
- (MTLArgumentBuffersTier)argumentBuffersSupport { return MTLArgumentBuffersTier1; }
- (BOOL)areRasterOrderGroupsSupported { return NO; }
- (BOOL)supportsDynamicLibraries { return NO; }
- (BOOL)supportsRenderDynamicLibraries { return NO; }
- (BOOL)supportsFunctionPointers { return NO; }
- (BOOL)supportsRaytracing { return NO; }
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error {
 if(![function isKindOfClass:[X9Function class]]||[(X9Function *)function owner]!=self) {
  if(error)*error=X9Error(2,@"Only this device's built-in add_u32 kernel is supported.");return nil;
 }
 X9Pipeline *p=[X9Pipeline new];p.owner=self;return p;
}
- (BOOL)execute:(NSDictionary *)op error:(NSError **)error {
 struct X9MetalRequest request={0};struct X9MetalResponse response={0};
 request.abi=X9_METAL_ABI;request.count=[op[@"count"] unsignedIntValue];request.local_x=[op[@"local"] unsignedIntValue];
 X9Buffer *a=op[@"a"],*b=op[@"b"],*c=op[@"c"];
 NSUInteger ao=[op[@"ao"] unsignedIntegerValue],bo=[op[@"bo"] unsignedIntegerValue],co=[op[@"co"] unsignedIntegerValue];
 NSUInteger bytes=request.count*4;
 if(!request.count||request.count>X9_METAL_MAX||!request.local_x||request.local_x>128||request.count%request.local_x||
    ao>a.length||bytes>a.length-ao||bo>b.length||bytes>b.length-bo||co>c.length||bytes>c.length-co) {
  if(error)*error=X9Error(3,@"Invalid dispatch geometry or buffer range.");return NO;
 }
 memcpy(request.a,(const char *)a.contents+ao,bytes);memcpy(request.b,(const char *)b.contents+bo,bytes);
 size_t outputBytes=sizeof(response);[_lock lock];
 kern_return_t kr=IOConnectCallStructMethod(_connection,1,&request,sizeof(request),&response,&outputBytes);
 [_lock unlock];
 if(kr!=KERN_SUCCESS||outputBytes!=sizeof(response)||response.abi!=X9_METAL_ABI||response.status||response.count!=request.count) {
  if(error)*error=X9Error(4,[NSString stringWithFormat:@"GPU submission/readback failed kr=0x%x; output is not replaced.",kr]);return NO;
 }
 memcpy((char *)c.contents+co,response.c,bytes);return YES;
}
@end
@implementation X9Queue
- (id<MTLDevice>)device { return _owner; }
- (id<MTLCommandBuffer>)commandBuffer {
 X9CommandBuffer *c=[X9CommandBuffer new];c.queue=self;c.operations=[NSMutableArray new];
 c.scheduled=[NSMutableArray new];c.completed=[NSMutableArray new];return c;
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [self commandBuffer]; }
@end
@implementation X9CommandBuffer
- (id<MTLDevice>)device { return _queue.owner; }
- (id<MTLCommandQueue>)commandQueue { return _queue; }
- (BOOL)retainedReferences { return YES; }
- (void)fail:(NSString *)message { if(!_error)_error=X9Error(5,message); }
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
 if(_encoding||_status>=MTLCommandBufferStatusCommitted) { [self fail:@"Invalid encoder state."];return nil; }
 _encoding=YES;X9Encoder *e=[X9Encoder new];e.command=self;e.bindings=[NSMutableDictionary new];return e;
}
- (void)enqueue { if(_status==MTLCommandBufferStatusNotEnqueued)_status=MTLCommandBufferStatusEnqueued; }
- (void)addScheduledHandler:(MTLCommandBufferHandler)handler {
 if(!handler)return;if(_status>=MTLCommandBufferStatusScheduled)handler(self);else [_scheduled addObject:[handler copy]];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {
 if(!handler)return;if(_status==MTLCommandBufferStatusCompleted||_status==MTLCommandBufferStatusError)handler(self);else [_completed addObject:[handler copy]];
}
- (void)commit {
 if(_status>=MTLCommandBufferStatusCommitted) { [self fail:@"Command buffer already committed."];return; }
 if(_encoding)[self fail:@"Encoder must end before commit."];
 _status=MTLCommandBufferStatusCommitted;
 if(!_error) {
  _status=MTLCommandBufferStatusScheduled;
  for(MTLCommandBufferHandler h in _scheduled)h(self);
  for(NSDictionary *op in _operations) {
   NSError *error=nil;if(![_queue.owner execute:op error:&error]) { _error=error;break; }
  }
 }
 _status=_error?MTLCommandBufferStatusError:MTLCommandBufferStatusCompleted;
 for(MTLCommandBufferHandler h in _completed)h(self);
 [_scheduled removeAllObjects];[_completed removeAllObjects];
}
- (void)waitUntilCompleted { /* commit is synchronous and waits for actual GPU completion. */ }
- (void)waitUntilScheduled { }
@end
@implementation X9Encoder
- (id<MTLDevice>)device { return _command.device; }
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state {
 if(_ended||_command.status>=MTLCommandBufferStatusCommitted||![state isKindOfClass:[X9Pipeline class]]||[(X9Pipeline *)state owner]!=_command.device)
  [_command fail:@"Invalid compute pipeline or encoder state."];
 else _pipeline=(X9Pipeline *)state;
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
 if(_ended||_command.status>=MTLCommandBufferStatusCommitted||index>=3||![buffer isKindOfClass:[X9Buffer class]]||buffer.device!=_command.device||offset%4||offset>buffer.length) {
  [_command fail:@"Invalid buffer binding or offset."];return;
 }
 _bindings[@(index)]=@{@"buffer":buffer,@"offset":@(offset)};
}
- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)local {
 if(_ended||_command.status>=MTLCommandBufferStatusCommitted||!_pipeline||!grid.width||grid.width>X9_METAL_MAX||
    !local.width||local.width>128||grid.width%local.width||grid.height!=1||grid.depth!=1||local.height!=1||local.depth!=1||_bindings.count!=3||_command.operations.count>=16) {
  [_command fail:@"Expected 1D add_u32 dispatch, count<=256, local<=128, complete groups and three buffers."];return;
 }
 [_command.operations addObject:@{@"count":@(grid.width),@"local":@(local.width),
  @"a":_bindings[@0][@"buffer"],@"ao":_bindings[@0][@"offset"],
  @"b":_bindings[@1][@"buffer"],@"bo":_bindings[@1][@"offset"],
  @"c":_bindings[@2][@"buffer"],@"co":_bindings[@2][@"offset"]}];
}
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)local {
 if(!local.width||local.width>128||!groups.width||groups.width>X9_METAL_MAX/local.width||groups.height!=1||groups.depth!=1) {
  [_command fail:@"Invalid threadgroup dimensions."];return;
 }
 [self dispatchThreads:MTLSizeMake(groups.width*local.width,1,1) threadsPerThreadgroup:local];
}
- (void)endEncoding { if(_ended)[_command fail:@"Encoder ended twice."];_ended=YES;_command.encoding=NO; }
@end
id<MTLDevice> X9MetalCreateDevice(NSError **error) {
 io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching(X9_METAL_SERVICE));
 if(!service) { if(error)*error=X9Error(10,@"SweetX9Compute service is unavailable; startup hardware baseline must pass first.");return nil; }
 io_connect_t connection=IO_OBJECT_NULL;kern_return_t kr=IOServiceOpen(service,mach_task_self(),0,&connection);IOObjectRelease(service);
 if(kr!=KERN_SUCCESS) { if(error)*error=X9Error(11,[NSString stringWithFormat:@"IOServiceOpen failed:0x%x; GPU access permission required.",kr]);return nil; }
 struct X9MetalCaps caps={0};size_t bytes=sizeof(caps);
 kr=IOConnectCallStructMethod(connection,0,NULL,0,&caps,&bytes);
 if(kr||bytes!=sizeof(caps)||caps.abi!=X9_METAL_ABI||caps.max_count!=X9_METAL_MAX||caps.gpu_id!=0xe8800010) {
  IOServiceClose(connection);if(error)*error=X9Error(12,@"Compute ABI or GPU mismatch.");return nil;
 }
 X9Device *device=[X9Device new];device.connection=connection;device.lock=[NSLock new];return device;
}
