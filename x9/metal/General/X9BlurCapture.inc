// Limited CPU-side input capture for the original QuartzCore blur draws.
// No shader substitution, pixel readback or GPU synchronization changes.
- (void)buffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 if((captureOptions().inputs && ((stage==1 && index<4)||(stage==2 && index==1))) ||
    (captureOptions().tracksGeometry() && ((stage==1 && index<31)||(stage==2 && (index==1||index==6))))) {
  if(!self.traceBuffers)self.traceBuffers=[NSMutableDictionary dictionary];
  if(buffer && [(id)buffer isKindOfClass:[NBBuffer class]])
   self.traceBuffers[@(stage*32+index)]=@{@"buffer":buffer,@"offset":@(offset)};
  else [self.traceBuffers removeObjectForKey:@(stage*32+index)];
 }
 [super buffer:buffer offset:offset index:index stage:stage];
}
- (void)bytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index stage:(uint32_t)stage {
 if((captureOptions().inputs && ((stage==1 && index<4)||(stage==2 && index==1))) ||
    (captureOptions().tracksGeometry() && ((stage==1 && index<31)||(stage==2 && (index==1||index==6))))) {
  if(!self.traceBuffers)self.traceBuffers=[NSMutableDictionary dictionary];
  if(bytes && length<=4096)
   self.traceBuffers[@(stage*32+index)]=@{@"bytes":[NSData dataWithBytes:bytes length:MIN(length,512ul)],@"length":@(length),@"offset":@0};
  else [self.traceBuffers removeObjectForKey:@(stage*32+index)];
 }
 [super bytes:bytes length:length index:index stage:stage];
}
- (void)offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 if(captureOptions().inputs||captureOptions().tracksGeometry()) {
  NSDictionary *old=self.traceBuffers[@(stage*32+index)];
  if(old) {
   NSMutableDictionary *entry=[old mutableCopy];
   if(old[@"bytes"]) {
    // Match the active runtime's set_buffer_offset_with_stride: inline
    // bytes become zeros, NOT the former bytes at the requested offset.
    entry[@"bytes"]=[NSMutableData dataWithLength:[old[@"bytes"] length]];
    entry[@"offset"]=@0;entry[@"zeroedInline"]=@YES;entry[@"requestedOffset"]=@(offset);
   }else entry[@"offset"]=@(offset);
   self.traceBuffers[@(stage*32+index)]=entry;
  }
 }
 [super offset:offset index:index stage:stage];
}
- (void)traceDraw:(MTLPrimitiveType)type start:(NSUInteger)start count:(NSUInteger)count indices:(NBBuffer *)indices offset:(NSUInteger)offset indexType:(MTLIndexType)indexType baseVertex:(NSInteger)baseVertex instances:(NSUInteger)instances baseInstance:(NSUInteger)baseInstance {
 const X9CaptureOptions &options=captureOptions();
 NBRenderPipeline *pipeline=self.tracePipeline;
 if(options.frame)[self traceFrameDraw:type start:start count:count indices:indices offset:offset indexType:indexType baseVertex:baseVertex instances:instances];
 if(options.geometry && pipeline) {
  const uint64_t uptime=nowUs();
  if(uptime>=options.geometryStartUs && uptime-options.geometryStartUs<90000000) {
   const unsigned window=(unsigned)((uptime-options.geometryStartUs)/30000000);
   const bool blur=[pipeline.traceVertex containsString:@"blur"]||[pipeline.traceFragment containsString:@"blur"];
   // Reserve independent quotas: a run of tiny blur quads cannot consume
   // the multi-quad or final-composite evidence budget. Reset by uptime,
   // not process start, so newly launched render services use the same window.
   const unsigned category=blur?(count>6?0:1):2;
   static std::mutex geometryMutex;static unsigned quotas[3][3]={},total;
   static NSMutableSet *seen;unsigned serial=0;
   {
    std::lock_guard<std::mutex> lock(geometryMutex);
    if(!seen)seen=[NSMutableSet set];
    NSString *key=[NSString stringWithFormat:@"%u/%@/%@/%llu/%u",window,pipeline.traceVertex,pipeline.traceFragment,(unsigned long long)count,indices!=nil];
    if(count && quotas[window][category]<2 && ![seen containsObject:key]) {
     [seen addObject:key];quotas[window][category]++;serial=++total;
    }
   }
   if(serial) {
    self.commandBuffer.traceGeometryUsed=YES;
    NSLog(@"X9 GEOM DRAW: id=%u cb=%llu encoder=%llu uptime_us=%llu window=%u category=%u",serial,(unsigned long long)self.commandBuffer.traceID,(unsigned long long)self.globalTraceObjectID,(unsigned long long)uptime,window,category);
    NSLog(@"X9 GEOM SHADER: id=%u vs=%@ fs=%@",serial,pipeline.traceVertex,pipeline.traceFragment);
    NSLog(@"X9 GEOM ARGS: id=%u type=%llu start=%llu count=%llu indexed=%u index_type=%llu index_offset=%llu",serial,(unsigned long long)type,(unsigned long long)start,(unsigned long long)count,indices!=nil,(unsigned long long)indexType,(unsigned long long)offset);
    NSLog(@"X9 GEOM BASE: id=%u base_vertex=%lld instances=%llu base_instance=%llu",serial,(long long)baseVertex,(unsigned long long)instances,(unsigned long long)baseInstance);
    for(NSString *part in [self.tracePass componentsSeparatedByString:@" "])
     if(part.length)NSLog(@"X9 GEOM PASS: id=%u %@",serial,part);
    MTLViewport viewport=self.traceViewport;MTLScissorRect scissor=self.traceScissor;
    if(self.traceViewportSet)NSLog(@"X9 GEOM VIEWPORT: id=%u explicit=1 x=%.17g y=%.17g w=%.17g h=%.17g near=%.17g far=%.17g",serial,viewport.originX,viewport.originY,viewport.width,viewport.height,viewport.znear,viewport.zfar);
    else NSLog(@"X9 GEOM VIEWPORT: id=%u explicit=0 default=render-area/near0/far1",serial);
    if(self.traceScissorSet)NSLog(@"X9 GEOM SCISSOR: id=%u explicit=1 x=%llu y=%llu w=%llu h=%llu",serial,(unsigned long long)scissor.x,(unsigned long long)scissor.y,(unsigned long long)scissor.width,(unsigned long long)scissor.height);
    else NSLog(@"X9 GEOM SCISSOR: id=%u explicit=0 default=render-area",serial);
    NSLog(@"X9 GEOM RASTER: id=%u cull=%llu winding=%llu fill=%llu",serial,[objc_getAssociatedObject(self,"cull") unsignedLongLongValue],[objc_getAssociatedObject(self,"winding") unsignedLongLongValue],[objc_getAssociatedObject(self,"fill") unsignedLongLongValue]);
    uint32_t ids[96]={},unique[96]={};unsigned used=(unsigned)MIN(count,96ul),uniqueCount=0;
    bool readable=true;const char *reason="ok";
    const unsigned width=indexType==MTLIndexTypeUInt16?2:4;
    const uint8_t *indexBytes=nullptr;
    if(indices) {
     NSLog(@"X9 GEOM INDEXBUF: id=%u length=%llu storage=%llu",serial,(unsigned long long)indices.length,(unsigned long long)indices.storageMode);
     if(indexType!=MTLIndexTypeUInt16 && indexType!=MTLIndexTypeUInt32){readable=false;reason="index-type";}
     else if(indices.storageMode!=MTLStorageModeShared){readable=false;reason="not-CPU-shared";}
     else if(!(indexBytes=(const uint8_t *)indices.contents)){readable=false;reason="no-contents";}
     else if(offset>indices.length || (uint64_t)used*width>indices.length-offset){readable=false;reason="index-OOB";}
    }
    if(readable)for(unsigned i=0;i<used;i++) {
     uint32_t value=0;
     if(indices)memcpy(&value,indexBytes+offset+(uint64_t)i*width,width);
     const int64_t vertex=indices?(int64_t)value+baseVertex:(int64_t)start+i;
     if(vertex<0||vertex>UINT32_MAX){readable=false;reason="vertex-range";break;}
     ids[i]=(uint32_t)vertex;
     if(std::find(unique,unique+uniqueCount,ids[i])==unique+uniqueCount)unique[uniqueCount++]=ids[i];
    }
    NSLog(@"X9 GEOM INDICES: id=%u captured=%u total=%llu complete=%u readable=%u reason=%s unique=%u",serial,readable?used:0,(unsigned long long)count,readable&&count==used,readable,reason,readable?uniqueCount:0);
    if(readable)for(unsigned row=0;row<used;row+=12) {
     NSMutableString *values=[NSMutableString string];
     for(unsigned i=row;i<MIN(row+12,used);++i)[values appendFormat:@"%s%u",i==row?"":",",ids[i]];
     NSLog(@"X9 GEOM INDEX: id=%u first=%u vertices=%@",serial,row,values);
    }
    for(unsigned a=0;a<31;a++) {
     MTLVertexAttributeDescriptor *attr=pipeline.traceLayout.attributes[a];
     if(!attr||attr.format==MTLVertexFormatInvalid)continue;
     MTLVertexBufferLayoutDescriptor *layout=pipeline.traceLayout.layouts[attr.bufferIndex];
     NSLog(@"X9 GEOM ATTR: id=%u attr=%u fmt=%llu offset=%llu buffer=%llu stride=%llu step=%llu rate=%llu",serial,a,(unsigned long long)attr.format,(unsigned long long)attr.offset,(unsigned long long)attr.bufferIndex,(unsigned long long)layout.stride,(unsigned long long)layout.stepFunction,(unsigned long long)layout.stepRate);
    }
    for(NSNumber *key in [[self.traceBuffers allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
     unsigned stage=key.unsignedIntValue/32,slot=key.unsignedIntValue%32;
     NSDictionary *entry=self.traceBuffers[key];NBBuffer *buffer=entry[@"buffer"];NSData *data=entry[@"bytes"];
     uint64_t base=[entry[@"offset"] unsignedLongLongValue],length=buffer?buffer.length:data.length;
     NSLog(@"X9 GEOM BIND: id=%u stage=%u slot=%u base=%llu length=%llu inline=%u zeroed_inline=%u requested_offset=%llu storage=%llu",serial,stage,slot,(unsigned long long)base,(unsigned long long)(buffer?length:[entry[@"length"] unsignedLongLongValue]),data!=nil,[entry[@"zeroedInline"] boolValue],(unsigned long long)[entry[@"requestedOffset"] unsignedLongLongValue],(unsigned long long)(buffer?buffer.storageMode:MTLStorageModeShared));
     if(buffer && buffer.storageMode!=MTLStorageModeShared){NSLog(@"X9 GEOM SKIP: id=%u stage=%u slot=%u reason=not-CPU-shared",serial,stage,slot);continue;}
     const uint8_t *bytes=buffer?(const uint8_t *)buffer.contents:(const uint8_t *)data.bytes;
     MTLVertexBufferLayoutDescriptor *layout=stage==1?pipeline.traceLayout.layouts[slot]:nil;
     const bool perVertex=layout && layout.stepFunction==MTLVertexStepFunctionPerVertex && layout.stride;
     const bool perInstance=layout && layout.stepFunction==MTLVertexStepFunctionPerInstance;
     // Do not silently call instance-stepped data or an unreadable index
     // list a validated vertex set. The recorded layout preserves that gap.
     if(perInstance || (perVertex&&!readable) || !bytes || base>length){NSLog(@"X9 GEOM SKIP: id=%u stage=%u slot=%u reason=unavailable-or-instance-stepped",serial,stage,slot);continue;}
     const unsigned samples=perVertex?uniqueCount:1;
     for(unsigned i=0;i<samples;i++) {
      uint64_t at=base;
      if(perVertex){if(unique[i]>(UINT64_MAX-at)/layout.stride)continue;at+=(uint64_t)unique[i]*layout.stride;}
      const uint64_t wanted=perVertex?layout.stride:MIN(length-base,96ull);
      if(at>length || wanted>length-at){NSLog(@"X9 GEOM OOB: id=%u stage=%u slot=%u vertex=%u at=%llu need=%llu length=%llu",serial,stage,slot,perVertex?unique[i]:UINT32_MAX,(unsigned long long)at,(unsigned long long)wanted,(unsigned long long)length);continue;}
      unsigned n=(unsigned)MIN(wanted,perVertex?64ull:96ull);
      NSLog(@"X9 GEOM RECORD: id=%u stage=%u slot=%u vertex=%u record_bytes=%llu captured_bytes=%u",serial,stage,slot,perVertex?unique[i]:UINT32_MAX,(unsigned long long)wanted,n);
      for(unsigned row=0;row<n;row+=32) {
       NSMutableString *hex=[NSMutableString string];
       for(unsigned j=row;j<MIN(row+32,n);j++)[hex appendFormat:@"%02x",bytes[at+j]];
       NSLog(@"X9 GEOM DATA: id=%u stage=%u slot=%u vertex=%u offset=%llu row=%u hex=%@",serial,stage,slot,perVertex?unique[i]:UINT32_MAX,(unsigned long long)at,row,hex);
      }
     }
    }
    NSLog(@"X9 GEOM END: id=%u CPU-encode-snapshot-not-GPU-output",serial);
   }
  }
 }
 if(!captureOptions().inputs)return;
 [self traceGraph:type count:count];
 if(![pipeline.traceVertex containsString:@"blur"] && ![pipeline.traceFragment containsString:@"blur"])return;
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
