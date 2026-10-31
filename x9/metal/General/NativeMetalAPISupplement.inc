// SPDX-License-Identifier: MIT
// Common Metal APIs routed to the same runtime objects as the original adapter.
@implementation NBBuffer (X9ResourceAPI)
- (void)makeAliasable {if(!nmb_buffer_make_aliasable(self.handle))NBLOG(@"native-metal: buffer makeAliasable: %@",lastError());}
- (BOOL)isAliasable {return nmb_buffer_is_aliasable(self.handle);}
- (NSUInteger)heapOffset {return nmb_buffer_heap_offset(self.handle);}
- (void)addDebugMarker:(NSString *)marker range:(NSRange)range {
 if(!marker||range.location>self.length||range.length>self.length-range.location){NBLOG(@"native-metal: invalid buffer debug marker range");return;}
 @synchronized(self){NSMutableArray *markers=objc_getAssociatedObject(self,@selector(addDebugMarker:range:));if(!markers){markers=[NSMutableArray array];objc_setAssociatedObject(self,@selector(addDebugMarker:range:),markers,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}[markers addObject:@[marker,[NSValue valueWithRange:range]]];}
}
- (void)removeAllDebugMarkers {@synchronized(self){objc_setAssociatedObject(self,@selector(addDebugMarker:range:),nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}}
@end
@implementation NBTexture (X9ResourceAPI)
- (void)makeAliasable {if(!nmb_texture_make_aliasable(self.handle))NBLOG(@"native-metal: texture makeAliasable: %@",lastError());}
- (BOOL)isAliasable {return nmb_texture_is_aliasable(self.handle);}
- (NSUInteger)heapOffset {return nmb_texture_query(self.handle,0);}
- (NSUInteger)parentRelativeLevel {return nmb_texture_query(self.handle,1);}
- (NSUInteger)parentRelativeSlice {return nmb_texture_query(self.handle,2);}
- (MTLHazardTrackingMode)hazardTrackingMode {return (MTLHazardTrackingMode)nmb_texture_query(self.handle,3);}
- (BOOL)allowGPUOptimizedContents {return nmb_texture_query(self.handle,4)!=0;}
- (BOOL)isSparse {return nmb_texture_query(self.handle,5)!=0;}
- (MTLResourceID)gpuResourceID {MTLResourceID value={nmb_texture_query(self.handle,6)};return value;}
- (MTLTextureSwizzleChannels)swizzle {uint64_t s[4]={};nmb_texture_swizzle(self.handle,s);return MTLTextureSwizzleChannelsMake((MTLTextureSwizzle)s[0],(MTLTextureSwizzle)s[1],(MTLTextureSwizzle)s[2],(MTLTextureSwizzle)s[3]);}
- (id<MTLResource>)rootResource {return self.parent?[(id)self.parent rootResource]:(!self.surface&&self.buffer?(id<MTLResource>)self.buffer:(id<MTLResource>)self);}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)kind levels:(NSRange)levels slices:(NSRange)slices swizzle:(MTLTextureSwizzleChannels)swizzle {
 const uint64_t l[2]={levels.location,levels.length},s[2]={slices.location,slices.length},w[4]={swizzle.red,swizzle.green,swizzle.blue,swizzle.alpha};
 return (id<MTLTexture>)[self view:nmb_texture_new_view_swizzle(self.handle,format,kind,l,s,w)];
}
@end
@implementation NBLibrary (X9AsyncAPI)
- (void)newFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {
 if(!handler)return;NSError *error=nil;id<MTLFunction> f=[self newFunctionWithDescriptor:descriptor error:&error];handler(f,error);
}
- (id<MTLFunction>)newIntersectionFunctionWithDescriptor:(MTLIntersectionFunctionDescriptor *)descriptor error:(NSError **)error {return [self newFunctionWithDescriptor:descriptor error:error];}
- (void)newIntersectionFunctionWithDescriptor:(MTLIntersectionFunctionDescriptor *)descriptor completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {
 if(!handler)return;NSError *error=nil;id<MTLFunction> f=[self newIntersectionFunctionWithDescriptor:descriptor error:&error];handler(f,error);
}
@end
@implementation NBCommandBuffer (X9SubmissionAPI)
- (void)presentDrawable:(id<MTLDrawable>)drawable {
 if(!drawable){[self refuse:@"nil drawable"];return;}
 [self addCompletedHandler:^(id<MTLCommandBuffer> b){if(b.status==MTLCommandBufferStatusCompleted)[drawable present];}];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)time {
 if(!drawable){[self refuse:@"nil drawable"];return;}
 [self addCompletedHandler:^(id<MTLCommandBuffer> b){if(b.status==MTLCommandBufferStatusCompleted)[drawable presentAtTime:time];}];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)duration {
 if(!drawable){[self refuse:@"nil drawable"];return;}
 [self addCompletedHandler:^(id<MTLCommandBuffer> b){if(b.status==MTLCommandBufferStatusCompleted)[drawable presentAfterMinimumDuration:duration];}];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)descriptor {
 for(NSUInteger i=0;i<4;i++)if(descriptor.sampleBufferAttachments[i].sampleBuffer){[self refuse:@"compute counter sample attachments unavailable"];return nil;}
 return [self computeCommandEncoderWithDispatchType:descriptor.dispatchType];
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)descriptor {
 for(NSUInteger i=0;i<4;i++)if(descriptor.sampleBufferAttachments[i].sampleBuffer){[self refuse:@"blit counter sample attachments unavailable"];return nil;}
 return [self blitCommandEncoder];
}
@end
@implementation NBQueue (X9QueueAPI)
- (NSUInteger)getGPUPriority {return 1;}
- (NSUInteger)getBackgroundGPUPriority {return 3;}
- (void)insertDebugCaptureBoundary {objc_setAssociatedObject(self,@selector(insertDebugCaptureBoundary),@YES,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
@end
@implementation NBDeviceMethods (X9AsyncPipelineAPI)
- (uint64_t)dedicatedMemorySize {return 0;}
- (uint64_t)sharedMemorySize {return [NSProcessInfo processInfo].physicalMemory;}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler {
 if(!handler)return;NSError *error=nil;id<MTLRenderPipelineState> state=[(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:descriptor error:&error];handler(state,error);
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler {
 if(!handler)return;NSError *error=nil;MTLRenderPipelineReflection *reflection=nil;
 id<MTLRenderPipelineState> state=[(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:descriptor options:options reflection:&reflection error:&error];handler(state,reflection,error);
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function completionHandler:(MTLNewComputePipelineStateCompletionHandler)handler {
 if(!handler)return;NSError *error=nil;id<MTLComputePipelineState> state=[(id<MTLDevice>)self newComputePipelineStateWithFunction:function error:&error];handler(state,error);
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler {
 if(!handler)return;NSError *error=nil;MTLComputePipelineReflection *reflection=nil;
 id<MTLComputePipelineState> state=[(id<MTLDevice>)self newComputePipelineStateWithFunction:function options:options reflection:&reflection error:&error];handler(state,reflection,error);
}
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor options:(MTLPipelineOption)options completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler {
 if(!handler)return;NSError *error=nil;MTLComputePipelineReflection *reflection=nil;
 id<MTLComputePipelineState> state=[(id<MTLDevice>)self newComputePipelineStateWithDescriptor:descriptor options:options reflection:&reflection error:&error];handler(state,reflection,error);
}
// Apple's precompiled GPU binaries cannot be consumed by PanVK. The framework
// receives an ordinary error and can build from the portable shader library.
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error {
 if(error)*error=nbError([NSString stringWithFormat:@"native GPU pipeline library unavailable: %@",path.lastPathComponent]);return nil;
}
@end
@implementation NBEncoder (X9ResidencyAPI)
- (void)residentResource:(id)resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages {
 NmbBuffer *b=nullptr;NmbTexture *t=nullptr;
 if([resource isKindOfClass:[NBBuffer class]])b=[(NBBuffer *)resource handle];
 else if([resource isKindOfClass:[NBTexture class]])t=[(NBTexture *)resource handle];
 else{[self.commandBuffer refuse:@"foreign resource residency"];return;}
 [self.commandBuffer.resourceOwners addObject:resource];
 if(t&&[(NBTexture *)resource surface])[self.commandBuffer.surfaces addObject:resource];
 [self check:nmb_encoder_use_resource(self.handle,b,t,usage,stages) what:"useResource"];
}
- (void)residentHeap:(id)heap stages:(MTLRenderStages)stages {
 if(![heap isKindOfClass:[NBHeap class]]){[self.commandBuffer refuse:@"foreign heap residency"];return;}
 [self.commandBuffer.resourceOwners addObject:heap];[self check:nmb_encoder_use_heap(self.handle,[(NBHeap *)heap handle],stages) what:"useHeap"];
}
@end
@implementation NBComputeEncoder (X9BindingAPI)
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])samplers withRange:(NSRange)r {
 if(r.length&&!samplers){[self.commandBuffer refuse:@"null sampler array"];return;}for(NSUInteger k=0;k<r.length;k++)[self sampler:samplers[k] index:r.location+k stage:0];
}
- (void)dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)buffer indirectBufferOffset:(NSUInteger)offset threadsPerThreadgroup:(MTLSize)threads {
 if(![(id)buffer isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"foreign indirect dispatch buffer"];return;}
 [self.commandBuffer.resourceOwners addObject:buffer];const uint64_t t[3]={threads.width,threads.height,threads.depth};
 [self check:nmb_compute_dispatch_indirect(self.handle,[(NBBuffer *)buffer handle],offset,t) what:"dispatchThreadgroupsIndirect"];
}
- (void)setStageInRegion:(MTLRegion)region {const uint64_t r[6]={region.origin.x,region.origin.y,region.origin.z,region.size.width,region.size.height,region.size.depth};[self check:nmb_compute_stage_in(self.handle,r,nullptr,0) what:"setStageInRegion"];}
- (void)setStageInRegionWithIndirectBuffer:(id<MTLBuffer>)buffer indirectBufferOffset:(NSUInteger)offset {
 if(![(id)buffer isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"foreign stage-in buffer"];return;}
 [self.commandBuffer.resourceOwners addObject:buffer];[self check:nmb_compute_stage_in(self.handle,nullptr,[(NBBuffer *)buffer handle],offset) what:"setStageInRegionIndirect"];
}
- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height {[self check:nmb_compute_imageblock(self.handle,width,height) what:"setImageblockSize"];}
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count usage:(MTLResourceUsage)usage {
 if(count&&!resources){[self.commandBuffer refuse:@"null resource array"];return;}for(NSUInteger k=0;k<count;k++)[self residentResource:resources[k] usage:usage stages:0];
}
- (void)useHeap:(id<MTLHeap>)heap {[self residentHeap:heap stages:0];}
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])heaps count:(NSUInteger)count {
 if(count&&!heaps){[self.commandBuffer refuse:@"null heap array"];return;}for(NSUInteger k=0;k<count;k++)[self residentHeap:heaps[k] stages:0];
}
- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count {
 [self useResources:resources count:count usage:MTLResourceUsageRead|MTLResourceUsageWrite];[self check:nmb_compute_barrier(self.handle,MTLBarrierScopeBuffers|MTLBarrierScopeTextures) what:"memoryBarrierWithResources"];
}
@end
@implementation NBRenderEncoder (X9BindingAPI)
- (void)setVertexTextures:(const id<MTLTexture> __unsafe_unretained [])textures withRange:(NSRange)r {if(r.length&&!textures){[self.commandBuffer refuse:@"null texture array"];return;}for(NSUInteger k=0;k<r.length;k++)[self texture:textures[k] index:r.location+k stage:1];}
- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])samplers withRange:(NSRange)r {if(r.length&&!samplers){[self.commandBuffer refuse:@"null sampler array"];return;}for(NSUInteger k=0;k<r.length;k++)[self sampler:samplers[k] index:r.location+k stage:1];}
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])samplers withRange:(NSRange)r {if(r.length&&!samplers){[self.commandBuffer refuse:@"null sampler array"];return;}for(NSUInteger k=0;k<r.length;k++)[self sampler:samplers[k] index:r.location+k stage:2];}
- (void)setViewports:(const MTLViewport *)viewports count:(NSUInteger)count {
 if(count>16||(count&&!viewports)){[self.commandBuffer refuse:@"invalid viewport array"];return;}
 std::vector<double> v;v.reserve(count*6);for(NSUInteger i=0;i<count;i++){const auto &p=viewports[i];for(double x:{p.originX,p.originY,p.width,p.height,p.znear,p.zfar})v.push_back(x);}
 [self check:nmb_render_viewports(self.handle,v.data(),count) what:"setViewports"];
}
- (void)setScissorRects:(const MTLScissorRect *)rects count:(NSUInteger)count {
 if(count>16||(count&&!rects)){[self.commandBuffer refuse:@"invalid scissor array"];return;}
 std::vector<uint64_t> v;v.reserve(count*4);for(NSUInteger i=0;i<count;i++){const auto &p=rects[i];for(uint64_t x:{p.x,p.y,p.width,p.height})v.push_back(x);}
 [self check:nmb_render_scissors(self.handle,v.data(),count) what:"setScissorRects"];
}
- (void)setDepthClipMode:(MTLDepthClipMode)mode {[self check:nmb_render_depth_clip(self.handle,mode) what:"setDepthClipMode"];}
- (void)setColorStoreAction:(MTLStoreAction)action atIndex:(NSUInteger)index {[self check:nmb_render_store_action(self.handle,0,(uint32_t)index,action,false) what:"setColorStoreAction"];}
- (void)setDepthStoreAction:(MTLStoreAction)action {[self check:nmb_render_store_action(self.handle,1,0,action,false) what:"setDepthStoreAction"];}
- (void)setStencilStoreAction:(MTLStoreAction)action {[self check:nmb_render_store_action(self.handle,2,0,action,false) what:"setStencilStoreAction"];}
- (void)setColorStoreActionOptions:(MTLStoreActionOptions)options atIndex:(NSUInteger)index {[self check:nmb_render_store_action(self.handle,0,(uint32_t)index,options,true) what:"setColorStoreActionOptions"];}
- (void)setDepthStoreActionOptions:(MTLStoreActionOptions)options {[self check:nmb_render_store_action(self.handle,1,0,options,true) what:"setDepthStoreActionOptions"];}
- (void)setStencilStoreActionOptions:(MTLStoreActionOptions)options {[self check:nmb_render_store_action(self.handle,2,0,options,true) what:"setStencilStoreActionOptions"];}
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage {[self residentResource:resource usage:usage stages:MTLRenderStageVertex|MTLRenderStageFragment];}
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count usage:(MTLResourceUsage)usage {[self useResources:resources count:count usage:usage stages:MTLRenderStageVertex|MTLRenderStageFragment];}
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages {if(count&&!resources){[self.commandBuffer refuse:@"null resource array"];return;}for(NSUInteger k=0;k<count;k++)[self residentResource:resources[k] usage:usage stages:stages];}
- (void)useHeap:(id<MTLHeap>)heap {[self residentHeap:heap stages:MTLRenderStageVertex|MTLRenderStageFragment];}
- (void)useHeap:(id<MTLHeap>)heap stages:(MTLRenderStages)stages {[self residentHeap:heap stages:stages];}
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])heaps count:(NSUInteger)count {[self useHeaps:heaps count:count stages:MTLRenderStageVertex|MTLRenderStageFragment];}
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])heaps count:(NSUInteger)count stages:(MTLRenderStages)stages {if(count&&!heaps){[self.commandBuffer refuse:@"null heap array"];return;}for(NSUInteger k=0;k<count;k++)[self residentHeap:heaps[k] stages:stages];}
@end
@implementation NBBlitEncoder (X9TextureHints)
- (void)synchronizeTexture:(id<MTLTexture>)texture slice:(NSUInteger)slice level:(NSUInteger)level {[self textureHint:texture operation:0 slice:slice level:level];}
- (void)textureHint:(id<MTLTexture>)texture operation:(uint32_t)operation slice:(NSUInteger)slice level:(NSUInteger)level {
 if(![(id)texture isKindOfClass:[NBTexture class]]){[self.commandBuffer refuse:@"foreign texture hint"];return;}
 [self.commandBuffer.resourceOwners addObject:texture];[self check:nmb_blit_texture_hint(self.handle,[(NBTexture *)texture handle],operation,slice,level) what:"texture synchronization/optimization"];
}
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)texture {[self textureHint:texture operation:1 slice:0 level:0];}
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)texture slice:(NSUInteger)slice level:(NSUInteger)level {[self textureHint:texture operation:2 slice:slice level:level];}
- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)texture {[self textureHint:texture operation:3 slice:0 level:0];}
- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)texture slice:(NSUInteger)slice level:(NSUInteger)level {[self textureHint:texture operation:4 slice:slice level:level];}
@end

