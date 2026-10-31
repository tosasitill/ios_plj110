// SPDX-License-Identifier: MIT
// UniXNU/AdrenoMetal 7183e588, included within NBDeviceMethods.
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error {
 if(!source){if(error)*error=nbError(@"no source");return nil;}
 if(options && options.libraryType==MTLLibraryTypeDynamic) {
  if(error)*error=[NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorUnsupported
                                  userInfo:@{NSLocalizedDescriptionKey:@"dynamic libraries are not supported"}];
  return nil;
 }
 NSString *warnings=nil,*failure=nil;
 NSData *metallib=compileMetalSource(source,compileArguments(options),&warnings,&failure);
 if(!metallib) {
  NBLOG(@"native-metal: newLibraryWithSource: %@ | compiler %s",[failure substringToIndex:MIN(failure.length,(NSUInteger)300)],gCompilerLoaded);
  if(error)*error=[NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorCompileFailure userInfo:@{NSLocalizedDescriptionKey:failure}];
  return nil;
 }
 dispatch_data_t data=dispatch_data_create(metallib.bytes,metallib.length,nullptr,^{(void)metallib;});
 id<MTLLibrary> library=[(id<MTLDevice>)self newLibraryWithData:data error:error];
 // As Metal does: a library compiled with warnings comes with them as a CompileWarning error.
 if(library && warnings.length && error)
  *error=[NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorCompileWarning userInfo:@{NSLocalizedDescriptionKey:warnings}];
 return library;
}
- (void)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options completionHandler:(MTLNewLibraryCompletionHandler)handler {
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
  NSError *error=nil;
  id<MTLLibrary> library=[(id<MTLDevice>)self newLibraryWithSource:source options:options error:&error];
  if(handler)handler(library,error);
 });
}
// Function stitching (Core Image makes its kernels so: a DAG of ci_stdlib_stitchable's functions with builtin and buffer nodes) is the
// front end's request 14, as Metal's own MTLLibraryBuilder writes it: u32 fourcc tags, 'dag ' and the graphs' DAG text NUL-terminated,
// 'numf' and the u32 count of functions, then for each function 'bitc', the u32 size and its wrapped bitcode. The reply is a metallib
// with the stitched functions. Metal's own path wants its own function objects (their library data) and refuses these.
- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error {
 NSArray<MTLFunctionStitchingGraph *> *graphs=d.functionGraphs;NSArray<id<MTLFunction>> *functions=d.functions;
 if(!graphs.count || !functions.count){if(error)*error=nbError(@"a stitched library needs graphs and functions");return nil;}
 if(!methodABI(self,sel_registerName("newDagStringWithGraphs:"),"@24@0:8@16")) {
  if(error)*error=nbError(@"original Metal DAG serializer ABI is unavailable");return nil;
 }
 NSString *dag=[(id<NBOriginalStitching>)self newDagStringWithGraphs:graphs];
 if(!dag.length){if(error)*error=nbError(@"the stitching graphs have no DAG text");return nil;}
 if(functions.count>UINT32_MAX || dag.length>(16u<<20)) {if(error)*error=nbError(@"stitching request exceeds frontend budget");return nil;}
 std::vector<uint8_t> request;
 auto word=[&](uint32_t v){const uint8_t *b=(const uint8_t *)&v;request.insert(request.end(),b,b+4);};
 const char *text=dag.UTF8String;
 if(!text){if(error)*error=nbError(@"DAG is not valid UTF-8");return nil;}
 word(0x64616720 /* 'dag ' */);request.insert(request.end(),text,text+strlen(text)+1);
 word(0x6e756d66 /* 'numf' */);word((uint32_t)functions.count);
 for(id function in functions) {
  NSData *bitcode=[function isKindOfClass:[NBFunction class]]?functionBitcode((NBFunction *)function):nil;
  if(!bitcode){if(error)*error=nbError([NSString stringWithFormat:@"stitched function %@ has no bitcode here",[function name]]);return nil;}
  if(bitcode.length>UINT32_MAX || bitcode.length>(256u<<20) || request.size()>(256u<<20)-bitcode.length) {
   if(error)*error=nbError(@"stitching bitcode exceeds frontend budget");return nil;
  }
  word(0x62697463 /* 'bitc' */);word((uint32_t)bitcode.length);
  const uint8_t *b=(const uint8_t *)bitcode.bytes;request.insert(request.end(),b,b+bitcode.length);
 }
 // macOS 27's compiler (MTLCompilerObject::stitchFunctionDag) reads three more tagged slots after the functions, each a tag that
 // is consumed whether or not it matches, and aborts when the request ends before them: 'nums' and the count of scripts, 'ptfm'
 // and the platform, 'arid' and the size of an archive id. None of them, as zeros.
 word(0x6e756d73 /* 'nums' */);word(0);word(0x7074666d /* 'ptfm' */);word(0);word(0x61726964 /* 'arid' */);word(0);
 NSString *warnings=nil,*failure=nil;
 NSData *metallib=codeGenRequest(14,request,&warnings,&failure);
 if(!metallib) {
  gStitchFailed++;
  NBLOG(@"native-metal: stitched library: %@",[failure substringToIndex:MIN(failure.length,(NSUInteger)300)]);
  noteProblem([@"stitched library: " stringByAppendingString:[failure substringToIndex:MIN(failure.length,(NSUInteger)120)]]);
  if(error)*error=[NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorCompileFailure userInfo:@{NSLocalizedDescriptionKey:failure}];
  return nil;
 }
 gStitchMade++;
 dispatch_data_t data=dispatch_data_create(metallib.bytes,metallib.length,nullptr,^{(void)metallib;});
 return [(id<MTLDevice>)self newLibraryWithData:data error:error];
}
- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d destinationBinaryArchive:(id<MTLBinaryArchive>)archive error:(NSError **)error {
 (void)archive;return [(id<MTLDevice>)self newLibraryWithStitchedDescriptor:d error:error];
}
- (void)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d completionHandler:(MTLNewLibraryCompletionHandler)handler {
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
  NSError *error=nil;
  id<MTLLibrary> library=[(id<MTLDevice>)self newLibraryWithStitchedDescriptor:d error:&error];
  if(handler)handler(library,error);
 });
}
