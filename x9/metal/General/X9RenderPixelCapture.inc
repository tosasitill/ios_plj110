// Diagnostic only: snapshot stored color0 at existing encoder boundaries, on
// three blur-containing command buffers at least two seconds apart. The copies
// add synchronization/work; an improvement on this build is not a repair claim.
// No draw, shader, attachment store action or handler ordering is substituted.
- (void)tracePixelSelect:(bool)blur {
 if(!blur||self.commandBuffer.tracePixelWindow)return;
 static std::mutex mutex;static unsigned windows;static uint64_t last,lastCB;
 const uint64_t now=nowUs();
 {
  std::lock_guard<std::mutex> lock(mutex);
  if(windows>=3||(last&&(now-last<2000000||self.commandBuffer.traceID<lastCB+12)))return;
  self.commandBuffer.tracePixelWindow=++windows;last=now;lastCB=self.commandBuffer.traceID;
 }
 // This source is copied AFTER this render pass, before later encoders can
 // reuse it. It is not a pre-draw snapshot if the pass also writes that image.
 NBTexture *source=self.traceTextures[@3];
 if(source && source!=self.tracePixelOutput[@"texture"])self.tracePixelSource=source;
 NBLOG(@"X9 PIXEL WINDOW: pid=%u window=%u cb=%llu",(unsigned)getpid(),self.commandBuffer.tracePixelWindow,(unsigned long long)self.commandBuffer.traceID);
}
- (void)tracePixelCopy:(NBTexture *)texture role:(NSString *)role {
 NBCommandBuffer *cb=self.commandBuffer;
 if(!texture||cb.encodeFailure||!cb.tracePixelWindow||cb.tracePixelCount>=24)return;
 const uint64_t w=texture.width,h=texture.height;
 // Raw BGRA8 bytes only. Other formats, MSAA, views and discarded attachments
 // need different interpretation and are never coerced into this format.
 if(texture.pixelFormat!=MTLPixelFormatBGRA8Unorm||texture.textureType!=MTLTextureType2D||
    texture.sampleCount!=1||texture.parent||texture.storageMode==MTLStorageModeMemoryless||
    !w||!h||w>4096||h>4096) {
  NBLOG(@"X9 PIXEL SKIP: cb=%llu encoder=%llu tex=%llu role=%@ reason=shape-or-format",(unsigned long long)cb.traceID,(unsigned long long)self.globalTraceObjectID,(unsigned long long)texture.traceID,role);return;
 }
 const uint64_t row=(w*4+255)&~255ull,length=row*h;
 if(length>16*1024*1024||cb.tracePixelBytes+length>64*1024*1024) {
  NBLOG(@"X9 PIXEL SKIP: cb=%llu tex=%llu role=%@ reason=byte-budget",(unsigned long long)cb.traceID,(unsigned long long)texture.traceID,role);return;
 }
 NmbBuffer *raw=nmb_buffer_new(gDevice,length,MTLResourceStorageModeShared,nullptr);
 if(!raw){NBLOG(@"X9 PIXEL SKIP: cb=%llu tex=%llu reason=allocation %@",(unsigned long long)cb.traceID,(unsigned long long)texture.traceID,lastError());return;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=raw;buffer.options=MTLResourceStorageModeShared;
 NmbEncoder *blit=nmb_blit_encoder_new(cb.handle);
 if(!blit){[cb refuse:[@"pixel capture blit: " stringByAppendingString:lastError()]];return;}
 [cb.resourceOwners addObject:buffer];[cb.resourceOwners addObject:texture];
 if(texture.surface)[cb.surfaces addObject:texture];
 const uint64_t origin[3]={0,0,0},size[3]={w,h,1};
 const bool copied=nmb_blit_texture_to_buffer(blit,texture.handle,0,0,origin,size,raw,0,row,length);
 if(!copied) {
  NSString *reason=lastError();nmb_encoder_discard(blit);[cb refuse:[@"pixel capture copy: " stringByAppendingString:reason]];return;
 }
 if(!nmb_encoder_end(blit)){[cb refuse:[@"pixel capture end: " stringByAppendingString:lastError()]];return;}
 cb.tracePixelBytes+=length;
 const unsigned image=++cb.tracePixelCount,window=cb.tracePixelWindow;
 const uint64_t cbID=cb.traceID,encoder=self.globalTraceObjectID,tex=texture.traceID;
 const unsigned first=self.traceGraphFirst,last=self.traceGraphLast;
 NBLOG(@"X9 PIXEL COPY: window=%u cb=%llu image=%u encoder=%llu draws=%u,%u tex=%llu role=%@ size=%llux%llu row=%llu",window,(unsigned long long)cbID,image,(unsigned long long)encoder,first,last,(unsigned long long)tex,role,(unsigned long long)w,(unsigned long long)h,(unsigned long long)row);
 static dispatch_queue_t dumpQueue;static dispatch_once_t once;
 dispatch_once(&once,^{dumpQueue=dispatch_queue_create("org.x9.metal.pixel-dump",DISPATCH_QUEUE_SERIAL);});
 // Run after actual completion; never call getBytes, wait for this CB inside
 // its handler, or dereference the buffer on failed/uncertain GPU ownership.
 [(id<MTLCommandBuffer>)cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
  if(done.status!=MTLCommandBufferStatusCompleted) {
   NBLOG(@"X9 PIXEL FAILED: cb=%llu image=%u status=%llu",(unsigned long long)cbID,image,(unsigned long long)done.status);return;
  }
  // Retaining this private readback buffer on a separate serial queue lets
  // normal completed handlers run without waiting for thousands of log rows.
  dispatch_async(dumpQueue,^{@autoreleasepool {
  const uint8_t *bytes=(const uint8_t *)buffer.contents;
  if(!bytes){NBLOG(@"X9 PIXEL FAILED: cb=%llu image=%u reason=no-cpu-view",(unsigned long long)cbID,image);return;}
  // Keep small intermediates whole (up to 128x128); sample a 32x32 nearest
  // grid of larger images, endpoints included. No filtering or color change.
  const unsigned gw=w<=128&&h<=128?(unsigned)w:32,gh=w<=128&&h<=128?(unsigned)h:32;
  uint32_t checksum=2166136261u;
  for(uint64_t y=0;y<h;y++)for(uint64_t x=0;x<w*4;x++)checksum=(checksum^bytes[y*row+x])*16777619u;
  NBLOG(@"X9 PIXEL DATA: pid=%u window=%u cb=%llu image=%u tex=%llu role=%@ size=%llux%llu grid=%ux%u fnv=%08x order=BGRA8",(unsigned)getpid(),window,(unsigned long long)cbID,image,(unsigned long long)tex,role,(unsigned long long)w,(unsigned long long)h,gw,gh,checksum);
  for(unsigned y=0;y<gh;y++)for(unsigned x=0;x<gw;x+=8) {
   const uint64_t sy=gh>1?(uint64_t)y*(h-1)/(gh-1):0;
   NSMutableString *hex=[NSMutableString string];
   for(unsigned k=x;k<MIN(x+8,gw);k++) {
    const uint64_t sx=gw>1?(uint64_t)k*(w-1)/(gw-1):0;
    const uint8_t *p=bytes+sy*row+sx*4;
    [hex appendFormat:@"%02x%02x%02x%02x",p[0],p[1],p[2],p[3]];
   }
   NBLOG(@"X9 PIXEL ROW: pid=%u cb=%llu image=%u y=%u x=%u hex=%@",(unsigned)getpid(),(unsigned long long)cbID,image,y,x,hex);
  }
  NBLOG(@"X9 PIXEL END: pid=%u cb=%llu image=%u",(unsigned)getpid(),(unsigned long long)cbID,image);
  }});
 }];
}
- (void)endEncoding {
 [super endEncoding];
 if(self.commandBuffer.encodeFailure||!self.commandBuffer.tracePixelWindow)return;
 [self tracePixelCopy:self.tracePixelSource role:@"source-after-pass"];
 NSDictionary *out=self.tracePixelOutput;
 if(!out)return;
 if([out[@"store"] unsignedLongLongValue]!=MTLStoreActionStore||
    [out[@"level"] unsignedLongLongValue]||[out[@"slice"] unsignedLongLongValue]||[out[@"depth"] unsignedLongLongValue]) {
  NBLOG(@"X9 PIXEL SKIP: cb=%llu encoder=%llu reason=color0-discard-or-subresource",(unsigned long long)self.commandBuffer.traceID,(unsigned long long)self.globalTraceObjectID);return;
 }
 [self tracePixelCopy:out[@"texture"] role:@"stored-color0"];
}