@implementation NBComputePipeline (X9PipelineAPI)
- (NSUInteger)staticThreadgroupMemoryLength {return nmb_pipeline_query(self.handle,nullptr,0,0,0,0);}
- (BOOL)supportIndirectCommandBuffers {return nmb_pipeline_query(self.handle,nullptr,1,0,0,0)!=0;}
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)s {return nmb_pipeline_query(self.handle,nullptr,2,s.width,s.height,s.depth);}
- (MTLResourceID)gpuResourceID {MTLResourceID value={nmb_pipeline_query(self.handle,nullptr,3,0,0,0)};return value;}
@end
@implementation NBRenderPipeline (X9PipelineAPI)
- (BOOL)supportIndirectCommandBuffers {return nmb_pipeline_query(nullptr,self.handle,1,0,0,0)!=0;}
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)s {return nmb_pipeline_query(nullptr,self.handle,2,s.width,s.height,s.depth);}
- (MTLResourceID)gpuResourceID {MTLResourceID value={nmb_pipeline_query(nullptr,self.handle,3,0,0,0)};return value;}
- (BOOL)threadgroupSizeMatchesTileSize {return nmb_pipeline_query(nullptr,self.handle,4,0,0,0)!=0;}
- (NSUInteger)imageblockSampleLength {return nmb_pipeline_query(nullptr,self.handle,5,0,0,0);}
@end
@implementation NBEncoder (X9SamplerLOD)
- (void)sampler:(id<MTLSamplerState>)sampler min:(float)min max:(float)max index:(NSUInteger)index stage:(uint32_t)stage {
 if(sampler&&![(id)sampler isKindOfClass:[NBSampler class]]){[self.commandBuffer refuse:@"foreign sampler"];return;}
 if(sampler)[self.commandBuffer.resourceOwners addObject:sampler];
 [self check:nmb_encoder_sampler_lod(self.handle,stage,sampler?[(NBSampler *)sampler handle]:nullptr,min,max,(uint32_t)index) what:"sampler LOD clamps"];
}
@end
@implementation NBComputeEncoder (X9SamplerLOD)
- (void)setSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)min lodMaxClamp:(float)max atIndex:(NSUInteger)i {[self sampler:s min:min max:max index:i stage:0];}
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s lodMinClamps:(const float *)min lodMaxClamps:(const float *)max withRange:(NSRange)r {
 if(r.length&&(!s||!min||!max)){[self.commandBuffer refuse:@"null sampler clamps"];return;}for(NSUInteger k=0;k<r.length;k++)[self sampler:s[k] min:min[k] max:max[k] index:r.location+k stage:0];
}
@end
@implementation NBRenderEncoder (X9SamplerLOD)
- (void)setVertexSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)min lodMaxClamp:(float)max atIndex:(NSUInteger)i {[self sampler:s min:min max:max index:i stage:1];}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)min lodMaxClamp:(float)max atIndex:(NSUInteger)i {[self sampler:s min:min max:max index:i stage:2];}
- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s lodMinClamps:(const float *)min lodMaxClamps:(const float *)max withRange:(NSRange)r {
 if(r.length&&(!s||!min||!max)){[self.commandBuffer refuse:@"null sampler clamps"];return;}for(NSUInteger k=0;k<r.length;k++)[self sampler:s[k] min:min[k] max:max[k] index:r.location+k stage:1];
}
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s lodMinClamps:(const float *)min lodMaxClamps:(const float *)max withRange:(NSRange)r {
 if(r.length&&(!s||!min||!max)){[self.commandBuffer refuse:@"null sampler clamps"];return;}for(NSUInteger k=0;k<r.length;k++)[self sampler:s[k] min:min[k] max:max[k] index:r.location+k stage:2];
}
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope afterStages:(MTLRenderStages)after beforeStages:(MTLRenderStages)before {[self check:nmb_render_barrier(self.handle,scope,after,before) what:"memoryBarrierWithScope"];}
- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count afterStages:(MTLRenderStages)after beforeStages:(MTLRenderStages)before {
 if(!count)return;[self useResources:resources count:count usage:MTLResourceUsageRead|MTLResourceUsageWrite stages:after|before];
 [self memoryBarrierWithScope:MTLBarrierScopeBuffers|MTLBarrierScopeTextures afterStages:after beforeStages:before];
}
- (void)drawPrimitives:(MTLPrimitiveType)primitive indirectBuffer:(id<MTLBuffer>)indirect indirectBufferOffset:(NSUInteger)offset {
 if(![(id)indirect isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"foreign indirect buffer"];return;}
 [self.commandBuffer.resourceOwners addObject:indirect];gDraws++;self.commandBuffer->encodedWork.draws++;
 [self check:nmb_render_indirect(self.handle,primitive,[(NBBuffer *)indirect handle],offset,nullptr,0,0) what:"drawPrimitivesIndirect"];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitive indexType:(MTLIndexType)type indexBuffer:(id<MTLBuffer>)indices indexBufferOffset:(NSUInteger)indexOffset indirectBuffer:(id<MTLBuffer>)indirect indirectBufferOffset:(NSUInteger)offset {
 if(![(id)indirect isKindOfClass:[NBBuffer class]]||![(id)indices isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"foreign indirect/index buffer"];return;}
 [self.commandBuffer.resourceOwners addObject:indirect];[self.commandBuffer.resourceOwners addObject:indices];gDraws++;self.commandBuffer->encodedWork.draws++;
 [self check:nmb_render_indirect(self.handle,primitive,[(NBBuffer *)indirect handle],offset,[(NBBuffer *)indices handle],type,indexOffset) what:"drawIndexedPrimitivesIndirect"];
}
@end
@interface NBArgumentEncoder : NBObject
@property(nonatomic) NmbArgumentEncoder *handle;
@property(nonatomic,strong) NSMutableSet *owners;
@end
@implementation NBArgumentEncoder
- (instancetype)init {self=[super init];if(self)_owners=[NSMutableSet set];return self;}
- (void)dealloc {nmb_argument_encoder_release(_handle);}
- (NSUInteger)encodedLength {return nmb_argument_encoder_query(_handle,false);}
- (NSUInteger)alignment {return nmb_argument_encoder_query(_handle,true);}
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset {[self setArgumentBuffer:buffer startOffset:offset arrayElement:0];}
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer startOffset:(NSUInteger)offset arrayElement:(NSUInteger)element {
 if(buffer&&![(id)buffer isKindOfClass:[NBBuffer class]]){NBLOG(@"native-metal: foreign argument buffer");return;}
 if(!nmb_argument_encoder_target(_handle,buffer?[(NBBuffer *)buffer handle]:nullptr,offset,element)){NBLOG(@"native-metal: argument target: %@",lastError());return;}
 if(buffer)[_owners addObject:buffer];
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
 if(buffer&&![(id)buffer isKindOfClass:[NBBuffer class]]){NBLOG(@"native-metal: foreign argument value");return;}
 if(!nmb_argument_encoder_buffer(_handle,buffer?[(NBBuffer *)buffer handle]:nullptr,offset,(uint32_t)index))NBLOG(@"native-metal: argument buffer: %@",lastError());
 if(buffer)[_owners addObject:buffer];
}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained [])buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)r {
 if(r.length&&(!buffers||!offsets))return;for(NSUInteger k=0;k<r.length;k++)[self setBuffer:buffers[k] offset:offsets[k] atIndex:r.location+k];
}
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
 if(texture&&![(id)texture isKindOfClass:[NBTexture class]]){NBLOG(@"native-metal: foreign argument texture");return;}
 if(!nmb_argument_encoder_texture(_handle,texture?[(NBTexture *)texture handle]:nullptr,(uint32_t)index))NBLOG(@"native-metal: argument texture: %@",lastError());
 if(texture)[_owners addObject:texture];
}
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained [])textures withRange:(NSRange)r {if(r.length&&!textures)return;for(NSUInteger k=0;k<r.length;k++)[self setTexture:textures[k] atIndex:r.location+k];}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
 if(sampler&&![(id)sampler isKindOfClass:[NBSampler class]]){NBLOG(@"native-metal: foreign argument sampler");return;}
 if(!nmb_argument_encoder_sampler(_handle,sampler?[(NBSampler *)sampler handle]:nullptr,(uint32_t)index))NBLOG(@"native-metal: argument sampler: %@",lastError());
 if(sampler)[_owners addObject:sampler];
}
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])samplers withRange:(NSRange)r {if(r.length&&!samplers)return;for(NSUInteger k=0;k<r.length;k++)[self setSamplerState:samplers[k] atIndex:r.location+k];}
- (void *)constantDataAtIndex:(NSUInteger)index {void *p=nmb_argument_encoder_constant(_handle,(uint32_t)index);if(!p)NBLOG(@"native-metal: argument constant: %@",lastError());return p;}
- (id<MTLArgumentEncoder>)newArgumentEncoderForBufferAtIndex:(NSUInteger)index {
 NmbArgumentEncoder *handle=nmb_argument_encoder_nested(_handle,(uint32_t)index);if(!handle){NBLOG(@"native-metal: nested argument encoder: %@",lastError());return nil;}
 NBArgumentEncoder *encoder=[NBArgumentEncoder new];encoder.handle=handle;return (id<MTLArgumentEncoder>)encoder;
}
@end
@implementation NBFunction (X9ArgumentAPI)
- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index {
 NmbArgumentEncoder *handle=nmb_argument_encoder_new(self.handle,(uint32_t)index);if(!handle){NBLOG(@"native-metal: function argument encoder: %@",lastError());return nil;}
 NBArgumentEncoder *encoder=[NBArgumentEncoder new];encoder.handle=handle;return (id<MTLArgumentEncoder>)encoder;
}
@end

