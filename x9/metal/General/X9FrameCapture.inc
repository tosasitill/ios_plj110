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
// x9vf: the exact function-constant values of every specialized function
// (CoreAnimation's uber shaders), keyed by the same object ID FRAME STATE
// prints, so a captured draw can be recompiled offline with its constants.
static void x9LogFunctionConstants(id function,NSString *name,const std::vector<NmbConstant> &constants) {
 if(!captureOptions().frame)return;
 static std::mutex mutex;static unsigned lines;
 std::lock_guard<std::mutex> lock(mutex);
 if(lines>=6000)return;
 const uint64_t fid=x9FrameObjectID(function);
 NSLog(@"X9 FC FUNC: pid=%d fid=%llu name=%@ count=%zu",getpid(),(unsigned long long)fid,name,constants.size());lines++;
 for(size_t row=0;row<constants.size();row+=12) {
  NSMutableString *s=[NSMutableString string];
  for(size_t i=row;i<MIN(row+12,constants.size());i++) {
   const NmbConstant &c=constants[i];
   if(c.name)[s appendFormat:@" %s:%llu:",c.name,(unsigned long long)c.type];
   else [s appendFormat:@" %llu:%llu:",(unsigned long long)c.index,(unsigned long long)c.type];
   for(uint64_t b=0;b<c.length&&b<32;b++)[s appendFormat:@"%02x",((const uint8_t *)c.bytes)[b]];
  }
  NSLog(@"X9 FC C: fid=%llu row=%zu%@",(unsigned long long)fid,row,s);lines++;
 }
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
 if(!remaining && trigger<2 && now>=start+(uint64_t)trigger*20000000ull){trigger++;remaining=3;}
 if(!remaining)return;
 remaining--;cb.traceFrame=++frames;cb.traceFrameTrigger=trigger;
 NSLog(@"X9 FRAME CB: f=%u trigger=%u cb=%llu uptime_us=%llu",cb.traceFrame,trigger,(unsigned long long)cb.traceID,(unsigned long long)now);
}
// Appended at commit, after every original encoder ended. Returns false only
// on a local refusal, which is reported and leaves the command buffer as is.
static void x9FrameReplay(NBCommandBuffer *cb) {
 NSArray *replays=cb.traceFrameReplays;
 if(!replays.count||cb.encodeFailure)return;
 cb.traceFrameReplays=nil;cb.traceFrameReplaying=YES;
 id<MTLDevice> device=(id<MTLDevice>)gDeviceObject;
 NSMutableArray *results=[NSMutableArray array];uint64_t total=0;
 for(NSDictionary *r in replays) {
  NSArray *box=r[@"box"];const uint64_t bw=[box[2] unsignedLongLongValue],bh=[box[3] unsignedLongLongValue];
  total+=((bw*4+255)&~255ull)*bh*3;
 }
 if(total>96ull*1024*1024){NSLog(@"X9 FRAME REPLAY SKIP: f=%u bytes=%llu reason=budget",cb.traceFrame,(unsigned long long)total);cb.traceFrameReplaying=NO;return;}
 id<MTLBuffer> readback=[device newBufferWithLength:total options:MTLResourceStorageModeShared];
 if(!readback){NSLog(@"X9 FRAME REPLAY SKIP: f=%u reason=readback",cb.traceFrame);cb.traceFrameReplaying=NO;return;}
 uint64_t at=0;
 for(NSDictionary *r in replays) {
  NBTexture *a0=r[@"att0"];id a1=r[@"att1"];
  MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:a0.pixelFormat width:a0.width height:a0.height mipmapped:NO];
  td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;td.storageMode=MTLStorageModePrivate;
  id<MTLTexture> c0=[device newTextureWithDescriptor:td],c1=nil;
  if([a1 isKindOfClass:[NBTexture class]]) {
   MTLTextureDescriptor *t1=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:[(NBTexture *)a1 pixelFormat] width:a0.width height:a0.height mipmapped:NO];
   t1.usage=MTLTextureUsageRenderTarget;t1.storageMode=MTLStorageModePrivate;c1=[device newTextureWithDescriptor:t1];
  }
  if(!c0){NSLog(@"X9 FRAME REPLAY SKIP: n=%@ reason=texture",r[@"n"]);continue;}
  NSArray *box=r[@"box"];const uint64_t bx=[box[0] unsignedLongLongValue],by=[box[1] unsignedLongLongValue],bw=[box[2] unsignedLongLongValue],bh=[box[3] unsignedLongLongValue];
  const uint64_t row=(bw*4+255)&~255ull;const NSUInteger start=[r[@"start"] unsignedIntegerValue],count=[r[@"count"] unsignedIntegerValue];
  MTLViewport v;[r[@"viewport"] getValue:&v];
  const NSUInteger ranges[3][2]={{start,count},{start+count-3,3},{start+6,count-6}};
  for(unsigned variant=0;variant<3;variant++) {
   if(!ranges[variant][1])continue;
   MTLRenderPassDescriptor *pd=[MTLRenderPassDescriptor renderPassDescriptor];
   pd.colorAttachments[0].texture=c0;pd.colorAttachments[0].loadAction=MTLLoadActionClear;
   pd.colorAttachments[0].clearColor=MTLClearColorMake(1,0,1,1);pd.colorAttachments[0].storeAction=MTLStoreActionStore;
   if(c1){pd.colorAttachments[1].texture=c1;pd.colorAttachments[1].loadAction=MTLLoadActionClear;
    pd.colorAttachments[1].clearColor=MTLClearColorMake(0,0,0,0);pd.colorAttachments[1].storeAction=MTLStoreActionDontCare;}
   id<MTLRenderCommandEncoder> e=[(id<MTLCommandBuffer>)cb renderCommandEncoderWithDescriptor:pd];
   if(!e){NSLog(@"X9 FRAME REPLAY SKIP: n=%@ reason=encoder",r[@"n"]);break;}
   [e setRenderPipelineState:r[@"pipeline"]];[e setViewport:v];
   [e setCullMode:(MTLCullMode)[r[@"cull"] unsignedIntegerValue]];[e setFrontFacingWinding:(MTLWinding)[r[@"winding"] unsignedIntegerValue]];
   NSDictionary *buffers=r[@"buffers"];
   for(NSNumber *key in buffers) {
    const unsigned stage=key.unsignedIntValue/32,slot=key.unsignedIntValue%32;
    NSDictionary *entry=buffers[key];NBBuffer *b=entry[@"buffer"];NSData *bytes=entry[@"bytes"];
    const NSUInteger off=[entry[@"offset"] unsignedIntegerValue];
    if(stage==1){if(b)[e setVertexBuffer:b offset:off atIndex:slot];else if(bytes.length)[e setVertexBytes:bytes.bytes length:bytes.length atIndex:slot];}
    else if(stage==2){if(b)[e setFragmentBuffer:b offset:off atIndex:slot];else if(bytes.length)[e setFragmentBytes:bytes.bytes length:bytes.length atIndex:slot];}
   }
   NSDictionary *textures=r[@"textures"];for(NSNumber *key in textures)[e setFragmentTexture:textures[key] atIndex:key.unsignedIntegerValue];
   NSDictionary *samplers=r[@"samplers"];for(NSNumber *key in samplers)[e setFragmentSamplerState:samplers[key] atIndex:key.unsignedIntegerValue];
   [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:ranges[variant][0] vertexCount:ranges[variant][1]];
   [e endEncoding];
   id<MTLBlitCommandEncoder> blit=[(id<MTLCommandBuffer>)cb blitCommandEncoder];
   [blit copyFromTexture:c0 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(bx,by,0) sourceSize:MTLSizeMake(bw,bh,1)
                toBuffer:readback destinationOffset:at destinationBytesPerRow:row destinationBytesPerImage:row*bh];
   [blit endEncoding];
   [results addObject:@{@"r":r,@"variant":@(variant),@"at":@(at),@"row":@(row),@"first":@(ranges[variant][0]-start),@"count":@(ranges[variant][1])}];
   at+=row*bh;
  }
 }
 cb.traceFrameReplaying=NO;
 const unsigned f=cb.traceFrame;
 NSLog(@"X9 FRAME REPLAY ENCODED: f=%u draws=%llu passes=%llu",f,(unsigned long long)replays.count,(unsigned long long)results.count);
 [(id<MTLCommandBuffer>)cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
  if(done.status!=MTLCommandBufferStatusCompleted){NSLog(@"X9 FRAME REPLAY FAILED: f=%u status=%llu",f,(unsigned long long)done.status);return;}
  const uint8_t *bytes=(const uint8_t *)readback.contents;if(!bytes)return;
  for(NSDictionary *res in results) {
   NSDictionary *r=res[@"r"];NSArray *pts=r[@"points"];NSArray *box=r[@"box"];
   const double bx=[box[0] doubleValue],by=[box[1] doubleValue];const int64_t bw=[box[2] longLongValue],bh=[box[3] longLongValue];
   const uint64_t base=[res[@"at"] unsignedLongLongValue],row=[res[@"row"] unsignedLongLongValue];
   const unsigned first=[res[@"first"] unsignedIntValue],cnt=[res[@"count"] unsignedIntValue];
   uint64_t magenta=0,sampled=0;
   for(int64_t y=0;y<bh;y+=4)for(int64_t x=0;x<bw;x+=4){const uint8_t *q=bytes+base+y*row+x*4;sampled++;if(q[0]==255&&q[1]==0&&q[2]==255&&q[3]==255)magenta++;}
   NSLog(@"X9 FRAME REPLAY RESULT: f=%u n=%@ variant=%@ first=%u count=%u box_uncovered=%llu/%llu",f,r[@"n"],res[@"variant"],first,cnt,(unsigned long long)magenta,(unsigned long long)sampled);
   for(unsigned t=first;t+3<=first+cnt;t+=3) {
    double P[3][2];for(unsigned k=0;k<3;k++){P[k][0]=[pts[t+k][0] doubleValue];P[k][1]=[pts[t+k][1] doubleValue];}
    const double area=((P[1][0]-P[0][0])*(P[2][1]-P[0][1])-(P[2][0]-P[0][0])*(P[1][1]-P[0][1]))/2;
    const double cx=(P[0][0]+P[1][0]+P[2][0])/3,cy=(P[0][1]+P[1][1]+P[2][1])/3;
    unsigned covered=0,tested=0;NSMutableString *px=[NSMutableString string];
    for(unsigned k=0;k<4;k++) {
     const double sx=k<3?cx+(P[k][0]-cx)*0.5:cx,sy=k<3?cy+(P[k][1]-cy)*0.5:cy;
     const int64_t ix=(int64_t)floor(sx-bx),iy=(int64_t)floor(sy-by);
     if(fabs(area)<1||ix<0||iy<0||ix>=bw||iy>=bh)continue;
     const uint8_t *q=bytes+base+iy*row+ix*4;tested++;
     if(!(q[0]==255&&q[1]==0&&q[2]==255&&q[3]==255))covered++;
     [px appendFormat:@" %02x%02x%02x%02x",q[0],q[1],q[2],q[3]];
    }
    NSLog(@"X9 FRAME REPLAY TRI: f=%u n=%@ variant=%@ tri=%u area=%.1f p=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) covered=%u/%u bgra=%@",
     f,r[@"n"],res[@"variant"],t/3,area,P[0][0],P[0][1],P[1][0],P[1][1],P[2][0],P[2][1],covered,tested,px);
   }
  }
  NSLog(@"X9 FRAME REPLAY END: f=%u",f);
 }];
}
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
// Isolated replay of Control-Center-sized non-indexed triangle lists: after
// the original passes, the same pipeline/buffers/textures/samplers redraw the
// draw alone into a magenta-cleared target (whole draw, last triangle alone,
// draw without its first two triangles), and each real triangle's interior
// is sampled after completion. Shows whether the GPU rasterizes a triangle
// at all, independent of other draws in the original pass.
- (void)sampler:(id<MTLSamplerState>)sampler index:(NSUInteger)index stage:(uint32_t)stage {
 if(captureOptions().frame && stage==2 && index<16) {
  if(!self.traceSamplers)self.traceSamplers=[NSMutableDictionary dictionary];
  if(sampler)self.traceSamplers[@(index)]=sampler;else [self.traceSamplers removeObjectForKey:@(index)];
 }
 [super sampler:sampler index:index stage:stage];
}
- (void)traceFrameSelectReplay:(MTLPrimitiveType)type start:(NSUInteger)start count:(NSUInteger)count indexed:(bool)indexed n:(unsigned)n vertices:(const uint8_t *)vertices stride:(uint64_t)stride a0:(MTLVertexAttributeDescriptor *)a0 {
 NBCommandBuffer *cb=self.commandBuffer;
 if(indexed||type!=MTLPrimitiveTypeTriangle||count<6||count>48||count%3||!vertices||!a0||a0.format!=MTLVertexFormatFloat4)return;
 if(cb.traceFrameReplays.count>=8)return;
 id att0=self.traceAttachments.count?self.traceAttachments[0]:nil;
 if(![att0 isKindOfClass:[NBTexture class]])return;
 NBTexture *t0=att0;
 if(t0.pixelFormat!=MTLPixelFormatBGRA8Unorm||t0.width<1000||t0.textureType!=MTLTextureType2D||t0.sampleCount!=1)return;
 NSDictionary *me=self.traceBuffers[@(32+2)];NBBuffer *mb=me[@"buffer"];NSData *md=me[@"bytes"];
 const uint8_t *mbytes=mb?(mb.storageMode==MTLStorageModeShared?(const uint8_t *)mb.contents:nullptr):(const uint8_t *)md.bytes;
 const uint64_t mbase=[me[@"offset"] unsignedLongLongValue],mlen=mb?mb.length:md.length;
 if(!mbytes||mbase>mlen||mlen-mbase<64)return;
 float M[16];memcpy(M,mbytes+mbase,64);
 const double vx=self.traceViewportSet?self.traceViewport.originX:0,vy=self.traceViewportSet?self.traceViewport.originY:0;
 const double vw=self.traceViewportSet?self.traceViewport.width:t0.width,vh=self.traceViewportSet?self.traceViewport.height:t0.height;
 NSMutableArray *pts=[NSMutableArray array];double x0=1e9,y0=1e9,x1=-1e9,y1=-1e9;
 for(NSUInteger i=0;i<count;i++) {
  float p[4];memcpy(p,vertices+(uint64_t)(start+i)*stride+a0.offset,16);
  double c[4];for(unsigned r=0;r<4;r++)c[r]=M[r]*p[0]+M[4+r]*p[1]+M[8+r]*p[2]+M[12+r]*p[3];
  if(c[3]<=1e-6)return;
  const double px=(c[0]/c[3]+1)/2*vw+vx,py=(1-c[1]/c[3])/2*vh+vy;
  [pts addObject:@[@(px),@(py)]];x0=MIN(x0,px);y0=MIN(y0,py);x1=MAX(x1,px);y1=MAX(y1,py);
 }
 if(x1-x0<150||y1-y0<150)return;
 const int64_t bx=MAX((int64_t)floor(x0)-2,0ll),by=MAX((int64_t)floor(y0)-2,0ll);
 const int64_t ex=MIN((int64_t)ceil(x1)+2,(int64_t)t0.width),ey=MIN((int64_t)ceil(y1)+2,(int64_t)t0.height);
 if(ex<=bx||ey<=by)return;
 if(!cb.traceFrameReplays)cb.traceFrameReplays=[NSMutableArray array];
 MTLViewport v=self.traceViewportSet?self.traceViewport:(MTLViewport){0,0,(double)t0.width,(double)t0.height,0,1};
 id att1=self.traceAttachments.count>1?self.traceAttachments[1]:nil;
 [cb.traceFrameReplays addObject:@{@"n":@(n),@"f":@(cb.traceFrame),@"pipeline":self.tracePipeline,@"start":@(start),@"count":@(count),
  @"buffers":[self.traceBuffers copy]?:@{},@"textures":[self.traceTextures copy]?:@{},@"samplers":[self.traceSamplers copy]?:@{},
  @"viewport":[NSValue valueWithBytes:&v objCType:@encode(MTLViewport)],@"att0":t0,@"att1":att1?:[NSNull null],
  @"cull":objc_getAssociatedObject(self,"cull")?:@0,@"winding":objc_getAssociatedObject(self,"winding")?:@0,
  @"points":pts,@"box":@[@(bx),@(by),@(ex-bx),@(ey-by)]}];
 NSLog(@"X9 FRAME REPLAY PICK: f=%u n=%u count=%llu box=%lld,%lld,%lldx%lld",cb.traceFrame,n,(unsigned long long)count,bx,by,ex-bx,ey-by);
}
- (void)traceFrameDraw:(MTLPrimitiveType)type start:(NSUInteger)start count:(NSUInteger)count indices:(NBBuffer *)indices offset:(NSUInteger)offset indexType:(MTLIndexType)indexType baseVertex:(NSInteger)baseVertex instances:(NSUInteger)instances {
 NBCommandBuffer *cb=self.commandBuffer;NBRenderPipeline *pipeline=self.tracePipeline;
 if(!cb.traceFrame||cb.traceFrameReplaying||!pipeline||cb.traceFrameDraws>=400)return;
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
 NSLog(@"X9 FRAME FUNCS: f=%u n=%u vfid=%llu ffid=%llu",f,n,(unsigned long long)pipeline.traceVertexID,(unsigned long long)pipeline.traceFragmentID);
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
 [self traceFrameSelectReplay:type start:start count:count indexed:indices!=nil n:n vertices:vok?vb+vbase:nullptr stride:stride a0:hasA0?a0:nil];
 // GPU-visible copy: 219 showed it matches the CPU for every real vertex
 // fetch, so it is off now to keep the capture from adding GPU work.
 if(true||cb.traceFrameBytes>=4*1024*1024)return;
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
