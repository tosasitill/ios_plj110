// A bounded CPU-side render graph after the first actual blur draw. Pixel
// captures, when selected, are separately identified. Texture IDs live for an object's lifetime;
// parent and surface IDs expose aliases without logging process pointers.
- (void)texture:(id<MTLTexture>)texture index:(NSUInteger)index stage:(uint32_t)stage {
 if(stage==2 && index<128) {
  if(!self.traceTextures)self.traceTextures=[NSMutableDictionary dictionary];
  if(texture && [(id)texture isKindOfClass:[NBTexture class]])self.traceTextures[@(index)]=texture;
  else [self.traceTextures removeObjectForKey:@(index)];
 }
 [super texture:texture index:index stage:stage];
}
- (void)traceGraph:(MTLPrimitiveType)type count:(NSUInteger)count {
 NBRenderPipeline *p=self.tracePipeline;if(!p)return;
 bool blur=[p.traceVertex containsString:@"blur"]||[p.traceFragment containsString:@"blur"];
 [self tracePixelSelect:blur];
 static std::mutex mutex;static bool started;static unsigned serial;
 static NSMutableDictionary *seen;unsigned draw;
 {
  std::lock_guard<std::mutex> lock(mutex);
  if(blur)started=true;
  if(!started||serial>=256)return;
  if(!seen)seen=[NSMutableDictionary dictionary];
  NSString *key=[NSString stringWithFormat:@"%@/%@",p.traceVertex,p.traceFragment];
  unsigned n=[seen[key] unsignedIntValue];if(n>=6&&!self.commandBuffer.tracePixelWindow)return;
  seen[key]=@(n+1);draw=++serial;
 }
 self.commandBuffer.traceGraphUsed=YES;
 if(!self.traceGraphFirst)self.traceGraphFirst=draw;self.traceGraphLast=draw;
 NBLOG(@"X9 GRAPH DRAW: id=%u cb=%llu encoder=%llu vs=%@ fs=%@ type=%llu count=%llu",draw,(unsigned long long)self.commandBuffer.traceID,(unsigned long long)self.globalTraceObjectID,p.traceVertex,p.traceFragment,(unsigned long long)type,(unsigned long long)count);
 // Split text at tokens rather than losing the last fields to USB line limits.
 for(NSString *part in [self.tracePass componentsSeparatedByString:@" "])
  if(part.length)NBLOG(@"X9 GRAPH PASS: id=%u %@",draw,part);
 for(NSString *part in [p.traceBlend componentsSeparatedByString:@" "])
  if(part.length)NBLOG(@"X9 GRAPH BLEND: id=%u %@",draw,part);
 NSDictionary *samplers=objc_getAssociatedObject(self,&graphSamplersKey);
 for(NSNumber *slot in [[samplers allKeys] sortedArrayUsingSelector:@selector(compare:)])
  NBLOG(@"X9 GRAPH SAMPLER: id=%u slot=%u %@",draw,slot.unsignedIntValue,samplers[slot]);
 for(NSNumber *slot in [[self.traceTextures allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
  NBTexture *t=self.traceTextures[slot];
  uint64_t parent=[(id)t.parent isKindOfClass:[NBTexture class]]?[(NBTexture *)t.parent traceID]:0;
  NBLOG(@"X9 GRAPH TEX: id=%u slot=%u tex=%llu parent=%llu surface=%u format=%llu size=%llux%llu levels=%llu samples=%llu",draw,slot.unsignedIntValue,(unsigned long long)t.traceID,(unsigned long long)parent,t.surface?IOSurfaceGetID(t.surface):0,(unsigned long long)t.pixelFormat,(unsigned long long)t.width,(unsigned long long)t.height,(unsigned long long)t.mipmapLevelCount,(unsigned long long)t.sampleCount);
  NBLOG(@"X9 GRAPH TEXMEM: id=%u slot=%u offset=%llu row=%llu plane=%llu usage=%llu options=%llu swizzle=%llu,%llu,%llu,%llu",draw,slot.unsignedIntValue,(unsigned long long)t.offset,(unsigned long long)t.rowBytes,(unsigned long long)t.surfacePlane,(unsigned long long)t.descriptor.usage,(unsigned long long)t.descriptor.options,(unsigned long long)t.descriptor.swizzle[0],(unsigned long long)t.descriptor.swizzle[1],(unsigned long long)t.descriptor.swizzle[2],(unsigned long long)t.descriptor.swizzle[3]);
 }
}