@interface NBFunctionHandle : NBObject
@property(nonatomic) NmbFunctionHandle *handle;
@end
@implementation NBFunctionHandle
- (void)dealloc {nmb_function_handle_release(_handle);}
- (NSString *)name {char text[512]={};nmb_function_handle_name(_handle,text,sizeof text);return [NSString stringWithUTF8String:text];}
@end
@interface NBVisibleFunctionTable : NBObject
@property(nonatomic) NmbVisibleTable *handle;
@end
@implementation NBVisibleFunctionTable
- (void)dealloc {nmb_visible_table_release(_handle);}
- (NSUInteger)allocatedSize {return nmb_visible_table_query(_handle,0);}
- (MTLResourceID)gpuResourceID {MTLResourceID value={nmb_visible_table_query(_handle,1)};return value;}
- (MTLResourceOptions)resourceOptions {return (MTLResourceOptions)nmb_visible_table_query(_handle,3);}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((self.resourceOptions&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((self.resourceOptions&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (MTLHazardTrackingMode)hazardTrackingMode {return (MTLHazardTrackingMode)((self.resourceOptions&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift);}
- (NSUInteger)heapOffset {return nmb_visible_table_query(_handle,4);}
- (id<MTLHeap>)heap {return nil;}
- (BOOL)isAliasable {return nmb_visible_table_query(_handle,5)!=0;}
- (void)makeAliasable {NBLOG(@"native-metal: visible function table cannot become heap-aliasable");}
- (void)setFunction:(id<MTLFunctionHandle>)function atIndex:(NSUInteger)index {
 if(function&&![(id)function isKindOfClass:[NBFunctionHandle class]]){NBLOG(@"native-metal: foreign function handle");return;}
 if(!nmb_visible_table_set(_handle,function?[(NBFunctionHandle *)function handle]:nullptr,(uint32_t)index))NBLOG(@"native-metal: function table entry: %@",lastError());
}
- (void)setFunctions:(const id<MTLFunctionHandle> __unsafe_unretained [])functions withRange:(NSRange)r {
 if(r.length&&!functions)return;for(NSUInteger k=0;k<r.length;k++)[self setFunction:functions[k] atIndex:r.location+k];
}
@end
static id<MTLFunctionHandle> x9FunctionHandle(NmbComputePipeline *compute,NmbRenderPipeline *render,id<MTLFunction>function,MTLRenderStages stage) {
 if(![(id)function isKindOfClass:[NBFunction class]])return nil;
 NmbFunctionHandle *handle=nmb_function_handle(compute,render,[(NBFunction *)function handle],stage);if(!handle)return nil;
 NBFunctionHandle *result=[NBFunctionHandle new];result.handle=handle;return (id<MTLFunctionHandle>)result;
}
static id<MTLVisibleFunctionTable> x9VisibleTable(NmbComputePipeline *compute,NmbRenderPipeline *render,MTLVisibleFunctionTableDescriptor *descriptor,MTLRenderStages stage) {
 if(!descriptor)return nil;NmbVisibleTable *handle=nmb_visible_table_new(compute,render,descriptor.functionCount,stage);
 if(!handle){NBLOG(@"native-metal: visible table refused: %@",lastError());return nil;}
 NBVisibleFunctionTable *result=[NBVisibleFunctionTable new];result.handle=handle;return (id<MTLVisibleFunctionTable>)result;
}
@implementation NBComputePipeline (X9FunctionTables)
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function {return x9FunctionHandle(self.handle,nullptr,function,0);}
- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)descriptor {return x9VisibleTable(self.handle,nullptr,descriptor,0);}
@end
@implementation NBRenderPipeline (X9FunctionTables)
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function stage:(MTLRenderStages)stage {return x9FunctionHandle(nullptr,self.handle,function,stage);}
- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)descriptor stage:(MTLRenderStages)stage {return x9VisibleTable(nullptr,self.handle,descriptor,stage);}
@end
@implementation NBEncoder (X9FunctionTables)
- (void)visibleTable:(id<MTLVisibleFunctionTable>)table index:(NSUInteger)index stage:(uint32_t)stage {
 if(table&&![(id)table isKindOfClass:[NBVisibleFunctionTable class]]){[self.commandBuffer refuse:@"foreign visible function table"];return;}
 if(table)[self.commandBuffer.resourceOwners addObject:table];
 [self check:nmb_encoder_visible_table(self.handle,stage,table?[(NBVisibleFunctionTable *)table handle]:nullptr,(uint32_t)index) what:"setVisibleFunctionTable"];
}
@end
@implementation NBComputeEncoder (X9FunctionTables)
- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)table atBufferIndex:(NSUInteger)index {[self visibleTable:table index:index stage:0];}
- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained [])tables withBufferRange:(NSRange)r {if(r.length&&!tables)return;for(NSUInteger k=0;k<r.length;k++)[self visibleTable:tables[k] index:r.location+k stage:0];}
@end
@implementation NBRenderEncoder (X9FunctionTables)
- (void)setVertexVisibleFunctionTable:(id<MTLVisibleFunctionTable>)table atBufferIndex:(NSUInteger)index {[self visibleTable:table index:index stage:1];}
- (void)setFragmentVisibleFunctionTable:(id<MTLVisibleFunctionTable>)table atBufferIndex:(NSUInteger)index {[self visibleTable:table index:index stage:2];}
- (void)setVertexVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained [])tables withBufferRange:(NSRange)r {if(r.length&&!tables)return;for(NSUInteger k=0;k<r.length;k++)[self visibleTable:tables[k] index:r.location+k stage:1];}
- (void)setFragmentVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained [])tables withBufferRange:(NSRange)r {if(r.length&&!tables)return;for(NSUInteger k=0;k<r.length;k++)[self visibleTable:tables[k] index:r.location+k stage:2];}
@end
@implementation NBArgumentEncoder (X9PipelineArguments)
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state atIndex:(NSUInteger)index {
 if(state&&![(id)state isKindOfClass:[NBComputePipeline class]])return;
 if(!nmb_argument_encoder_pipeline(self.handle,state?[(NBComputePipeline *)state handle]:nullptr,nullptr,(uint32_t)index))NBLOG(@"native-metal: argument pipeline: %@",lastError());
 if(state)[self.owners addObject:state];
}
- (void)setComputePipelineStates:(const id<MTLComputePipelineState> __unsafe_unretained [])states withRange:(NSRange)r {if(r.length&&!states)return;for(NSUInteger k=0;k<r.length;k++)[self setComputePipelineState:states[k] atIndex:r.location+k];}
- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)table atIndex:(NSUInteger)index {
 if(table&&![(id)table isKindOfClass:[NBVisibleFunctionTable class]])return;
 if(!nmb_argument_encoder_visible_table(self.handle,table?[(NBVisibleFunctionTable *)table handle]:nullptr,(uint32_t)index))NBLOG(@"native-metal: argument function table: %@",lastError());
 if(table)[self.owners addObject:table];
}
- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained [])tables withRange:(NSRange)r {if(r.length&&!tables)return;for(NSUInteger k=0;k<r.length;k++)[self setVisibleFunctionTable:tables[k] atIndex:r.location+k];}
@end

