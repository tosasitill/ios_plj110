// x9vf whole-frame geometry capture (backboardd only, opt-in).
// At three uptime triggers, every draw of the next three command buffers is
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
 if(!remaining && trigger<3 && now>=start+(uint64_t)trigger*20000000ull){trigger++;remaining=3;}
 if(!remaining)return;
 remaining--;cb.traceFrame=++frames;cb.traceFrameTrigger=trigger;
 NSLog(@"X9 FRAME CB: f=%u trigger=%u cb=%llu uptime_us=%llu",cb.traceFrame,trigger,(unsigned long long)cb.traceID,(unsigned long long)now);
}
// Bytes a replay binding can reach, from its offset: enough for the draw's
// vertices (stride*(start+count)) and at least 4 KiB of uniforms, clipped to
// the buffer. Null when the buffer is not CPU readable.
static NSData *x9ReplayBindingBytes(NSDictionary *entry,uint64_t want) {
 NBBuffer *b=entry[@"buffer"];NSData *d=entry[@"bytes"];
 if(!b)return d.length?[d copy]:nil;
 if(b.storageMode!=MTLStorageModeShared||!b.contents)return nil;
 const uint64_t off=[entry[@"offset"] unsignedLongLongValue];if(off>=b.length)return nil;
 const uint64_t n=MIN(b.length-off,MAX(want,(uint64_t)4096));
 return [NSData dataWithBytes:(const uint8_t *)b.contents+off length:(NSUInteger)n];
}
static uint64_t x9ReplayHash(NSData *d) {
 uint64_t h=1469598103934665603ull;const uint8_t *p=(const uint8_t *)d.bytes;
 for(NSUInteger i=0;i<d.length;i++){h^=p[i];h*=1099511628211ull;}
 return h;
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
  total+=((bw*4+255)&~255ull)*bh*14;
 }
 if(total>192ull*1024*1024){NSLog(@"X9 FRAME REPLAY SKIP: f=%llu bytes=%llu reason=budget",(unsigned long long)cb.traceID,(unsigned long long)total);cb.traceFrameReplaying=NO;return;}
 id<MTLBuffer> readback=[device newBufferWithLength:total options:MTLResourceStorageModeShared];
 if(!readback){NSLog(@"X9 FRAME REPLAY SKIP: f=%llu reason=readback",(unsigned long long)cb.traceID);cb.traceFrameReplaying=NO;return;}
 // Variant 10 binds this in place of every fragment texture: 256x256 BGRA8,
 // R=x G=y B=0x40, so each written pixel encodes the interpolated texcoord
 // the shader sampled with (fetch -> varying -> raster, end to end).
 static id<MTLTexture> gradient;
 if(!gradient) {
  MTLTextureDescriptor *gd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256 mipmapped:NO];
  gd.usage=MTLTextureUsageShaderRead;gd.storageMode=MTLStorageModeShared;
  gradient=[device newTextureWithDescriptor:gd];
  if(gradient) {
   NSMutableData *g=[NSMutableData dataWithLength:256*256*4];uint8_t *q=(uint8_t *)g.mutableBytes;
   for(unsigned y=0;y<256;y++)for(unsigned x=0;x<256;x++){uint8_t *o=q+(y*256+x)*4;o[0]=0x40;o[1]=(uint8_t)y;o[2]=(uint8_t)x;o[3]=0xff;}
   [gradient replaceRegion:MTLRegionMake2D(0,0,256,256) mipmapLevel:0 withBytes:q bytesPerRow:256*4];
  }
 }
 uint64_t at=0;
 for(NSDictionary *r in replays) {
  NBTexture *a0=r[@"att0"];id a1=r[@"att1"];
  NSArray *box=r[@"box"];const uint64_t bx=[box[0] unsignedLongLongValue],by=[box[1] unsignedLongLongValue],bw=[box[2] unsignedLongLongValue],bh=[box[3] unsignedLongLongValue];
  const uint64_t row=(bw*4+255)&~255ull;const NSUInteger start=[r[@"start"] unsignedIntegerValue],count=[r[@"count"] unsignedIntegerValue];
  MTLViewport v;[r[@"viewport"] getValue:&v];
  // 0 whole, 1 last alone, 2 without first two, 3/4 last alone with its
  // vertex order rotated (different provoking vertex), 5 first alone.
  const NSUInteger ranges[6][2]={{start,count},{start+count-3,3},{start+6,count-6},{0,3},{0,3},{start,3}};
  const uint16_t rotated[2][3]={{(uint16_t)(start+count-2),(uint16_t)(start+count-1),(uint16_t)(start+count-3)},
                                {(uint16_t)(start+count-1),(uint16_t)(start+count-3),(uint16_t)(start+count-2)}};
  id<MTLBuffer> rotation=[device newBufferWithBytes:rotated length:sizeof(rotated) options:MTLResourceStorageModeShared];
  // 6..9 draw from a buffer this capture owns, bound at offset 0: 6 the last
  // triangle moved to vertices 0..2, 7 the whole draw copied, 8/9 the last
  // triangle mirrored horizontally/vertically in object space.
  NSData *raw=r[@"raw"];const uint64_t vstride=[r[@"stride"] unsignedLongLongValue],posoff=[r[@"posoff"] unsignedLongLongValue];
  const unsigned vslot=[r[@"slot"] unsignedIntValue];
  NSArray *allPts=r[@"points"];
  id<MTLBuffer> owned[4]={nil,nil,nil,nil};NSArray *ownedPts[4]={nil,nil,nil,nil};
  if(raw.length==count*vstride&&vstride>=posoff+8) {
   const uint8_t *src=(const uint8_t *)raw.bytes;const uint8_t *last=src+(count-3)*vstride;
   owned[0]=[device newBufferWithBytes:last length:3*vstride options:MTLResourceStorageModeShared];
   ownedPts[0]=[allPts subarrayWithRange:NSMakeRange(count-3,3)];
   owned[1]=[device newBufferWithBytes:src length:count*vstride options:MTLResourceStorageModeShared];
   ownedPts[1]=allPts;
   for(unsigned axis=0;axis<2;axis++) {
    NSMutableData *m=[NSMutableData dataWithBytes:last length:3*vstride];uint8_t *d=(uint8_t *)m.mutableBytes;
    float lo=INFINITY,hi=-INFINITY;double slo=INFINITY,shi=-INFINITY;
    for(unsigned k=0;k<3;k++){float v;memcpy(&v,d+k*vstride+posoff+axis*4,4);lo=MIN(lo,v);hi=MAX(hi,v);
     const double sv=[ownedPts[0][k][axis] doubleValue];slo=MIN(slo,sv);shi=MAX(shi,sv);}
    NSMutableArray *mp=[NSMutableArray array];
    for(unsigned k=0;k<3;k++) {
     float v;memcpy(&v,d+k*vstride+posoff+axis*4,4);v=lo+hi-v;memcpy(d+k*vstride+posoff+axis*4,&v,4);
     double x=[ownedPts[0][k][0] doubleValue],y=[ownedPts[0][k][1] doubleValue];
     if(axis==0)x=slo+shi-x;else y=slo+shi-y;
     [mp addObject:@[@(x),@(y)]];
    }
    owned[2+axis]=[device newBufferWithBytes:d length:3*vstride options:MTLResourceStorageModeShared];ownedPts[2+axis]=mp;
   }
  }
  // Every pass gets its own box-sized targets (viewport shifted by the box
  // origin), so no pass's readback can race a later pass's clear; variants
  // 10 is the whole draw with the UV-gradient texture in every slot; 11 is
  // the whole draw with every buffer binding replaced by the CPU snapshot
  // taken at pick time (inputs frozen). 0 and 11 alternate, 4 repeats each,
  // so live-input mutation and GPU non-determinism can be told apart.
  NSMutableDictionary *frozen=[NSMutableDictionary dictionary];
  NSDictionary *snap=r[@"snap"];
  for(NSNumber *key in snap){NSData *d=snap[key];id<MTLBuffer> fb=[device newBufferWithBytes:d.bytes length:d.length options:MTLResourceStorageModeShared];if(fb)frozen[key]=fb;}
  // 12 = whole draw, live inputs, colour attachment 1 cleared to (1,1,1,1)
  // instead of 0: a different result means the shader reads attachment 1.
  static const uint8_t schedule[][2]={{0,0},{11,0},{12,0},{0,1},{11,1},{12,1},{0,2},{11,2},{12,2},{0,3},{0,4},{0,5},{10,0},{1,0}};
  for(unsigned pass=0;pass<sizeof schedule/sizeof schedule[0];pass++) {
   const unsigned variant=schedule[pass][0],repeat=schedule[pass][1];
   if(variant==11&&!frozen.count)continue;
   if(variant==10&&!gradient)continue;
   if(variant>=6&&variant<10&&!owned[variant-6])continue;
   if(variant<6&&!ranges[variant][1])continue;
   MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:a0.pixelFormat width:bw height:bh mipmapped:NO];
   td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;td.storageMode=MTLStorageModePrivate;
   id<MTLTexture> c0=[device newTextureWithDescriptor:td],c1=nil;
   if([a1 isKindOfClass:[NBTexture class]]) {
    MTLTextureDescriptor *t1=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:[(NBTexture *)a1 pixelFormat] width:bw height:bh mipmapped:NO];
    t1.usage=MTLTextureUsageRenderTarget;t1.storageMode=MTLStorageModePrivate;c1=[device newTextureWithDescriptor:t1];
   }
   if(!c0){NSLog(@"X9 FRAME REPLAY SKIP: n=%@ reason=texture",r[@"n"]);break;}
   MTLViewport shifted=v;shifted.originX-=(double)bx;shifted.originY-=(double)by;
   MTLRenderPassDescriptor *pd=[MTLRenderPassDescriptor renderPassDescriptor];
   pd.colorAttachments[0].texture=c0;pd.colorAttachments[0].loadAction=MTLLoadActionClear;
   pd.colorAttachments[0].clearColor=MTLClearColorMake(1,0,1,1);pd.colorAttachments[0].storeAction=MTLStoreActionStore;
   if(c1){pd.colorAttachments[1].texture=c1;pd.colorAttachments[1].loadAction=MTLLoadActionClear;
    pd.colorAttachments[1].clearColor=variant==12?MTLClearColorMake(1,1,1,1):MTLClearColorMake(0,0,0,0);pd.colorAttachments[1].storeAction=MTLStoreActionDontCare;}
   id<MTLRenderCommandEncoder> e=[(id<MTLCommandBuffer>)cb renderCommandEncoderWithDescriptor:pd];
   if(!e){NSLog(@"X9 FRAME REPLAY SKIP: n=%@ reason=encoder",r[@"n"]);break;}
   [e setRenderPipelineState:r[@"pipeline"]];[e setViewport:shifted];
   [e setCullMode:(MTLCullMode)[r[@"cull"] unsignedIntegerValue]];[e setFrontFacingWinding:(MTLWinding)[r[@"winding"] unsignedIntegerValue]];
   NSDictionary *buffers=r[@"buffers"];
   for(NSNumber *key in buffers) {
    const unsigned stage=key.unsignedIntValue/32,slot=key.unsignedIntValue%32;
    NSDictionary *entry=buffers[key];NBBuffer *b=entry[@"buffer"];NSData *bytes=entry[@"bytes"];
    const NSUInteger off=[entry[@"offset"] unsignedIntegerValue];
    id<MTLBuffer> fz=variant==11?frozen[key]:nil;
    if(fz){if(stage==1)[e setVertexBuffer:fz offset:0 atIndex:slot];else if(stage==2)[e setFragmentBuffer:fz offset:0 atIndex:slot];continue;}
    if(stage==1){if(b)[e setVertexBuffer:b offset:off atIndex:slot];else if(bytes.length)[e setVertexBytes:bytes.bytes length:bytes.length atIndex:slot];}
    else if(stage==2){if(b)[e setFragmentBuffer:b offset:off atIndex:slot];else if(bytes.length)[e setFragmentBytes:bytes.bytes length:bytes.length atIndex:slot];}
   }
   NSDictionary *textures=r[@"textures"];for(NSNumber *key in textures)[e setFragmentTexture:variant==10?gradient:textures[key] atIndex:key.unsignedIntegerValue];
   NSDictionary *samplers=r[@"samplers"];for(NSNumber *key in samplers)[e setFragmentSamplerState:samplers[key] atIndex:key.unsignedIntegerValue];
   if(variant>=10)[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:start vertexCount:count];
   else if(variant>=6) {
    [e setVertexBuffer:owned[variant-6] offset:0 atIndex:vslot];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:variant==7?count:3];
   } else if(variant==3||variant==4)
    [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt16 indexBuffer:rotation indexBufferOffset:(variant-3)*6];
   else [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:ranges[variant][0] vertexCount:ranges[variant][1]];
   [e endEncoding];
   id<MTLBlitCommandEncoder> blit=[(id<MTLCommandBuffer>)cb blitCommandEncoder];
   [blit copyFromTexture:c0 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(bw,bh,1)
                toBuffer:readback destinationOffset:at destinationBytesPerRow:row destinationBytesPerImage:row*bh];
   [blit endEncoding];
   const bool whole=variant>=10;
   const NSUInteger rel=whole?0:variant>=6?0:variant==3||variant==4?count-3:ranges[variant][0]-start;
   const NSUInteger vcount=whole?count:variant>=6?(variant==7?count:3):ranges[variant][1];
   [results addObject:@{@"r":r,@"variant":@(variant),@"repeat":@(repeat),@"at":@(at),@"row":@(row),@"first":@(rel),@"count":@(vcount),
    @"pts":whole?allPts:variant>=6?ownedPts[variant-6]:allPts}];
   at+=row*bh;
  }
 }
 cb.traceFrameReplaying=NO;
 const unsigned f=(unsigned)cb.traceID;
 NSLog(@"X9 FRAME REPLAY ENCODED: f=%u draws=%llu passes=%llu",f,(unsigned long long)replays.count,(unsigned long long)results.count);
 [(id<MTLCommandBuffer>)cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
  if(done.status!=MTLCommandBufferStatusCompleted){NSLog(@"X9 FRAME REPLAY FAILED: f=%u status=%llu",f,(unsigned long long)done.status);return;}
  const uint8_t *bytes=(const uint8_t *)readback.contents;if(!bytes)return;
  for(NSDictionary *res in results) {
   NSDictionary *r=res[@"r"];NSArray *pts=res[@"pts"];NSArray *box=r[@"box"];
   const double bx=[box[0] doubleValue],by=[box[1] doubleValue];const int64_t bw=[box[2] longLongValue],bh=[box[3] longLongValue];
   const uint64_t base=[res[@"at"] unsignedLongLongValue],row=[res[@"row"] unsignedLongLongValue];
   const unsigned first=[res[@"first"] unsignedIntValue],cnt=[res[@"count"] unsignedIntValue];
   uint64_t magenta=0,sampled=0;
   for(int64_t y=0;y<bh;y+=4)for(int64_t x=0;x<bw;x+=4){const uint8_t *q=bytes+base+y*row+x*4;sampled++;if(q[0]==255&&q[1]==0&&q[2]==255&&q[3]==255)magenta++;}
   uint64_t boxHash=1469598103934665603ull;
   for(int64_t y=0;y<bh;y++)for(int64_t x=0;x<bw*4;x++){boxHash^=bytes[base+y*row+x];boxHash*=1099511628211ull;}
   NSLog(@"X9 FRAME REPLAY RESULT: f=%u n=%@ variant=%@ repeat=%@ first=%u count=%u box_uncovered=%llu/%llu hash=%016llx",f,r[@"n"],res[@"variant"],res[@"repeat"],first,cnt,(unsigned long long)magenta,(unsigned long long)sampled,(unsigned long long)boxHash);
   // 16x16 pixel grid over the box (cell centres), one row per line, for
   // the whole draw with real (0) and gradient (10) textures. Box-relative.
   const unsigned variantN=[res[@"variant"] unsignedIntValue];
   // 32x32 coverage map of every pass: '.' clear colour kept, '#' written.
   for(unsigned my=0;my<32;my++) {
    char line[33];const int64_t iy=(int64_t)((my+0.5)*bh/32);
    for(unsigned mx=0;mx<32;mx++){const int64_t ix=(int64_t)((mx+0.5)*bw/32);const uint8_t *q=bytes+base+iy*row+ix*4;
     line[mx]=(q[0]==255&&q[1]==0&&q[2]==255&&q[3]==255)?'.':'#';}
    line[32]=0;if(!strchr(line,'#'))continue;
    NSLog(@"X9 FRAME REPLAY MAP: f=%u n=%@ v=%u r=%@ y=%02u %s",f,r[@"n"],variantN,res[@"repeat"],my,line);
   }
   if((variantN==0||variantN==10)&&[res[@"repeat"] unsignedIntValue]==0)
    for(unsigned gy=0;gy<16;gy++) {
     NSMutableString *line=[NSMutableString string];const int64_t iy=(int64_t)((gy+0.5)*bh/16);
     for(unsigned gx=0;gx<16;gx++){const int64_t ix=(int64_t)((gx+0.5)*bw/16);const uint8_t *q=bytes+base+iy*row+ix*4;
      [line appendFormat:@" %02x%02x%02x%02x",q[0],q[1],q[2],q[3]];}
     NSLog(@"X9 FRAME REPLAY GRID: f=%u n=%@ v=%u y=%lld%@",f,r[@"n"],variantN,iy,line);
    }
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
    NSLog(@"X9 FRAME REPLAY TRI: f=%u n=%@ variant=%@ repeat=%@ tri=%u area=%.1f p=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) covered=%u/%u bgra=%@",
     f,r[@"n"],res[@"variant"],res[@"repeat"],t/3,area,P[0][0],P[0][1],P[1][0],P[1][1],P[2][0],P[2][1],covered,tested,px);
   }
  }
  // Did any live binding change between pick (encode) and GPU completion?
  for(NSDictionary *r in replays) {
   NSDictionary *snap=r[@"snap"],*buffers=r[@"buffers"];
   for(NSNumber *key in snap) {
    // Only bytes this draw can read: the vertex range for the position slot,
    // the first 256 bytes for every other binding (later draws fill the ring
    // after the pick, so a whole 4 KiB window always differs).
    const bool vtx=key.unsignedIntValue==32+[r[@"slot"] unsignedIntValue];
    const NSUInteger want=MIN([snap[key] length],vtx?(NSUInteger)([r[@"start"] unsignedLongLongValue]+[r[@"count"] unsignedLongLongValue])*[r[@"stride"] unsignedLongLongValue]:(NSUInteger)256);
    NSData *now=x9ReplayBindingBytes(buffers[key],want),*was=[snap[key] subdataWithRange:NSMakeRange(0,want)];
    if(now.length>want)now=[now subdataWithRange:NSMakeRange(0,want)];
    NSLog(@"X9 FRAME REPLAY LIVE: f=%u n=%@ stage=%u slot=%u len=%llu pick=%016llx done=%016llx same=%u",f,r[@"n"],key.unsignedIntValue/32,key.unsignedIntValue%32,
     (unsigned long long)want,(unsigned long long)x9ReplayHash(was),now?(unsigned long long)x9ReplayHash(now):0ull,now&&[now isEqualToData:was]);
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
// Any backboardd command buffer after the start time can contribute replay
// candidates (process budget 8), so the capture no longer depends on the
// user having Control Center open at a fixed uptime.
- (void)traceReplayCandidate:(MTLPrimitiveType)type start:(NSUInteger)start count:(NSUInteger)count indexed:(bool)indexed {
 NBCommandBuffer *cb=self.commandBuffer;NBRenderPipeline *pipeline=self.tracePipeline;
 if(cb.traceFrameReplaying||!pipeline||indexed||type!=MTLPrimitiveTypeTriangle||count<6||count>48||count%3)return;
 static const bool server=!strcmp(getprogname(),"backboardd");
 if(!server)return;
 const X9CaptureOptions &options=captureOptions();
 if(nowUs()<(options.startGiven?options.geometryStartUs:45000000ull))return;
 static std::atomic<unsigned> budget{0};static std::atomic<uint64_t> firstFragment{0};
 if(budget.load()>=2||cb.traceFrameReplays.count>=2)return;
 // The second pick must use another fragment function (rev236 spent both on ffid66).
 if(budget.load()==1&&pipeline.traceFragmentID==firstFragment.load())return;
 MTLVertexAttributeDescriptor *a0=pipeline.traceLayout.attributes[0];
 if(!a0||a0.format!=MTLVertexFormatFloat4)return;
 MTLVertexBufferLayoutDescriptor *vl=pipeline.traceLayout.layouts[a0.bufferIndex];
 NSDictionary *ventry=self.traceBuffers[@(32+a0.bufferIndex)];NBBuffer *vbuf=ventry[@"buffer"];NSData *vdata=ventry[@"bytes"];
 const uint8_t *vb=vbuf?(vbuf.storageMode==MTLStorageModeShared?(const uint8_t *)vbuf.contents:nullptr):(const uint8_t *)vdata.bytes;
 const uint64_t vbase=[ventry[@"offset"] unsignedLongLongValue],vlength=vbuf?vbuf.length:vdata.length,stride=vl?vl.stride:0;
 if(!vb||!stride||vl.stepFunction!=MTLVertexStepFunctionPerVertex||vbase>vlength||(uint64_t)(start+count)*stride>vlength-vbase)return;
 const NSUInteger before=cb.traceFrameReplays.count;
 [self traceFrameSelectReplay:type start:start count:count indexed:false n:(unsigned)(budget.load()+1) vertices:vb+vbase stride:stride a0:a0];
 if(cb.traceFrameReplays.count>before){if(!budget.load())firstFragment=pipeline.traceFragmentID;budget++;}
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
  // 2D platter quads only (perspective CC transitions render both halves).
  if(c[3]<0.99||c[3]>1.01)return;
  const double px=(c[0]/c[3]+1)/2*vw+vx,py=(1-c[1]/c[3])/2*vh+vy;
  [pts addObject:@[@(px),@(py)]];x0=MIN(x0,px);y0=MIN(y0,py);x1=MAX(x1,px);y1=MAX(y1,py);
 }
 // Whole draw on screen and at least 200x200: CC module cards (~511x511),
 // not edge strips whose vertices run off the target (rev234 burned both
 // picks on a 105x834 clipped strip at x=1151 before CC was opened).
 if(x1-x0<200||y1-y0<200)return;
 if(x0<-0.5||y0<-0.5||x1>t0.width+0.5||y1>t0.height+0.5)return;
 const int64_t bx=MAX((int64_t)floor(x0)-2,0ll),by=MAX((int64_t)floor(y0)-2,0ll);
 const int64_t ex=MIN((int64_t)ceil(x1)+2,(int64_t)t0.width),ey=MIN((int64_t)ceil(y1)+2,(int64_t)t0.height);
 if(ex<=bx||ey<=by)return;
 // Freeze every vertex/fragment buffer binding as the CPU sees it now.
 NSMutableDictionary *snap=[NSMutableDictionary dictionary];
 for(NSNumber *key in self.traceBuffers) {
  NSDictionary *entry=self.traceBuffers[key];const unsigned stage=key.unsignedIntValue/32;
  if(stage!=1&&stage!=2)continue;
  NSData *d=x9ReplayBindingBytes(entry,(uint64_t)(start+count)*stride);
  NBBuffer *b=entry[@"buffer"];
  NSLog(@"X9 FRAME REPLAY BIND: f=%llu n=%u stage=%u slot=%u kind=%s obj=%llu offset=%llu len=%llu hash=%016llx",(unsigned long long)cb.traceID,n,stage,key.unsignedIntValue%32,
   b?"buffer":"bytes",(unsigned long long)(b?x9FrameObjectID(b):0),(unsigned long long)[entry[@"offset"] unsignedLongLongValue],(unsigned long long)d.length,d?(unsigned long long)x9ReplayHash(d):0ull);
  if(d)snap[key]=d;
 }
 if(!cb.traceFrameReplays)cb.traceFrameReplays=[NSMutableArray array];
 MTLViewport v=self.traceViewportSet?self.traceViewport:(MTLViewport){0,0,(double)t0.width,(double)t0.height,0,1};
 id att1=self.traceAttachments.count>1?self.traceAttachments[1]:nil;
 [cb.traceFrameReplays addObject:@{@"n":@(n),@"f":@(cb.traceID),@"pipeline":self.tracePipeline,@"start":@(start),@"count":@(count),
  @"buffers":[self.traceBuffers copy]?:@{},@"textures":[self.traceTextures copy]?:@{},@"samplers":[self.traceSamplers copy]?:@{},
  @"viewport":[NSValue valueWithBytes:&v objCType:@encode(MTLViewport)],@"att0":t0,@"att1":att1?:[NSNull null],
  @"cull":objc_getAssociatedObject(self,"cull")?:@0,@"winding":objc_getAssociatedObject(self,"winding")?:@0,
  @"points":pts,@"box":@[@(bx),@(by),@(ex-bx),@(ey-by)],
  @"raw":[NSData dataWithBytes:vertices+(uint64_t)start*stride length:(NSUInteger)(count*stride)],
  @"stride":@(stride),@"slot":@(a0.bufferIndex),@"posoff":@(a0.offset),@"snap":snap}];
 NSLog(@"X9 FRAME REPLAY PICK: f=%llu n=%u count=%llu box=%lld,%lld,%lldx%lld fs=%@ ffid=%llu",(unsigned long long)cb.traceID,n,(unsigned long long)count,bx,by,ex-bx,ey-by,self.tracePipeline.traceFragment,(unsigned long long)self.tracePipeline.traceFragmentID);
 for(NSUInteger i=0;i<count;i++)for(uint64_t r0=0;r0<stride&&r0<64;r0+=24) {
  NSMutableString *hex=[NSMutableString string];
  for(uint64_t b=r0;b<MIN(r0+24,stride);b++)[hex appendFormat:@"%02x",vertices[(uint64_t)(start+i)*stride+b]];
  NSLog(@"X9 FRAME REPLAY VTX: f=%llu n=%u v=%llu off=%llu hex=%@",(unsigned long long)cb.traceID,n,(unsigned long long)i,(unsigned long long)r0,hex);
 }
}
// CPU projection of the captured position attribute through fragment buffer
// slot 2 (the same transform the replay uses) into drawable pixels. Reports a
// bounding box only: no GPU work, no state change. False when a binding is not
// CPU readable so the caller records the gap instead of a guessed rectangle.
static bool x9FrameCoverage(NBRenderEncoder *encoder,NBRenderPipeline *pipeline,
                            const uint8_t *vertices,uint64_t vertexBase,uint64_t vertexLength,
                            uint64_t stride,MTLVertexAttributeDescriptor *a0,
                            const uint32_t *ids,unsigned idCount,double box[4]) {
 if(!encoder||!pipeline||!vertices||!a0||a0.format!=MTLVertexFormatFloat4||!stride)return false;
 NSDictionary *entry=encoder.traceBuffers[@(32+2)];NBBuffer *buffer=entry[@"buffer"];NSData *data=entry[@"bytes"];
 const uint8_t *m=buffer?(buffer.storageMode==MTLStorageModeShared?(const uint8_t *)buffer.contents:nullptr):(const uint8_t *)data.bytes;
 const uint64_t base=[entry[@"offset"] unsignedLongLongValue],length=buffer?buffer.length:data.length;
 if(!m||base>length||length-base<64)return false;
 float M[16];memcpy(M,m+base,64);
 double vx=0,vy=0,vw=0,vh=0;
 if(encoder.traceViewportSet){vx=encoder.traceViewport.originX;vy=encoder.traceViewport.originY;vw=encoder.traceViewport.width;vh=encoder.traceViewport.height;}
 else {
  id att=encoder.traceAttachments.count?encoder.traceAttachments[0]:nil;
  if(![att isKindOfClass:[NBTexture class]])return false;
  vw=[(NBTexture *)att width];vh=[(NBTexture *)att height];
 }
 if(vw<=0||vh<=0||vertexBase>vertexLength)return false;
 double x0=1e30,y0=1e30,x1=-1e30,y1=-1e30;unsigned used=0;
 for(unsigned k=0;k<idCount;k++) {
  const uint64_t at=(uint64_t)ids[k];
  if(at>(vertexLength-vertexBase)/stride)continue;
  if(vertexBase+at*stride+a0.offset+16>vertexLength)continue;
  float p[4];memcpy(p,vertices+vertexBase+at*stride+a0.offset,16);
  double c[4];for(unsigned r=0;r<4;r++)c[r]=M[r]*p[0]+M[4+r]*p[1]+M[8+r]*p[2]+M[12+r]*p[3];
  if(c[3]<=1e-6)continue;
  const double px=(c[0]/c[3]+1)/2*vw+vx,py=(1-c[1]/c[3])/2*vh+vy;
  x0=MIN(x0,px);y0=MIN(y0,py);x1=MAX(x1,px);y1=MAX(y1,py);used++;
 }
 if(!used)return false;
 box[0]=x0;box[1]=y0;box[2]=x1;box[3]=y1;return true;
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
 // Fragment texture slots as bound at this draw. tex is the Metal object,
 // surface is the IOSurface the kernel presents (the same ID space as
 // SWEET FB swap / X9 LAYER), parent names an aliased view.
 for(NSNumber *slot in [[self.traceTextures allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
  NBTexture *t=self.traceTextures[slot];
  const uint64_t parent=[(id)t.parent isKindOfClass:[NBTexture class]]?[(NBTexture *)t.parent traceID]:0;
  NSLog(@"X9 FRAME TEX: f=%u n=%u slot=%u tex=%llu parent=%llu surface=%u format=%llu size=%llux%llu levels=%llu",f,n,slot.unsignedIntValue,
   (unsigned long long)t.traceID,(unsigned long long)parent,t.surface?IOSurfaceGetID(t.surface):0,(unsigned long long)t.pixelFormat,(unsigned long long)t.width,(unsigned long long)t.height,(unsigned long long)t.mipmapLevelCount);
 }
 // Framebuffer fetch / read-back: a colour attachment also bound as a
 // fragment texture is read through tile memory (or a copy back). Load/store
 // and the pipeline colour-write mask state what the tile path may write.
 for(unsigned rt=0;rt<8;rt++) {
  id o=self.traceAttachments.count>rt?self.traceAttachments[rt]:nil;
  if(![o isKindOfClass:[NBTexture class]])continue;
  NBTexture *att=(NBTexture *)o;int bound=-1;
  for(NSNumber *slot in self.traceTextures)if([(NBTexture *)self.traceTextures[slot] traceID]==att.traceID){bound=(int)slot.unsignedIntValue;break;}
  NSDictionary *st=self.traceAttachmentState.count>rt?self.traceAttachmentState[rt]:nil;
  NSArray *masks=pipeline.traceWriteMasks;
  const long long wmask=masks.count>rt?[masks[rt] longLongValue]:-1;
  NSLog(@"X9 FRAME FBF: f=%u n=%u rt=%u tex=%llu surface=%u bound_as_texture=%u slot=%d load=%llu store=%llu wmask=%lld",f,n,rt,
   (unsigned long long)att.traceID,att.surface?IOSurfaceGetID(att.surface):0,bound>=0,bound,
   (unsigned long long)[st[@"load"] unsignedLongLongValue],(unsigned long long)[st[@"store"] unsignedLongLongValue],wmask);
 }
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
 // Per-draw pixel coverage: the projected position bbox, clipped to the
 // explicit scissor and the render target. CPU-only; shows whether a draw can
 // reach the region under investigation and whether a scissor excludes it.
 if(vok) {
  double box[4];
  if(x9FrameCoverage(self,pipeline,vb,vbase,vlength,stride,a0,ids,readable?used:0,box)) {
   id att=self.traceAttachments.count?self.traceAttachments[0]:nil;
   const double areaW=[att isKindOfClass:[NBTexture class]]?[(NBTexture *)att width]:0;
   const double areaH=[att isKindOfClass:[NBTexture class]]?[(NBTexture *)att height]:0;
   const double sx=self.traceScissorSet?self.traceScissor.x:0,sy=self.traceScissorSet?self.traceScissor.y:0;
   const double sw=self.traceScissorSet?self.traceScissor.width:areaW,sh=self.traceScissorSet?self.traceScissor.height:areaH;
   const int64_t ix0=MAX((int64_t)floor(box[0]),(int64_t)sx),iy0=MAX((int64_t)floor(box[1]),(int64_t)sy);
   const int64_t ix1=MIN((int64_t)ceil(box[2]),(int64_t)(sx+sw)),iy1=MIN((int64_t)ceil(box[3]),(int64_t)(sy+sh));
   NSLog(@"X9 FRAME COVER: f=%u n=%u bbox=%.1f,%.1f,%.1fx%.1f scissor=%.1f,%.1f,%.1fx%.1f clipped=%lld,%lld,%lldx%lld area=%.0fx%.0f clipped_nonempty=%u",
    f,n,box[0],box[1],box[2]-box[0],box[3]-box[1],sx,sy,sw,sh,ix0,iy0,ix1-ix0,iy1-iy0,areaW,areaH,ix1>ix0&&iy1>iy0);
  } else NSLog(@"X9 FRAME COVER: f=%u n=%u unavailable=projection-binding",f,n);
 } else NSLog(@"X9 FRAME COVER: f=%u n=%u unavailable=%s",f,n,!readable?"indices":!vb?"vertices":!hasA0?"position-attribute":"vertex-layout");
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
