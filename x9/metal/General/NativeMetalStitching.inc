// SPDX-License-Identifier: MIT
// UniXNU/AdrenoMetal 7183e588: retained library bytes for CoreImage stitching.
// A function's wrapped bitcode in its library's metallib: the function list's entry NAMEd as the function, MDSZ bytes at OFFT's bitcode
// offset into the bitcode section (header: function list at 24, bitcode section offset and size at 72 and 80; tags a fourcc, a u16
// length and the bytes, up to ENDT).
static NSData *functionBitcode(NBFunction *function) {
 NBLibrary *library=function.library;const uint8_t *file=library.bytes;const size_t length=library.length;
 const char *wanted=function.name.UTF8String;
 if(!file || !wanted || length<88 || memcmp(file,"MTLB",4))return nil;
 uint64_t listAt,bitcodeAt,bitcodeSize;memcpy(&listAt,file+24,8);memcpy(&bitcodeAt,file+72,8);memcpy(&bitcodeSize,file+80,8);
 if(listAt>length-4 || bitcodeAt>length || bitcodeSize>length-bitcodeAt)return nil;
 uint32_t count;memcpy(&count,file+listAt,4);
 size_t at=listAt+4;
 for(uint32_t i=0;i<count;i++) {
  uint32_t size;if(at>length-4)return nil;memcpy(&size,file+at,4);
  if(size<8 || size>length-at)return nil;
  const size_t end=at+size;
  std::string name;uint64_t bytes=0,offset=0;bool sized=false,placed=false;
  for(size_t t=at+4;t+4<=end && memcmp(file+t,"ENDT",4);) {
   if(t+6>end)return nil;
   uint16_t n;memcpy(&n,file+t+4,2);const uint8_t *v=file+t+6;
   if(n>end-(t+6))return nil;
   if(!memcmp(file+t,"NAME",4))name.assign((const char *)v,strnlen((const char *)v,n));
   else if(!memcmp(file+t,"MDSZ",4) && n>=8){memcpy(&bytes,v,8);sized=true;}
   else if(!memcmp(file+t,"OFFT",4) && n>=24){memcpy(&offset,v+16,8);placed=true;}
   t+=6+n;
  }
  if(name==wanted) {
   if(!sized || !placed || offset>bitcodeSize || bytes>bitcodeSize-offset)return nil;
   return [NSData dataWithBytes:file+bitcodeAt+offset length:(NSUInteger)bytes];
  }
  at=end;
 }
 return nil;
}
// Each entry of a metallib's function list, in file order, as its name and TYPE (-1: none); false for a list that is not well formed.
static bool forEachLibraryEntry(NBLibrary *library,const std::function<bool(const std::string &,int)> &visit) {
 const uint8_t *file=library.bytes;const size_t length=library.length;
 if(!file || length<88 || memcmp(file,"MTLB",4))return false;
 uint64_t listAt;memcpy(&listAt,file+24,8);
 if(listAt>length-4)return false;
 uint32_t count;memcpy(&count,file+listAt,4);
 size_t at=listAt+4;
 for(uint32_t i=0;i<count;i++) {
  uint32_t size;if(at>length-4)return false;memcpy(&size,file+at,4);
  if(size<8 || size>length-at)return false;
  const size_t end=at+size;
  std::string entry;int type=-1;
  for(size_t t=at+4;t+4<=end && memcmp(file+t,"ENDT",4);) {
   if(t+6>end)return false;
   uint16_t n;memcpy(&n,file+t+4,2);const uint8_t *v=file+t+6;
   if(n>end-(t+6))return false;
   if(!memcmp(file+t,"NAME",4))entry.assign((const char *)v,strnlen((const char *)v,n));
   else if(!memcmp(file+t,"TYPE",4) && n>=1)type=v[0];
   t+=6+n;
  }
  if(!visit(entry,type))return true;
  at=end;
 }
 return true;
}
// At most16 failed lookups per process. No shader bodies, constants, aliases or
// substitute functions: compare the original MTLB table with the runtime view.
static void logFunctionLookupFailure(NBLibrary *library,NSString *name,NSString *reason) {
 static std::atomic<unsigned> reports{0};
 if(reports.fetch_add(1)>=16)return;
 const char *wanted=name.UTF8String;
 if(!wanted)return;
 unsigned entries=0,matches=0;int matchedType=-1;
 bool parsed=forEachLibraryEntry(library,[&](const std::string &entry,int type) {
  entries++;if(entry==wanted){matches++;matchedType=type;}return true;
 });
 // Capture reason before enumeration: bridge guards replace last-error state.
 uint32_t runtimeCount=nmb_library_function_count(library.handle);
 NSLog(@"native-metal: library-lookup id=%llu origin=%@ bytes=%llu requested=%@ raw_valid=%u raw_entries=%u raw_matches=%u raw_type=%d runtime_entries=%u reason=%@",
  (unsigned long long)library.diagnosticID,library.origin?:@"generated-AIR",
  (unsigned long long)library.length,name,parsed,entries,matches,matchedType,runtimeCount,reason);
}
static NBFunction *ciKernelFunction(NBLibrary *library,NSString *name) {
 const uint8_t *file=library.bytes;const char *wanted=name.UTF8String;
 if(!file || !wanted || library.length<88 || file[10]!=1)return nil;
 NBFunction *found=nil;
 forEachLibraryEntry(library,[&](const std::string &entry,int type) {
  if(entry!=wanted)return true;
  if(type==5) {
   found=[NBFunction new];found.name=name;found.functionType=(MTLFunctionType)4;found.library=library;
  }
  return false;
 });
 return found;
}