@interface NBFunctionConstant : NSObject
@property(nonatomic,copy) NSString *name;
@property(nonatomic) MTLDataType type;
@property(nonatomic) NSUInteger index;
@property(nonatomic) BOOL required;
@end
@implementation NBFunctionConstant
@end
@implementation NBFunction (X9MetadataAPI)
- (MTLPatchType)patchType {return (MTLPatchType)nmb_function_query(self.handle,0);}
- (NSInteger)patchControlPointCount {return (NSInteger)nmb_function_query(self.handle,1);}
- (NSDictionary<NSString *,MTLFunctionConstant *> *)functionConstantsDictionary {
 NSMutableDictionary *result=[NSMutableDictionary dictionary];NSUInteger count=nmb_function_query(self.handle,2);
 for(NSUInteger i=0;i<count;i++){char name[1024]={};uint64_t words[3]={};
  if(!nmb_function_constant_info(self.handle,i,name,sizeof name,words)){NBLOG(@"native-metal: function constant metadata: %@",lastError());return nil;}
  NBFunctionConstant *c=[NBFunctionConstant new];c.name=[NSString stringWithUTF8String:name];c.type=(MTLDataType)words[0];c.index=words[1];c.required=words[2]!=0;result[c.name]=c;
 }
 return (NSDictionary *)result;
}
@end
@implementation NBComputePipeline (X9BinaryFunctions)
- (id<MTLComputePipelineState>)newComputePipelineStateWithAdditionalBinaryFunctions:(NSArray<id<MTLFunction>> *)functions error:(NSError **)error {
 std::vector<const NmbFunction *> input;for(id f in functions){if(![f isKindOfClass:[NBFunction class]]){if(error)*error=nbError(@"foreign binary function");return nil;}input.push_back([(NBFunction *)f handle]);}
 NmbComputePipeline *handle=(NmbComputePipeline *)nmb_pipeline_adding(self.handle,nullptr,input.data(),input.size(),nullptr);
 if(!handle){if(error)*error=nbError(lastError());return nil;}NBComputePipeline *state=[NBComputePipeline new];state.handle=handle;if(error)*error=nil;return (id<MTLComputePipelineState>)state;
}
@end
@implementation NBRenderPipeline (X9BinaryFunctions)
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithAdditionalBinaryFunctions:(MTLRenderPipelineFunctionsDescriptor *)descriptor error:(NSError **)error {
 if(!descriptor){if(error)*error=nbError(@"nil binary functions descriptor");return nil;}
 std::vector<const NmbFunction *> input;std::vector<uint32_t> stages;
 NSArray *groups=@[descriptor.vertexAdditionalBinaryFunctions?:@[],descriptor.fragmentAdditionalBinaryFunctions?:@[],descriptor.tileAdditionalBinaryFunctions?:@[]];
 for(unsigned stage=0;stage<3;stage++)for(id f in groups[stage]){if(![f isKindOfClass:[NBFunction class]]){if(error)*error=nbError(@"foreign binary function");return nil;}input.push_back([(NBFunction *)f handle]);stages.push_back(stage+1);}
 NmbRenderPipeline *handle=(NmbRenderPipeline *)nmb_pipeline_adding(nullptr,self.handle,input.data(),input.size(),stages.data());
 if(!handle){if(error)*error=nbError(lastError());return nil;}NBRenderPipeline *state=[NBRenderPipeline new];state.handle=handle;if(error)*error=nil;return (id<MTLRenderPipelineState>)state;
}
@end

