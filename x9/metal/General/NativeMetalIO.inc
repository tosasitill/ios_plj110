// SPDX-License-Identifier: MIT
// Native file reads followed by real Metal uploads; all completion follows I/O and GPU work.
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <compression.h>

@interface NBIOFileHandle : NBObject <MTLIOFileHandle>
@property(nonatomic) int descriptor;
@property(nonatomic) uint64_t bytes,physicalBytes,chunkBytes;
@property(nonatomic) compression_algorithm algorithm;
@property(nonatomic,strong) NSData *chunkTable;
@end
@implementation NBIOFileHandle
- (instancetype)init {self=[super init];if(self)_descriptor=-1;return self;}
- (void)dealloc {if(_descriptor>=0)close(_descriptor);}
@end

@interface NBIOQueue : NBObject <MTLIOCommandQueue>
@property(nonatomic,strong) dispatch_queue_t work;
@property(nonatomic,strong) dispatch_semaphore_t capacity;
@property(nonatomic,strong) id<MTLCommandQueue> uploads;
@property(nonatomic,strong) id<MTLIOScratchBufferAllocator> allocator;
@end
@interface NBIOCommandBuffer : NBObject <MTLIOCommandBuffer>
@property(nonatomic,strong) NBIOQueue *queue;
@property(nonatomic,strong) NSMutableArray *operations,*handlers,*statusTargets,*debugGroups;
@property(nonatomic,strong) dispatch_semaphore_t ready;
@property(nonatomic,strong) dispatch_group_t done;
@property(atomic) BOOL committed,enqueued,cancelled;
@property(atomic) MTLIOStatus status;
@property(atomic,strong) NSError *error;
- (void)encode:(BOOL (^)(NSError **))operation;
- (void)execute;
@end
static thread_local void *gActiveIOCommand=nullptr;
static NSError *nbIOError(NSString *what,int code) {
 return [NSError errorWithDomain:MTLIOErrorDomain code:code?:MTLIOErrorInternal userInfo:@{NSLocalizedDescriptionKey:what}];
}
static BOOL nbIOReadRaw(NBIOFileHandle *file,uint64_t offset,void *pointer,NSUInteger bytes,NSError **error) {
 if(!file||file.descriptor<0||offset>file.physicalBytes||bytes>file.physicalBytes-offset||offset>INT64_MAX||bytes>(uint64_t)INT64_MAX-offset||(bytes&&!pointer)){
  if(error)*error=nbIOError(@"file read outside source range",MTLIOErrorInternal);return NO;
 }
 NSUInteger at=0;
 while(at<bytes){
  ssize_t got=pread(file.descriptor,(char *)pointer+at,MIN(bytes-at,(NSUInteger)SSIZE_MAX),(off_t)(offset+at));
  if(got<0&&errno==EINTR)continue;
  if(got<=0){if(error)*error=nbIOError(got==0?@"file ended before requested range":[NSString stringWithUTF8String:strerror(errno)],MTLIOErrorInternal);return NO;}
  at+=(NSUInteger)got;
 }
 return YES;
}
// Native iOS16 MTLIO compressor layout: 24-byte header, then 24-byte
// chunk records (compressed byte, absolute file offset, stored byte count).
// Codecs are the original libcompression implementations, including LZBitmap.
static uint64_t nbIOWord(const uint8_t *p){uint64_t value;memcpy(&value,p,8);return value;}
static BOOL nbIORead(NBIOFileHandle *file,uint64_t offset,void *pointer,NSUInteger bytes,NSError **error) {
 if(!file.chunkTable)return nbIOReadRaw(file,offset,pointer,bytes,error);
 if(offset>file.bytes||bytes>file.bytes-offset||(bytes&&!pointer)){if(error)*error=nbIOError(@"compressed read outside source range",MTLIOErrorInternal);return NO;}
 NSUInteger at=0;const uint8_t *table=(const uint8_t *)file.chunkTable.bytes;
 while(at<bytes){
  uint64_t position=offset+at,index=position/file.chunkBytes,within=position%file.chunkBytes;
  const uint8_t *record=table+index*24;uint64_t diskOffset=nbIOWord(record+8),stored=nbIOWord(record+16);
  NSUInteger n=(NSUInteger)MIN((uint64_t)(bytes-at),file.chunkBytes-within);
  if(record[0]){
   NSMutableData *packed=[NSMutableData dataWithLength:(NSUInteger)stored],*decoded=[NSMutableData dataWithLength:(NSUInteger)file.chunkBytes];
   if(!nbIOReadRaw(file,diskOffset,packed.mutableBytes,packed.length,error))return NO;
   size_t got=compression_decode_buffer((uint8_t *)decoded.mutableBytes,decoded.length,(const uint8_t *)packed.bytes,packed.length,nullptr,file.algorithm);
   if(got<within||n>got-within){if(error)*error=nbIOError(@"compressed chunk decode failed or requested bytes exceed decoded chunk",MTLIOErrorInternal);return NO;}
   memcpy((char *)pointer+at,(const char *)decoded.bytes+within,n);
  }else{
   if(within>stored||n>stored-within){if(error)*error=nbIOError(@"read exceeds final raw chunk",MTLIOErrorInternal);return NO;}
   if(!nbIOReadRaw(file,diskOffset+within,(char *)pointer+at,n,error))return NO;
  }
  at+=n;
 }
 return YES;
}
static id<MTLBuffer> nbIOScratch(NBIOQueue *queue,NSUInteger bytes,id<MTLIOScratchBuffer> *owner,NSError **error) {
 id<MTLBuffer> buffer=nil;
 if(queue.allocator){*owner=[queue.allocator newScratchBufferWithMinimumSize:bytes];buffer=(*owner).buffer;}
 else buffer=[gDeviceObject newBufferWithLength:bytes options:MTLResourceStorageModeShared];
 if(!buffer||buffer.length<bytes||!buffer.contents||![(id)buffer isKindOfClass:[NBBuffer class]]){
  if(error)*error=nbIOError(@"scratch allocation must provide writable shared native storage",MTLIOErrorInternal);return nil;
 }
 return buffer;
}
static BOOL nbIOUpload(NBIOQueue *queue,id<MTLBuffer> staging,id<MTLBuffer> destination,NSUInteger offset,NSUInteger size,NSError **error){
 id<MTLCommandBuffer> command=[queue.uploads commandBuffer];id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
 if(!blit){if(error)*error=nbIOError(@"upload command could not begin",MTLIOErrorInternal);return NO;}
 [blit copyFromBuffer:staging sourceOffset:0 toBuffer:destination destinationOffset:offset size:size];[blit endEncoding];[command commit];[command waitUntilCompleted];
 if(command.status!=MTLCommandBufferStatusCompleted){if(error)*error=command.error?:nbIOError(@"GPU upload failed",MTLIOErrorInternal);return NO;}return YES;
}
@implementation NBIOCommandBuffer
- (instancetype)init {self=[super init];if(self){_operations=[NSMutableArray array];_handlers=[NSMutableArray array];_statusTargets=[NSMutableArray array];_debugGroups=[NSMutableArray array];_ready=dispatch_semaphore_create(0);_done=dispatch_group_create();dispatch_group_enter(_done);_status=MTLIOStatusPending;}return self;}
- (void)encode:(BOOL (^)(NSError **))operation {@synchronized(self){if(_committed){_error=nbIOError(@"I/O command encoded after commit",MTLIOErrorInternal);return;}if(operation)[_operations addObject:[operation copy]];}}
- (void)addCompletedHandler:(MTLIOCommandBufferHandler)handler {if(!handler)return;@synchronized(self){if(_status==MTLIOStatusPending){[_handlers addObject:[handler copy]];return;}}dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{handler(self);});}
- (void)loadBytes:(void *)pointer size:(NSUInteger)size sourceHandle:(id<MTLIOFileHandle>)source sourceHandleOffset:(NSUInteger)offset {
 if(![(id)source isKindOfClass:[NBIOFileHandle class]]){_error=nbIOError(@"foreign I/O file handle",MTLIOErrorInternal);return;}
 [self encode:^BOOL(NSError **error){return nbIORead((NBIOFileHandle *)source,offset,pointer,size,error);}];
}
- (void)loadBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset size:(NSUInteger)size sourceHandle:(id<MTLIOFileHandle>)source sourceHandleOffset:(NSUInteger)fileOffset {
 if(![(id)buffer isKindOfClass:[NBBuffer class]]||![(id)source isKindOfClass:[NBIOFileHandle class]]||offset>buffer.length||size>buffer.length-offset||(uint64_t)fileOffset>((NBIOFileHandle *)source).bytes||size>((NBIOFileHandle *)source).bytes-(uint64_t)fileOffset){_error=nbIOError(@"invalid buffer load range or source",MTLIOErrorInternal);return;}
 NBIOQueue *queue=_queue;__weak NBIOCommandBuffer *weak=self;
 [self encode:^BOOL(NSError **error){
  const NSUInteger window=4u<<20;NSUInteger at=0;
  while(at<size){if(weak.cancelled)return NO;NSUInteger n=MIN(size-at,window);id<MTLIOScratchBuffer> owner=nil;id<MTLBuffer> staging=nbIOScratch(queue,n,&owner,error);if(!staging)return NO;
   if(!nbIORead((NBIOFileHandle *)source,(uint64_t)fileOffset+at,staging.contents,n,error)||!nbIOUpload(queue,staging,buffer,offset+at,n,error))return NO;
   at+=n;
  }return YES;
 }];
}
- (void)loadTexture:(id<MTLTexture>)texture slice:(NSUInteger)slice level:(NSUInteger)level size:(MTLSize)size sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)image destinationOrigin:(MTLOrigin)origin sourceHandle:(id<MTLIOFileHandle>)source sourceHandleOffset:(NSUInteger)offset {
 if(![(id)texture isKindOfClass:[NBTexture class]]||![(id)source isKindOfClass:[NBIOFileHandle class]]){_error=nbIOError(@"foreign texture or file handle",MTLIOErrorInternal);return;}
 uint64_t plane=0,bytes=0;
 if(__builtin_mul_overflow((uint64_t)row,nmb_texture_io_rows([(NBTexture *)texture handle],size.height),&plane)||__builtin_mul_overflow((uint64_t)image,(uint64_t)(size.depth?size.depth-1:0),&bytes)||__builtin_add_overflow(bytes,plane,&bytes)||bytes>128u*1024*1024){_error=nbIOError(@"texture load size overflow or exceeds native allocation",MTLIOErrorInternal);return;}
 NBIOQueue *queue=_queue;
 [self encode:^BOOL(NSError **error){
  id<MTLIOScratchBuffer> owner=nil;id<MTLBuffer> staging=nbIOScratch(queue,(NSUInteger)bytes,&owner,error);if(!staging)return NO;
  if(!nbIORead((NBIOFileHandle *)source,offset,staging.contents,(NSUInteger)bytes,error))return NO;
  id<MTLCommandBuffer> command=[queue.uploads commandBuffer];id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
  if(!blit){if(error)*error=nbIOError(@"texture upload could not begin",MTLIOErrorInternal);return NO;}
  [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:image sourceSize:size toTexture:texture destinationSlice:slice destinationLevel:level destinationOrigin:origin];
  [blit endEncoding];[command commit];[command waitUntilCompleted];
  if(command.status!=MTLCommandBufferStatusCompleted){if(error)*error=command.error?:nbIOError(@"texture upload failed",MTLIOErrorInternal);return NO;}return YES;
 }];
}
- (void)copyStatusToBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset {
 @synchronized(self){if(_committed||![(id)buffer isKindOfClass:[NBBuffer class]]||(offset&3)||offset>buffer.length||4>buffer.length-offset){_error=nbIOError(@"invalid I/O status destination",MTLIOErrorInternal);return;}[_statusTargets addObject:@[buffer,@(offset)]];}
}
- (void)waitForEvent:(id<MTLSharedEvent>)event value:(uint64_t)value {
 if(![(id)event isKindOfClass:[NBSharedEvent class]]){_error=nbIOError(@"foreign shared event",MTLIOErrorInternal);return;}
 __weak NBIOCommandBuffer *weak=self;
 [self encode:^BOOL(NSError **error){(void)error;while(!nmb_shared_event_wait([(NBSharedEvent *)event handle],value,100)){if(weak.cancelled)return NO;}return YES;}];
}
- (void)signalEvent:(id<MTLSharedEvent>)event value:(uint64_t)value {
 if(![(id)event isKindOfClass:[NBSharedEvent class]]){_error=nbIOError(@"foreign shared event",MTLIOErrorInternal);return;}
 [self encode:^BOOL(NSError **error){(void)error;event.signaledValue=value;return YES;}];
}
- (void)addBarrier { /* Every operation finishes its I/O and upload before the next begins. */ }
- (void)pushDebugGroup:(NSString *)name {if(name)@synchronized(self){[_debugGroups addObject:name];}}
- (void)popDebugGroup {@synchronized(self){if(_debugGroups.count)[_debugGroups removeLastObject];}}
- (void)enqueue {@synchronized(self){if(_enqueued)return;_enqueued=YES;dispatch_async(_queue.work,^{dispatch_semaphore_wait(self.ready,DISPATCH_TIME_FOREVER);[self execute];});}}
- (void)commit {@synchronized(self){if(_committed)return;_committed=YES;[self enqueue];dispatch_semaphore_signal(_ready);}}
- (void)tryCancel {_cancelled=YES;}
- (void)waitUntilCompleted {if(!_committed)return;if(gActiveIOCommand==(__bridge void *)self){NBLOG(@"native-metal: own I/O callback wait refused");return;}dispatch_group_wait(_done,DISPATCH_TIME_FOREVER);}
- (void)execute {
 dispatch_semaphore_wait(_queue.capacity,DISPATCH_TIME_FOREVER);
 NSArray *operations;@synchronized(self){operations=[_operations copy];[_operations removeAllObjects];}
 NSError *error=self.error;
 for(BOOL (^operation)(NSError **) in operations){if(error||_cancelled)break;if(!operation(&error)){if(!_cancelled&&!error)error=nbIOError(@"I/O operation failed",MTLIOErrorInternal);break;}}
 MTLIOStatus finalStatus=_cancelled?MTLIOStatusCancelled:(error?MTLIOStatusError:MTLIOStatusComplete);
 for(NSArray *target in _statusTargets){uint32_t status=(uint32_t)finalStatus;id<MTLBuffer> staging=[gDeviceObject newBufferWithBytes:&status length:sizeof(status) options:MTLResourceStorageModeShared];NSError *copyError=nil;
  if(!staging||!nbIOUpload(_queue,staging,target[0],[target[1] unsignedIntegerValue],sizeof(status),&copyError)){error=copyError?:nbIOError(@"I/O status upload failed",MTLIOErrorInternal);finalStatus=MTLIOStatusError;break;}}
 NSArray *handlers;@synchronized(self){_error=error;_status=finalStatus;handlers=[_handlers copy];[_handlers removeAllObjects];[_statusTargets removeAllObjects];}
 void *previous=gActiveIOCommand;gActiveIOCommand=(__bridge void *)self;
 @try {for(MTLIOCommandBufferHandler handler in handlers)handler(self);}
 @finally {gActiveIOCommand=previous;dispatch_group_leave(_done);dispatch_semaphore_signal(_queue.capacity);}
}
@end
@implementation NBIOQueue
- (id<MTLIOCommandBuffer>)commandBuffer {NBIOCommandBuffer *command=[NBIOCommandBuffer new];command.queue=self;return command;}
- (id<MTLIOCommandBuffer>)commandBufferWithUnretainedReferences {return [self commandBuffer];}
- (void)enqueueBarrier {dispatch_barrier_async(_work,^{});}
@end
@implementation NBDeviceMethods (X9IOAPI)
- (id<MTLIOFileHandle>)newIOHandleWithURL:(NSURL *)url error:(NSError **)error {
 if(!url.isFileURL){if(error)*error=nbIOError(@"I/O source requires a file URL",MTLIOErrorURLInvalid);return nil;}
 int fd=open(url.fileSystemRepresentation,O_RDONLY|O_CLOEXEC);struct stat info={};
 if(fd<0||fstat(fd,&info)||!S_ISREG(info.st_mode)||info.st_size<0){if(fd>=0)close(fd);if(error)*error=nbIOError(@"I/O source could not be opened as a regular file",MTLIOErrorURLInvalid);return nil;}
 NBIOFileHandle *file=[NBIOFileHandle new];file.descriptor=fd;file.bytes=(uint64_t)info.st_size;file.physicalBytes=file.bytes;if(error)*error=nil;return file;
}
- (id<MTLIOFileHandle>)newIOHandleWithURL:(NSURL *)url compressionMethod:(MTLIOCompressionMethod)method error:(NSError **)error {
 if(method<MTLIOCompressionMethodZlib||method>MTLIOCompressionMethodLZBitmap){if(error)*error=nbIOError(@"invalid I/O compression method",MTLIOErrorInternal);return nil;}
 NBIOFileHandle *file=(NBIOFileHandle *)[self newIOHandleWithURL:url error:error];if(!file)return nil;
 uint8_t header[24]={};if(!nbIOReadRaw(file,0,header,sizeof(header),error))return nil;
 uint32_t magic=0;memcpy(&magic,header,4);uint64_t chunk=nbIOWord(header+8),count=nbIOWord(header+16),tableBytes=0,logical=0;
 if(magic!=0xbadc0fee||!chunk||chunk>(128u<<20)||__builtin_mul_overflow(count,24ull,&tableBytes)||tableBytes>file.physicalBytes-sizeof(header)||tableBytes>SIZE_MAX||__builtin_mul_overflow(chunk,count,&logical)){
  if(error)*error=nbIOError(@"invalid MTLIO compressed container bounds",MTLIOErrorInternal);return nil;
 }
 NSMutableData *table=[NSMutableData dataWithLength:(NSUInteger)tableBytes];if(!nbIOReadRaw(file,24,table.mutableBytes,table.length,error))return nil;
 uint64_t previousEnd=24+tableBytes;const uint8_t *records=(const uint8_t *)table.bytes;
 for(uint64_t i=0;i<count;i++){const uint8_t *record=records+i*24;uint64_t offset=nbIOWord(record+8),size=nbIOWord(record+16);
  if(!size||size>chunk||offset<previousEnd||offset>file.physicalBytes||size>file.physicalBytes-offset){if(error)*error=nbIOError(@"invalid MTLIO chunk extent",MTLIOErrorInternal);return nil;}previousEnd=offset+size;
 }
 const uint32_t algorithms[5]={0x205,0x801,0x101,0x306,0x702};file.algorithm=(compression_algorithm)algorithms[method];file.chunkBytes=chunk;file.chunkTable=table;file.bytes=logical;
 if(error)*error=nil;return file;
}
- (id<MTLIOCommandQueue>)newIOCommandQueueWithDescriptor:(MTLIOCommandQueueDescriptor *)descriptor error:(NSError **)error {
 if(!descriptor||descriptor.priority<MTLIOPriorityHigh||descriptor.priority>MTLIOPriorityLow||descriptor.type<MTLIOCommandQueueTypeConcurrent||descriptor.type>MTLIOCommandQueueTypeSerial){if(error)*error=nbIOError(@"invalid I/O queue descriptor",MTLIOErrorInternal);return nil;}
 NBIOQueue *queue=[NBIOQueue new];queue.uploads=[gDeviceObject newCommandQueue];if(!queue.uploads){if(error)*error=nbIOError(@"GPU upload queue unavailable",MTLIOErrorInternal);return nil;}
 qos_class_t qos=descriptor.priority==MTLIOPriorityHigh?QOS_CLASS_USER_INITIATED:descriptor.priority==MTLIOPriorityLow?QOS_CLASS_UTILITY:QOS_CLASS_DEFAULT;
 dispatch_queue_attr_t attr=descriptor.type==MTLIOCommandQueueTypeConcurrent?DISPATCH_QUEUE_CONCURRENT:DISPATCH_QUEUE_SERIAL;
 queue.work=dispatch_queue_create("org.heliodarwin.metal.io",dispatch_queue_attr_make_with_qos_class(attr,qos,0));
 NSUInteger limit=MIN(descriptor.maxCommandBufferCount?:64,(NSUInteger)64);if(descriptor.maxCommandsInFlight)limit=MIN(limit,descriptor.maxCommandsInFlight);
 queue.capacity=dispatch_semaphore_create((long)limit);queue.allocator=descriptor.scratchBufferAllocator;if(error)*error=nil;return queue;
}
@end
