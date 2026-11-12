// SPDX-License-Identifier: MIT
// Timestamp counters sample actual GPU execution, with no CPU timing substitution.
@interface NBCounter : NSObject <MTLCounter>
@end
@implementation NBCounter
- (NSString *)name {return MTLCommonCounterTimestamp;}
@end
@interface NBCounterSet : NSObject <MTLCounterSet>
@end
@implementation NBCounterSet
- (NSString *)name {return MTLCommonCounterSetTimestamp;}
- (NSArray<id<MTLCounter>> *)counters {return @[[NBCounter new]];}
@end
@interface NBCounterSampleBuffer : NBObject <MTLCounterSampleBuffer>
@property(nonatomic) NmbCounter *handle;
@property(nonatomic) NSUInteger sampleCount;
@property(nonatomic) MTLStorageMode storageMode;
@end
@implementation NBCounterSampleBuffer
- (void)dealloc {nmb_counter_release(_handle);}
- (NSData *)resolveCounterRange:(NSRange)range {
 if(_storageMode!=MTLStorageModeShared||range.location>_sampleCount||range.length>_sampleCount-range.location)return nil;
 NSMutableData *data=[NSMutableData dataWithLength:range.length*sizeof(uint64_t)];
 if(!nmb_counter_resolve(_handle,range.location,range.length,(uint64_t *)data.mutableBytes)){NBLOG(@"native-metal: resolve counters: %@",lastError());return nil;}return data;
}
@end
@implementation NBDeviceMethods (X9CounterAPI)
- (NSArray<id<MTLCounterSet>> *)counterSets {return nmb_counter_supported(gDevice)?@[[NBCounterSet new]]:@[];}
- (id<MTLCounterSampleBuffer>)newCounterSampleBufferWithDescriptor:(MTLCounterSampleBufferDescriptor *)descriptor error:(NSError **)error {
 if(!descriptor||![(id)descriptor.counterSet isKindOfClass:[NBCounterSet class]]||(descriptor.storageMode!=MTLStorageModeShared&&descriptor.storageMode!=MTLStorageModePrivate)||!descriptor.sampleCount||descriptor.sampleCount>4096){if(error)*error=nbError(@"invalid timestamp sample buffer descriptor");return nil;}
 NmbCounter *handle=nmb_counter_new(gDevice,descriptor.sampleCount);if(!handle){if(error)*error=nbError(lastError());return nil;}
 NBCounterSampleBuffer *buffer=[NBCounterSampleBuffer new];buffer.handle=handle;buffer.sampleCount=descriptor.sampleCount;buffer.storageMode=descriptor.storageMode;buffer.label=descriptor.label;if(error)*error=nil;return buffer;
}
- (BOOL)supportsCounterSampling:(MTLCounterSamplingPoint)point {
 return nmb_counter_supported(gDevice)&&(point==MTLCounterSamplingPointAtBlitBoundary||point==MTLCounterSamplingPointAtDispatchBoundary||point==MTLCounterSamplingPointAtDrawBoundary);
}
@end
@implementation NBEncoder (X9CounterSampling)
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)buffer atSampleIndex:(NSUInteger)index withBarrier:(BOOL)barrier {
 if(![(id)buffer isKindOfClass:[NBCounterSampleBuffer class]]||index>=buffer.sampleCount){[self.commandBuffer refuse:@"invalid counter sample target"];return;}
 [self.commandBuffer own:buffer];[self check:nmb_counter_sample(self.handle,[(NBCounterSampleBuffer *)buffer handle],index,barrier) what:"sample GPU timestamp"];
}
@end
@implementation NBBlitEncoder (X9CounterResolve)
- (void)resolveCounters:(id<MTLCounterSampleBuffer>)counters inRange:(NSRange)range destinationBuffer:(id<MTLBuffer>)destination destinationOffset:(NSUInteger)offset {
 if(![(id)counters isKindOfClass:[NBCounterSampleBuffer class]]||![(id)destination isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"foreign counter resolve resource"];return;}
 [self.commandBuffer own:counters];[self.commandBuffer own:destination];
 [self check:nmb_counter_gpu_resolve(self.handle,[(NBCounterSampleBuffer *)counters handle],range.location,range.length,[(NBBuffer *)destination handle],offset) what:"resolve GPU timestamps"];
}
@end