@implementation NBBlitEncoder (X9CopyOptions)
- (void)copyFromBuffer:(id<MTLBuffer>)source sourceOffset:(NSUInteger)offset sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)image sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)destination destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin options:(MTLBlitOption)options {
 if(![self buffers:NB_OBJECTS(source) count:1]||![self textures:NB_OBJECTS(destination) count:1])return;
 const uint64_t s[3]={size.width,size.height,size.depth},o[3]={origin.x,origin.y,origin.z};
 [self check:nmb_blit_buffer_texture_options(self.handle,[(NBBuffer *)source handle],[(NBTexture *)destination handle],offset,row,image,s,o,slice,level,options,true) what:"buffer-to-texture options"];
}
- (void)copyFromTexture:(id<MTLTexture>)source sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)destination destinationOffset:(NSUInteger)offset destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)image options:(MTLBlitOption)options {
 if(![self textures:NB_OBJECTS(source) count:1]||![self buffers:NB_OBJECTS(destination) count:1])return;
 const uint64_t s[3]={size.width,size.height,size.depth},o[3]={origin.x,origin.y,origin.z};
 [self check:nmb_blit_buffer_texture_options(self.handle,[(NBBuffer *)destination handle],[(NBTexture *)source handle],offset,row,image,s,o,slice,level,options,false) what:"texture-to-buffer options"];
}
- (void)copyFromTexture:(id<MTLTexture>)source sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel toTexture:(id<MTLTexture>)destination destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel sliceCount:(NSUInteger)slices levelCount:(NSUInteger)levels {
 if(![self textures:NB_OBJECTS(source,destination) count:2])return;
 NSUInteger sourceSlices=source.arrayLength*((source.textureType==MTLTextureTypeCube||source.textureType==MTLTextureTypeCubeArray)?6:1);
 NSUInteger destSlices=destination.arrayLength*((destination.textureType==MTLTextureTypeCube||destination.textureType==MTLTextureTypeCubeArray)?6:1);
 if(sourceSlice>sourceSlices||slices>sourceSlices-sourceSlice||destinationSlice>destSlices||slices>destSlices-destinationSlice||sourceLevel>source.mipmapLevelCount||levels>source.mipmapLevelCount-sourceLevel||destinationLevel>destination.mipmapLevelCount||levels>destination.mipmapLevelCount-destinationLevel){[self.commandBuffer refuse:@"texture copy level/slice range"];return;}
 for(NSUInteger s=0;s<slices;s++)for(NSUInteger l=0;l<levels;l++){
  NSUInteger level=sourceLevel+l;MTLSize size=MTLSizeMake(MAX(source.width>>level,(NSUInteger)1),MAX(source.height>>level,(NSUInteger)1),MAX(source.depth>>level,(NSUInteger)1));
  [self copyFromTexture:source sourceSlice:sourceSlice+s sourceLevel:level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size toTexture:destination destinationSlice:destinationSlice+s destinationLevel:destinationLevel+l destinationOrigin:MTLOriginMake(0,0,0)];
 }
}
@end

