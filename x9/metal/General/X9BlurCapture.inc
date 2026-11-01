// Limited CPU-side input capture for the original QuartzCore blur draws.
// No shader substitution, pixel readback or GPU synchronization changes.
- (void)buffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 if((stage==1 && index<4)||(stage==2 && index==1)) {
  if(!self.traceBuffers)self.traceBuffers=[NSMutableDictionary dictionary];
  if(buffer && [(id)buffer isKindOfClass:[NBBuffer class]])
   self.traceBuffers[@(stage*32+index)]=@{@"buffer":buffer,@"offset":@(offset)};
  else [self.traceBuffers removeObjectForKey:@(stage*32+index)];
 }
 [super buffer:buffer offset:offset index:index stage:stage];
}
- (void)bytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index stage:(uint32_t)stage {
 if((stage==1 && index<4)||(stage==2 && index==1)) {
  if(!self.traceBuffers)self.traceBuffers=[NSMutableDictionary dictionary];
  if(bytes && length<=4096)
   self.traceBuffers[@(stage*32+index)]=@{@"bytes":[NSData dataWithBytes:bytes length:MIN(length,512ul)],@"offset":@0};
  else [self.traceBuffers removeObjectForKey:@(stage*32+index)];
 }
 [super bytes:bytes length:length index:index stage:stage];
}
- (void)offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 NSDictionary *old=self.traceBuffers[@(stage*32+index)];
 if(old) {NSMutableDictionary *entry=[old mutableCopy];entry[@"offset"]=@(offset);self.traceBuffers[@(stage*32+index)]=entry;}
 [super offset:offset index:index stage:stage];
}
- (void)traceDraw:(MTLPrimitiveType)type start:(NSUInteger)start count:(NSUInteger)count indices:(NBBuffer *)indices offset:(NSUInteger)offset indexType:(MTLIndexType)indexType baseVertex:(NSInteger)baseVertex {
 NBRenderPipeline *pipeline=self.tracePipeline;if(!pipeline.traceFragment)return;
 // Two draws per shader pair, at most 24 in a process. No pointer identities.
 static std::mutex mutex;static NSMutableDictionary *seen;static unsigned total;
 unsigned serial;
 {std::lock_guard<std::mutex> lock(mutex);
  if(!seen)seen=[NSMutableDictionary dictionary];
  NSString *key=[NSString stringWithFormat:@"%@/%@",pipeline.traceVertex,pipeline.traceFragment];
  unsigned n=[seen[key] unsignedIntValue];if(n>=2||total>=24)return;
  seen[key]=@(n+1);serial=++total;
 }
 uint32_t ids[6]={};unsigned used=(unsigned)MIN(count,6ul);
 if(indices) {
  if(indices.storageMode!=MTLStorageModeShared)return;
  unsigned width=indexType==MTLIndexTypeUInt16?2:4;
  if(!indices.contents||offset>indices.length||used*width>indices.length-offset)return;
  for(unsigned i=0;i<used;i++) {
   uint32_t value=0;memcpy(&value,(const uint8_t *)indices.contents+offset+i*width,width);
   int64_t vertex=(int64_t)value+baseVertex;if(vertex<0||vertex>UINT32_MAX)return;ids[i]=(uint32_t)vertex;
  }
 }else for(unsigned i=0;i<used;i++){if(start+i>UINT32_MAX)return;ids[i]=(uint32_t)(start+i);}
 NBLOG(@"X9 BLUR DRAW: id=%u vs=%@ fs=%@ type=%llu count=%llu indexed=%u vertices=%u,%u,%u,%u,%u,%u",serial,pipeline.traceVertex,pipeline.traceFragment,(unsigned long long)type,(unsigned long long)count,indices!=nil,ids[0],ids[1],ids[2],ids[3],ids[4],ids[5]);
 for(unsigned a=0;a<31;a++) {
  MTLVertexAttributeDescriptor *attr=pipeline.traceLayout.attributes[a];
  if(!attr||attr.format==MTLVertexFormatInvalid)continue;
  MTLVertexBufferLayoutDescriptor *layout=pipeline.traceLayout.layouts[attr.bufferIndex];
  NBLOG(@"X9 BLUR ATTR: id=%u attr=%u fmt=%llu offset=%llu buffer=%llu stride=%llu step=%llu rate=%llu",serial,a,(unsigned long long)attr.format,(unsigned long long)attr.offset,(unsigned long long)attr.bufferIndex,(unsigned long long)layout.stride,(unsigned long long)layout.stepFunction,(unsigned long long)layout.stepRate);
 }
 for(NSNumber *key in self.traceBuffers) {
  unsigned stage=key.unsignedIntValue/32,slot=key.unsignedIntValue%32;
  NSDictionary *entry=self.traceBuffers[key];NBBuffer *buffer=entry[@"buffer"];NSData *data=entry[@"bytes"];
  if(buffer && buffer.storageMode!=MTLStorageModeShared)continue;
  const uint8_t *bytes=buffer?(const uint8_t *)buffer.contents:(const uint8_t *)data.bytes;
  uint64_t length=buffer?buffer.length:data.length,base=[entry[@"offset"] unsignedLongLongValue];
  if(!bytes||base>length)continue;
  MTLVertexBufferLayoutDescriptor *layout=stage==1?pipeline.traceLayout.layouts[slot]:nil;
  bool vertex=layout && layout.stepFunction==MTLVertexStepFunctionPerVertex && layout.stride;
  unsigned samples=vertex?used:1;
  for(unsigned i=0;i<samples;i++) {
   uint64_t at=base;if(vertex){if(ids[i]>(UINT64_MAX-at)/layout.stride)continue;at+=ids[i]*layout.stride;}
   if(at>length)continue;
   unsigned n=(unsigned)MIN(length-at,vertex?MIN(layout.stride,64ul):96ul);
   // Rows stay below USB's line size. Raw words preserve float/half bits.
   for(unsigned off=0;off<n;off+=32) {
    NSMutableString *hex=[NSMutableString string];
    for(unsigned j=off;j<MIN(off+32,n);j++)[hex appendFormat:@"%02x",bytes[at+j]];
    NBLOG(@"X9 BLUR DATA: id=%u stage=%u slot=%u vertex=%u offset=%llu row=%u hex=%@",serial,stage,slot,vertex?ids[i]:UINT32_MAX,(unsigned long long)at,off,hex);
   }
  }
 }
}
// Controlled isolation of the v14 indexed/reused-vertex path. The original
// downsample blur vertex AIR reads position/UV and buffer2/3 only: no vertex
// or instance ID. Expand just the captured six-index, one-instance quad into
// six identical vertex records; keep its shader, winding and every fragment
// input unchanged. Anything outside that exact contract takes the old path.
- (bool)drawBlurWithoutIndex:(MTLPrimitiveType)type count:(NSUInteger)count indices:(NBBuffer *)indices offset:(NSUInteger)offset indexType:(MTLIndexType)indexType instances:(NSUInteger)instances baseVertex:(NSInteger)baseVertex baseInstance:(NSUInteger)baseInstance {
 if(type!=MTLPrimitiveTypeTriangle||count!=6||instances!=1||baseInstance||
    ![self.tracePipeline.traceVertex isEqualToString:@"downsample_blur_vert_lph"]||
    indices.storageMode!=MTLStorageModeShared)return false;
 MTLVertexDescriptor *layout=self.tracePipeline.traceLayout;
 if(!layout)return false;
 for(unsigned a=0;a<31;a++) {
  MTLVertexAttributeDescriptor *attr=layout.attributes[a];
  if(attr.format!=MTLVertexFormatInvalid && attr.bufferIndex!=1)return false;
 }
 MTLVertexBufferLayoutDescriptor *binding=layout.layouts[1];
 if(binding.stride!=48||binding.stepFunction!=MTLVertexStepFunctionPerVertex||binding.stepRate!=1)return false;
 NSDictionary *entry=self.traceBuffers[@33];NBBuffer *buffer=entry[@"buffer"];
 if(!buffer||buffer.storageMode!=MTLStorageModeShared||!buffer.contents||!indices.contents)return false;
 unsigned width=indexType==MTLIndexTypeUInt16?2:4;uint32_t vertex[6];
 if(offset>indices.length||6*width>indices.length-offset)return false;
 for(unsigned j=0;j<6;j++) {
  uint32_t value=0;memcpy(&value,(const uint8_t *)indices.contents+offset+j*width,width);
  int64_t adjusted=(int64_t)value+baseVertex;if(adjusted<0||adjusted>UINT32_MAX)return false;vertex[j]=(uint32_t)adjusted;
 }
 if(vertex[0]!=vertex[5]||vertex[2]!=vertex[3]||vertex[1]!=vertex[0]+1||
    vertex[2]!=vertex[0]+2||vertex[4]!=vertex[0]+3)return false;
 uint64_t base=[entry[@"offset"] unsignedLongLongValue];uint8_t expanded[6*48];
 for(unsigned j=0;j<6;j++) {
  uint64_t at=base+(uint64_t)vertex[j]*48;
  if(at<base||at>buffer.length||48>buffer.length-at)return false;
  memcpy(expanded+j*48,(const uint8_t *)buffer.contents+at,48);
 }
 // The runtime copies inline bytes now and snapshots bindings per draw.
 // Restore the original binding so subsequent encoder calls see identical state.
 bool ok=nmb_encoder_set_bytes(self.handle,1,expanded,sizeof(expanded),1);
 if(ok)ok=nmb_render_draw(self.handle,type,0,6,1,0);
 bool restored=nmb_encoder_set_buffer(self.handle,1,buffer.handle,base,1);
 [self check:ok&&restored what:"blur deindex isolation"];
 static std::atomic<unsigned> draws{0};unsigned n=++draws;
 if(n<=4||n%128==0)NBLOG(@"X9 BLUR DEINDEX: draws=%u original_indices=%u,%u,%u,%u,%u,%u original_vertex_bytes=288 ok=%u",n,vertex[0],vertex[1],vertex[2],vertex[3],vertex[4],vertex[5],ok&&restored);
 return true;
}
