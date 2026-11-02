// x9vf whole-frame geometry capture (backboardd only, opt-in).
// At four uptime triggers, every draw of the next three command buffers is
// logged compactly (shaders, blend/write masks, pass, indices, decoded vertex
// position/texcoord, first 64 bytes of vertex slots 2/3). The vertex and index
// byte ranges each draw references are snapshotted at encode time and also
// copied by a GPU blit appended at commit; after completion the GPU copy is
// compared with the snapshot and with the CPU's current bytes. A mismatch
// shows the GPU-visible memory differs from what the CPU encoded; agreement
// moves the fault to vertex fetch/IDVS/raster. No draw, shader or state of the
// original work changes; the blit adds work after the original passes.

static uint64_t x9FrameObjectID(id object) {
 static char key;static std::atomic<uint64_t> next{0};
 if(!object)return 0;
 NSNumber *value=objc_getAssociatedObject(object,&key);
 if(!value){value=@(++next);objc_setAssociatedObject(object,&key,value,OBJC_ASSOCIATION_RETAIN);}
 return value.unsignedLongLongValue;
}
static NSString *x9FrameDecode(const uint8_t *p,uint64_t format) {
 // MTLVertexFormat: Half2..4 = 25..27, Float..Float4 = 28..31.
 if(format>=28&&format<=31) {
  float f[4]={};memcpy(f,p,4*(format-27));
  return [NSString stringWithFormat:@"%.6g,%.6g,%.6g,%.6g",f[0],f[1],f[2],f[3]];
 }
 if(format>=25&&format<=27) {
  __fp16 h[4]={};memcpy(h,p,2*(format-23));
  return [NSString stringWithFormat:@"%.5g,%.5g,%.5g,%.5g",(float)h[0],(float)h[1],(float)h[2],(float)h[3]];
 }
 NSMutableString *hex=[NSMutableString stringWithString:@"x"];
 for(unsigned i=0;i<16;i++)[hex appendFormat:@"%02x",p[i]];
 return hex;
}
static void x9FrameSelect(NBCommandBuffer *cb) {
 const X9CaptureOptions &options=captureOptions();
 if(!options.frame)return;
 static const bool server=!strcmp(getprogname(),"backboardd");
 if(!server)return;
 static std::mutex mutex;static unsigned trigger,remaining,frames;
 const uint64_t start=options.startGiven?options.geometryStartUs:60000000ull,now=nowUs();
 std::lock_guard<std::mutex> lock(mutex);
 if(!remaining && trigger<4 && now>=start+(uint64_t)trigger*20000000ull){trigger++;remaining=3;}
 if(!remaining)return;
 remaining--;cb.traceFrame=++frames;cb.traceFrameTrigger=trigger;
 NSLog(@"X9 FRAME CB: f=%u trigger=%u cb=%llu uptime_us=%llu",cb.traceFrame,trigger,(unsigned long long)cb.traceID,(unsigned long long)now);
}
// Appended at commit, after every original encoder ended. Returns false only
// on a local refusal, which is reported and leaves the command buffer as is.
static void x9FrameFlush(NBCommandBuffer *cb) {
 NSArray *copies=cb.traceFrameCopies;
 if(!cb.traceFrame||!copies.count||cb.encodeFailure)return;
 cb.traceFrameCopies=nil;
 uint64_t total=0;
 for(NSDictionary *c in copies)total+=([c[@"vlen"] unsignedLongLongValue]+15)/16*16+([c[@"ilen"] unsignedLongLongValue]+15)/16*16;
 const unsigned f=cb.traceFrame;const uint64_t cbID=cb.traceID;
 if(!total||total>8*1024*1024){NSLog(@"X9 FRAME GPU SKIP: f=%u cb=%llu bytes=%llu reason=budget",f,(unsigned long long)cbID,(unsigned long long)total);return;}
 NmbBuffer *raw=nmb_buffer_new(gDevice,total,MTLResourceStorageModeShared,nullptr);
 if(!raw){NSLog(@"X9 FRAME GPU SKIP: f=%u reason=allocation %@",f,lastError());return;}
 NBBuffer *readback=[NBBuffer new];readback.handle=raw;readback.options=MTLResourceStorageModeShared;
 NmbEncoder *blit=nmb_blit_encoder_new(cb.handle);
 if(!blit){NSLog(@"X9 FRAME GPU SKIP: f=%u reason=blit %@",f,lastError());return;}
 [cb.resourceOwners addObject:readback];
 NSMutableArray *placed=[NSMutableArray array];uint64_t at=0;
 for(NSDictionary *c in copies) {
  NSMutableDictionary *m=[c mutableCopy];
  for(NSString *k in @[@"v",@"i"]) {
   NBBuffer *src=c[[k stringByAppendingString:@"buf"]];
   const uint64_t len=[c[[k stringByAppendingString:@"len"]] unsignedLongLongValue],from=[c[[k stringByAppendingString:@"start"]] unsignedLongLongValue];
   if(!src||!len)continue;
   [cb.resourceOwners addObject:src];
   if(!nmb_blit_copy_buffer(blit,src.handle,from,raw,at,len)) {
    NSString *reason=lastError();nmb_encoder_discard(blit);
    NSLog(@"X9 FRAME GPU SKIP: f=%u reason=copy %@",f,reason);return;
   }
   m[[k stringByAppendingString:@"dst"]]=@(at);at+=(len+15)/16*16;
  }
  [placed addObject:m];
 }
 if(!nmb_encoder_end(blit)){NSLog(@"X9 FRAME GPU SKIP: f=%u reason=end %@",f,lastError());return;}
 NSLog(@"X9 FRAME GPU COPY: f=%u cb=%llu draws=%llu bytes=%llu",f,(unsigned long long)cbID,(unsigned long long)placed.count,(unsigned long long)total);
 [(id<MTLCommandBuffer>)cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
  if(done.status!=MTLCommandBufferStatusCompleted){NSLog(@"X9 FRAME GPU FAILED: f=%u status=%llu",f,(unsigned long long)done.status);return;}
  const uint8_t *gpu=(const uint8_t *)readback.contents;
  if(!gpu){NSLog(@"X9 FRAME GPU FAILED: f=%u reason=no-cpu-view",f);return;}
  unsigned bad=0;
  for(NSDictionary *c in placed)for(NSString *k in @[@"v",@"i"]) {
   NSData *snap=c[[k stringByAppendingString:@"snap"]];NBBuffer *src=c[[k stringByAppendingString:@"buf"]];
   if(!snap||!c[[k stringByAppendingString:@"dst"]])continue;
   const uint64_t dst=[c[[k stringByAppendingString:@"dst"]] unsignedLongLongValue],from=[c[[k stringByAppendingString:@"start"]] unsignedLongLongValue];
   const uint8_t *want=(const uint8_t *)snap.bytes,*got=gpu+dst,*now=src?(const uint8_t *)src.contents:nullptr;
   const uint64_t len=snap.length;uint64_t diff=0,first=UINT64_MAX,changed=0;
   for(uint64_t b=0;b<len;b++) {
    if(want[b]!=got[b]){diff++;if(first==UINT64_MAX)first=b;}
    if(now && now[from+b]!=want[b])changed++;
   }
   NSLog(@"X9 FRAME GPU CHECK: f=%u n=%@ kind=%@ start=%llu bytes=%llu gpu_diff=%llu first=%lld cpu_changed_since_encode=%llu",
    f,c[@"n"],k,(unsigned long long)from,(unsigned long long)len,(unsigned long long)diff,first==UINT64_MAX?-1ll:(long long)first,(unsigned long long)changed);
   if(diff && bad<24) {
    bad++;
    const uint64_t row=first&~15ull;
    NSMutableString *a=[NSMutableString string],*g=[NSMutableString string];
    for(uint64_t b=row;b<MIN(row+32,len);b++){[a appendFormat:@"%02x",want[b]];[g appendFormat:@"%02x",got[b]];}
    NSLog(@"X9 FRAME GPU DIFF: f=%u n=%@ kind=%@ at=%llu want=%@ got=%@",f,c[@"n"],k,(unsigned long long)(from+row),a,g);
   }
  }
  NSLog(@"X9 FRAME GPU END: f=%u",f);
 }];
}
- (void)traceFrameDraw:(MTLPrimitiveType)type start:(NSUInteger)start count:(NSUInteger)count indices:(NBBuffer *)indices offset:(NSUInteger)offset indexType:(MTLIndexType)indexType baseVertex:(NSInteger)baseVertex instances:(NSUInteger)instances {
 NBCommandBuffer *cb=self.commandBuffer;NBRenderPipeline *pipeline=self.tracePipeline;
 if(!cb.traceFrame||!pipeline||cb.traceFrameDraws>=160)return;
 const unsigned n=++cb.traceFrameDraws,f=cb.traceFrame;
 const unsigned width=indexType==MTLIndexTypeUInt16?2:4;
 MTLVertexDescriptor *layoutSet=pipeline.traceLayout;
 MTLVertexAttributeDescriptor *a0=layoutSet.attributes[0],*a1=layoutSet.attributes[1];
 const bool hasA0=a0&&a0.format!=MTLVertexFormatInvalid,hasA1=a1&&a1.format!=MTLVertexFormatInvalid;
 const unsigned vslot=hasA0?(unsigned)a0.bufferIndex:31;
 MTLVertexBufferLayoutDescriptor *vl=hasA0?layoutSet.layouts[vslot]:nil;
 NSDictionary *ventry=hasA0?self.traceBuffers[@(32+vslot)]:nil;
 NBBuffer *vbuf=ventry[@"buffer"];NSData *vdata=ventry[@"bytes"];
 const uint64_t vbase=[ventry[@"offset"] unsignedLongLongValue],vlength=vbuf?vbuf.length:vdata.length,stride=vl?vl.stride:0;
 NSLog(@"X9 FRAME DRAW: f=%u n=%u cb=%llu enc=%llu vs=%@ fs=%@ type=%llu count=%llu start=%llu indexed=%u itype=%llu ioff=%llu ibuf=%llu ilen=%llu istorage=%lld base_vertex=%lld inst=%llu",
  f,n,(unsigned long long)cb.traceID,(unsigned long long)self.globalTraceObjectID,pipeline.traceVertex,pipeline.traceFragment,(unsigned long long)type,(unsigned long long)count,(unsigned long long)start,
  indices!=nil,(unsigned long long)indexType,(unsigned long long)offset,(unsigned long long)x9FrameObjectID(indices),(unsigned long long)(indices?indices.length:0),indices?(long long)indices.storageMode:-1ll,(long long)baseVertex,(unsigned long long)instances);
 NSLog(@"X9 FRAME STATE: f=%u n=%u pass=%@ vp=%@ sc=%@ cull=%llu wind=%llu blend=%@",f,n,self.tracePass,
  self.traceViewportSet?[NSString stringWithFormat:@"%g,%g,%g,%g",self.traceViewport.originX,self.traceViewport.originY,self.traceViewport.width,self.traceViewport.height]:@"default",
  self.traceScissorSet?[NSString stringWithFormat:@"%llu,%llu,%llu,%llu",(unsigned long long)self.traceScissor.x,(unsigned long long)self.traceScissor.y,(unsigned long long)self.traceScissor.width,(unsigned long long)self.traceScissor.height]:@"default",
  [objc_getAssociatedObject(self,"cull") unsignedLongLongValue],[objc_getAssociatedObject(self,"winding") unsignedLongLongValue],pipeline.traceBlend);
 NSLog(@"X9 FRAME VBUF: f=%u n=%u slot=%u vbuf=%llu base=%llu length=%llu stride=%llu step=%llu inline=%u a0=%llu/%llu a1=%llu/%llu",f,n,vslot,
  (unsigned long long)x9FrameObjectID(vbuf),(unsigned long long)vbase,(unsigned long long)vlength,(unsigned long long)stride,(unsigned long long)(vl?vl.stepFunction:0),vdata!=nil,
  (unsigned long long)(hasA0?a0.format:0),(unsigned long long)(hasA0?a0.offset:0),(unsigned long long)(hasA1?a1.format:0),(unsigned long long)(hasA1?a1.offset:0));
 // Effective vertex ids, as Metal fetches them.
 const unsigned used=(unsigned)MIN(count,96ul);uint32_t ids[96]={};bool readable=true;const char *reason="ok";
 const uint8_t *ib=nullptr;
 if(indices) {
  if(indexType!=MTLIndexTypeUInt16&&indexType!=MTLIndexTypeUInt32){readable=false;reason="index-type";}
  else if(indices.storageMode!=MTLStorageModeShared){readable=false;reason="not-shared";}
  else if(!(ib=(const uint8_t *)indices.contents)){readable=false;reason="no-contents";}
  else if(offset>indices.length||(uint64_t)used*width>indices.length-offset){readable=false;reason="index-OOB";}
 }
 uint32_t lo=UINT32_MAX,hi=0;
 if(readable)for(unsigned i=0;i<used;i++) {
  uint32_t value=0;if(indices)memcpy(&value,ib+offset+(uint64_t)i*width,width);
  const int64_t v=indices?(int64_t)value+baseVertex:(int64_t)start+i;
  if(v<0||v>UINT32_MAX){readable=false;reason="vertex-range";break;}
  ids[i]=(uint32_t)v;lo=MIN(lo,ids[i]);hi=MAX(hi,ids[i]);
 }
 NSLog(@"X9 FRAME IDX: f=%u n=%u readable=%u reason=%s captured=%u total=%llu min=%u max=%u",f,n,readable,reason,readable?used:0,(unsigned long long)count,readable?lo:0,readable?hi:0);
 if(readable&&indices)for(unsigned row=0;row<used;row+=24) {
  NSMutableString *s=[NSMutableString string];
  for(unsigned i=row;i<MIN(row+24,used);i++)[s appendFormat:@"%s%u",i==row?"":",",ids[i]];
  NSLog(@"X9 FRAME I: f=%u n=%u first=%u v=%@",f,n,row,s);
 }
 const uint8_t *vb=vbuf?(vbuf.storageMode==MTLStorageModeShared?(const uint8_t *)vbuf.contents:nullptr):(const uint8_t *)vdata.bytes;
 const bool vok=readable&&vb&&stride&&vl.stepFunction==MTLVertexStepFunctionPerVertex&&hi!=0xffffffffu&&
  vbase<=vlength&&(uint64_t)(hi+1)*stride<=vlength-vbase;
 if(readable&&hasA0&&!vok)NSLog(@"X9 FRAME VSKIP: f=%u n=%u reason=%s",f,n,!vb?"unreadable":!stride?"no-stride":"range-or-step");
 if(vok)for(uint32_t v=lo,k=0;v<=hi&&k<64;v++,k++) {
  const uint8_t *p=vb+vbase+(uint64_t)v*stride;
  NSLog(@"X9 FRAME V: f=%u n=%u v=%u p=%@ t=%@",f,n,v,x9FrameDecode(p+a0.offset,a0.format),hasA1?x9FrameDecode(p+a1.offset,a1.format):@"-");
 }
 for(unsigned slot=2;slot<=3;slot++) {
  NSDictionary *e=self.traceBuffers[@(32+slot)];if(!e||slot==vslot)continue;
  NBBuffer *b=e[@"buffer"];NSData *d=e[@"bytes"];
  const uint8_t *bytes=b?(b.storageMode==MTLStorageModeShared?(const uint8_t *)b.contents:nullptr):(const uint8_t *)d.bytes;
  const uint64_t base=[e[@"offset"] unsignedLongLongValue],len=b?b.length:d.length;
  if(!bytes||base>len||len-base<16)continue;
  float m[16]={};memcpy(m,bytes+base,MIN(64ull,(len-base)/4*4));
  NSMutableString *s=[NSMutableString string];
  for(unsigned i=0;i<16;i++)[s appendFormat:@"%s%.6g",i?",":"",m[i]];
  NSLog(@"X9 FRAME U: f=%u n=%u slot=%u base=%llu f32=%@",f,n,slot,(unsigned long long)base,s);
 }
 // GPU-visible copy of exactly the bytes this draw fetches.
 if(cb.traceFrameBytes>=4*1024*1024)return;
 NSMutableDictionary *copy=[NSMutableDictionary dictionaryWithObject:@(n) forKey:@"n"];
 // Blit ranges are widened to 4-byte alignment inside the buffer.
 auto aligned=[](uint64_t from,uint64_t len,uint64_t total,uint64_t *outFrom,uint64_t *outLen) {
  uint64_t a=from&~3ull,b=(from+len+3)&~3ull;if(b>total)b=total&~3ull;
  if(b<=a)return false;*outFrom=a;*outLen=b-a;return true;
 };
 uint64_t from=0,len=0;
 if(vok&&vbuf&&aligned(vbase+(uint64_t)lo*stride,MIN((uint64_t)(hi-lo+1)*stride,65536ull),vbuf.length,&from,&len)) {
  copy[@"vbuf"]=vbuf;copy[@"vstart"]=@(from);copy[@"vlen"]=@(len);copy[@"vsnap"]=[NSData dataWithBytes:vb+from length:len];
  cb.traceFrameBytes+=len;
 }
 if(readable&&indices&&ib&&count&&aligned(offset,MIN((uint64_t)count*width,(uint64_t)(indices.length-offset)),indices.length,&from,&len)) {
  copy[@"ibuf"]=indices;copy[@"istart"]=@(from);copy[@"ilen"]=@(len);copy[@"isnap"]=[NSData dataWithBytes:ib+from length:len];
  cb.traceFrameBytes+=len;
 }
 if(copy.count>1){if(!cb.traceFrameCopies)cb.traceFrameCopies=[NSMutableArray array];[cb.traceFrameCopies addObject:copy];}
}