@implementation NBDeviceMethods (X9ArgumentDescriptors)
- (id<MTLArgumentEncoder>)newArgumentEncoderWithArguments:(NSArray<MTLArgumentDescriptor *> *)arguments {
 if(arguments.count>4096)return nil;std::vector<uint64_t> words;words.reserve(arguments.count*6);
 for(MTLArgumentDescriptor *d in arguments){for(uint64_t v:{(uint64_t)d.dataType,(uint64_t)d.index,(uint64_t)d.arrayLength,(uint64_t)d.access,(uint64_t)d.textureType,(uint64_t)d.constantBlockAlignment})words.push_back(v);}
 NmbArgumentEncoder *handle=nmb_argument_encoder_descriptors(gDevice,words.data(),arguments.count);
 if(!handle){NBLOG(@"native-metal: argument descriptors refused: %@",lastError());return nil;}
 NBArgumentEncoder *encoder=[NBArgumentEncoder new];encoder.handle=handle;return (id<MTLArgumentEncoder>)encoder;
}
@end

@implementation NBDeviceMethods (X9HeapQueries)
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
 uint64_t v[2]={};if(!nmb_device_heap_size(gDevice,nullptr,length,options,v))NBLOG(@"native-metal: heap buffer size: %@",lastError());return (MTLSizeAndAlign){v[0],v[1]};
}
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)descriptor {
 uint64_t v[2]={};NmbTextureDescriptor d={};if(textureDescriptor(descriptor,&d)&&!nmb_device_heap_size(gDevice,&d,0,0,v))NBLOG(@"native-metal: heap texture size: %@",lastError());return (MTLSizeAndAlign){v[0],v[1]};
}
@end
@implementation NBRenderEncoder (X9AmplificationAPI)
- (void)setVertexAmplificationCount:(NSUInteger)count viewMappings:(const MTLVertexAmplificationViewMapping *)mappings {
 if(count>8){[self.commandBuffer refuse:@"amplification count past 8"];return;}
 uint32_t v[16]={};if(mappings)for(NSUInteger i=0;i<count;i++){v[i*2]=mappings[i].viewportArrayIndexOffset;v[i*2+1]=mappings[i].renderTargetArrayIndexOffset;}
 [self check:nmb_render_amplification(self.handle,count,mappings?v:nullptr) what:"vertex amplification"];
}
@end
