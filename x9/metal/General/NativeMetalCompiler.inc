// SPDX-License-Identifier: MIT
// Adapted from UniXNU/AdrenoMetal 7183e588 for the original iOS16 compiler.
// No Apple executable patch and no Adreno pipeline binary is used.
static Timed gTimeCompile;
static std::atomic<uint64_t> gCompileCached;
typedef void (^NBCodeGenReply)(int32_t status,const void *reply,size_t length,const char *message);
// The compiler was written for MTLCompilerService's threads and recurses deeply; here it runs on whatever thread asked, and a dispatch
// worker's 512 KiB stack was too small for it (Aztec Ruins' shaders: a stack-protector abort inside libGPUCompilerImplLazy). A request
// runs on a thread of its own with a 16 MiB stack, the caller waiting.
static void *runBlock(void *block) {@autoreleasepool {((__bridge void (^)(void))block)();}return nullptr;}
static bool onLargeStack(void (^body)(void)) {
 pthread_attr_t attr;pthread_t thread;
 if(pthread_attr_init(&attr))return false;
 const bool made=!pthread_attr_setstacksize(&attr,16u<<20) && !pthread_create(&thread,&attr,runBlock,(__bridge void *)body);
 pthread_attr_destroy(&attr);
 if(!made)return false;
 return pthread_join(thread,nullptr)==0;
}
// A request's reply depends only on its bytes and the compiler, so a compiled one is kept in the process's Caches directory
// (Library/Caches/native-metal-shaders/SHA256.reply, SHA-256 of MTLCompiler's LC_UUID, the request type and its bytes): an
// app that compiles its shaders from source at every launch (GFXBench spends minutes of Clang and LLVM loading a test)
// compiles each one once. A file is the magic, the warning and metallib lengths and the SHA-256 of both, then both; it is
// written to a temporary name and renamed, and one that does not check out is compiled again. Failures are not kept.
// NATIVE_METAL_NO_SHADER_CACHE=1 turns it off.
struct NBShaderCacheHeader {uint32_t magic,warningBytes;uint64_t metallibBytes;unsigned char digest[CC_SHA256_DIGEST_LENGTH];};
static const uint32_t kNBShaderCacheMagic=0x3143534e;   // "NSC1"
static NSString *shaderCachePath(const unsigned char uuid[16],uint32_t type,const std::vector<uint8_t> &request) {
 static NSString *directory;
 static dispatch_once_t once;
 dispatch_once(&once,^{
  if(getenv("NATIVE_METAL_NO_SHADER_CACHE"))return;
  NSString *caches=NSSearchPathForDirectoriesInDomains(NSCachesDirectory,NSUserDomainMask,YES).firstObject;
  NSString *path=caches?[caches stringByAppendingPathComponent:@"native-metal-shaders"]:nil;
  if(path && [[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil])directory=path;
 });
 if(!directory)return nil;
 CC_SHA256_CTX context;CC_SHA256_Init(&context);
 CC_SHA256_Update(&context,uuid,16);CC_SHA256_Update(&context,&type,sizeof type);
 CC_SHA256_Update(&context,request.data(),(CC_LONG)request.size());
 unsigned char key[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(key,&context);
 char name[2*CC_SHA256_DIGEST_LENGTH+8];
 for(int i=0;i<CC_SHA256_DIGEST_LENGTH;i++)snprintf(name+2*i,3,"%02x",key[i]);
 strlcat(name,".reply",sizeof name);
 return [directory stringByAppendingPathComponent:@(name)];
}
static bool shaderCacheRead(NSString *path,NSData **metallib,NSString **warnings) {
 NSData *file=path?[NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil]:nil;
 NBShaderCacheHeader header;
 if(file.length<sizeof header || file.length>(256u<<20))return false;
 memcpy(&header,file.bytes,sizeof header);
 if(header.magic!=kNBShaderCacheMagic || header.metallibBytes<4 || header.warningBytes>file.length-sizeof header || header.metallibBytes!=file.length-sizeof header-header.warningBytes)return false;
 const uint8_t *payload=(const uint8_t *)file.bytes+sizeof header;
 unsigned char digest[CC_SHA256_DIGEST_LENGTH];
 CC_SHA256(payload,(CC_LONG)(header.warningBytes+header.metallibBytes),digest);
 if(memcmp(digest,header.digest,sizeof digest))return false;
 if(memcmp(payload+header.warningBytes,"MTLB",4))return false;
 *warnings=header.warningBytes?[[NSString alloc] initWithBytes:payload length:header.warningBytes encoding:NSUTF8StringEncoding]:nil;
 *metallib=[NSData dataWithBytes:payload+header.warningBytes length:(NSUInteger)header.metallibBytes];
 return true;
}
static void shaderCacheWrite(NSString *path,NSData *metallib,NSString *warnings) {
 if(!path || !metallib.length || metallib.length>(256u<<20) || warnings.length>(1u<<20))return;
 NSData *warned=[warnings dataUsingEncoding:NSUTF8StringEncoding];
 NBShaderCacheHeader header={kNBShaderCacheMagic,(uint32_t)warned.length,metallib.length,{}};
 NSMutableData *file=[NSMutableData dataWithCapacity:sizeof header+warned.length+metallib.length];
 [file appendBytes:&header length:sizeof header];
 if(warned.length)[file appendData:warned];
 [file appendData:metallib];
 uint8_t *bytes=(uint8_t *)file.mutableBytes;
 CC_SHA256(bytes+sizeof header,(CC_LONG)(warned.length+metallib.length),((NBShaderCacheHeader *)bytes)->digest);
 NSString *temporary=[path stringByAppendingFormat:@".%d.%u.tmp",getpid(),pthread_mach_thread_np(pthread_self())];
 if(![file writeToFile:temporary atomically:NO] || rename(temporary.fileSystemRepresentation,path.fileSystemRepresentation))
  unlink(temporary.fileSystemRepresentation);
}
// For refusal texts: the compiler version loaded ("top", "Current", the version directory's number, "none") and stitched libraries made
// and refused.
static char gCompilerLoaded[16]="untried";
static std::atomic<uint32_t> gStitchMade,gStitchFailed;
static NSData *codeGenRequest(uint32_t type,const std::vector<uint8_t> &request,NSString **warnings,NSString **failure) {
 if(request.empty() || request.size()>(256u<<20)){*failure=@"Metal compiler request exceeds the 256MiB frontend budget";return nil;}
 static void *service;
 static void (*build)(void *,void *,uint32_t,const void *,size_t,NBCodeGenReply);
 static unsigned char compilerUuid[16];
 static dispatch_once_t once;
 static std::mutex lock;
 dispatch_once(&once,^{
  static const char kFramework[]="/System/Library/PrivateFrameworks/MTLCompiler.framework";
  // Use the system top-level path, Metal.framework's own embedded version path,
  // or Current. Never guess the highest installed compiler's private wire ABI.
  std::string path=std::string(kFramework)+"/MTLCompiler";
  void *library=dlopen(path.c_str(),RTLD_NOW);
  // The version Apple's Metal.framework itself loads: the compiler path recorded among its C strings (24A446: Versions/32023). Read from
  // the loaded image, so a sandbox that refuses to list the framework's directory does not matter.
  if(!library) {
   static const char kPrefix[]="/System/Library/PrivateFrameworks/MTLCompiler.framework/Versions/";
   for(uint32_t i=0,n=_dyld_image_count();i<n && !library;i++) {
    const char *name=_dyld_get_image_name(i);
    if(!name || !strstr(name,"/Metal.framework/") || strcmp(strrchr(name,'/'),"/Metal"))continue;
    unsigned long size=0;
    const char *strings=(const char *)getsectiondata((const struct mach_header_64 *)_dyld_get_image_header(i),"__TEXT","__cstring",&size);
    for(unsigned long at=0;strings && at+sizeof kPrefix<size;) {
     const char *text=strings+at;const size_t length=strnlen(text,size-at);
     if(!strncmp(text,kPrefix,sizeof kPrefix-1) && length>sizeof kPrefix && !strcmp(text+length-12,"/MTLCompiler")) {
      path.assign(text,length);library=dlopen(path.c_str(),RTLD_NOW);break;
     }
     at+=length+1;
    }
   }
  }
  if(!library && !access((std::string(kFramework)+"/Versions/Current/MTLCompiler").c_str(),R_OK)) {
   path=std::string(kFramework)+"/Versions/Current/MTLCompiler";
   library=dlopen(path.c_str(),RTLD_NOW);
  }
  if(library && path!=std::string(kFramework)+"/MTLCompiler")NSLog(@"native-metal: Metal compiler loaded from %s",path.c_str());
  const size_t versions=path.find("/Versions/");
  strlcpy(gCompilerLoaded,!library?"none":versions==std::string::npos?"top":path.substr(versions+10,path.rfind('/')-versions-10).c_str(),
          sizeof gCompilerLoaded);
  void *(*create)(const char *)=library?(void *(*)(const char *))dlsym(library,"MTLCodeGenServiceCreate"):nullptr;
  build=library?(void (*)(void *,void *,uint32_t,const void *,size_t,NBCodeGenReply))dlsym(library,"MTLCodeGenServiceBuildRequest"):nullptr;
  service=create && build?create("native-metal"):nullptr;
  Dl_info info;
  if(build && dladdr((const void *)build,&info) && info.dli_fbase) {
   const mach_header_64 *header=(const mach_header_64 *)info.dli_fbase;
   const uint8_t *command=(const uint8_t *)(header+1);
   for(uint32_t i=0;i<header->ncmds;i++,command+=((const load_command *)command)->cmdsize)
    if(((const load_command *)command)->cmd==LC_UUID){memcpy(compilerUuid,((const uuid_command *)command)->uuid,16);break;}
  }
 });
 if(!service){*failure=@"the system's Metal compiler (MTLCompiler.framework) is not available";return nil;}
 NSString *cached=shaderCachePath(compilerUuid,type,request);
 NSData *kept=nil;NSString *keptWarnings=nil;
 if(shaderCacheRead(cached,&kept,&keptWarnings)) {
  gCompileCached++;
  if(gCompileCached.load()<=4)NSLog(@"native-metal: compiler cache type=%u metallib=%lu",type,(unsigned long)kept.length);
  *warnings=keptWarnings;*failure=nil;
  return kept;
 }
 __block NSData *metallib=nil;
 __block NSString *warned=nil,*message=nil;
 __block bool called=false;
 {
  std::lock_guard<std::mutex> guard(lock);
  const uint64_t compileBegan=nowUs();
  const bool ran=onLargeStack(^{build(service,nullptr,type,request.data(),request.size(),^(int32_t status,const void *reply,size_t length,const char *why) {
   called=true;
   uint32_t header=0,warning=0;
   if(reply && length>=16){memcpy(&header,(const uint8_t *)reply+8,4);memcpy(&warning,(const uint8_t *)reply+12,4);}
   if(status || !reply || length<16 || length>(256u<<20) || header<16 || (uint64_t)header+warning>length) {
    message=why && *why?@(why):[NSString stringWithFormat:@"the Metal compiler failed (status %d)",status];
    return;
   }
   if(length-header-warning<4 || memcmp((const uint8_t *)reply+header+warning,"MTLB",4)) {
    message=@"Metal compiler returned no metallib";return;
   }
   if(warning)warned=[[NSString alloc] initWithBytes:(const uint8_t *)reply+header length:warning encoding:NSUTF8StringEncoding];
   metallib=[NSData dataWithBytes:(const uint8_t *)reply+header+warning length:length-header-warning];
  });});
  if(!ran)message=@"Metal compiler worker could not create/join its 16MiB stack";
  gTimeCompile.add(compileBegan);
 }
 if(!called && !message)message=@"the Metal compiler did not answer synchronously";
 *warnings=warned;*failure=message;
 if(!message)shaderCacheWrite(cached,metallib,warned);
 static std::atomic<unsigned> logged{0};
 if(logged.fetch_add(1)<8)NSLog(@"native-metal: compiler request type=%u replied=%u metallib=%lu success=%u",type,called,(unsigned long)metallib.length,message==nil);
 return message?nil:metallib;
}
// Metal source, request 13: two little-endian u64 lengths, the source NUL-padded to a multiple of 4 bytes and the arguments
// NUL-terminated.
// DEBUG: NATIVE_METAL_DUMP_TESS=1 writes to standard error every Metal source with a patch function, and every tessellation
// pipeline's tessellation state and vertex inputs (what an app's tessellation asks of the bridge).
static const bool gDumpTessellation=getenv("NATIVE_METAL_DUMP_TESS")!=nullptr;
static void dumpText(NSString *text) {NSData *bytes=[text dataUsingEncoding:NSUTF8StringEncoding];(void)!write(2,bytes.bytes,bytes.length);}
static NSData *compileMetalSource(NSString *source,NSString *arguments,NSString **warnings,NSString **failure) {
 if(gDumpTessellation && [source containsString:@"[[patch"])
  dumpText([NSString stringWithFormat:@"\n=== native-metal tessellation source (options %@) ===\n%@\n=== end ===\n",arguments,source]);
 if(!source || !arguments){*failure=@"source and compiler arguments are required";return nil;}
 NSData *text=[source dataUsingEncoding:NSUTF8StringEncoding];
 const char *options=arguments.UTF8String;
 if(!text || !options || text.length>(128u<<20) || strlen(options)>(1u<<20)){*failure=@"invalid or oversized Metal source";return nil;}
 const uint64_t sourceBytes=(text.length+1+3)&~uint64_t(3),optionBytes=strlen(options)+1;
 std::vector<uint8_t> request(16+sourceBytes+optionBytes,0);
 memcpy(request.data(),&sourceBytes,8);memcpy(request.data()+8,&optionBytes,8);
 memcpy(request.data()+16,text.bytes,text.length);memcpy(request.data()+16+sourceBytes,options,optionBytes);
 return codeGenRequest(13,request,warnings,failure);
}
// The front end's arguments for a set of compile options, as Metal passes them: the language version, fast math (Metal's default),
// invariance, size optimization and the preprocessor macros; module caches in this process's temporary directory, which a sandboxed
// client may write.
static NSString *compileArgument(NSString *text) {
 if([text rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@" \t\"\\'"]].location==NSNotFound)return text;
 NSString *escaped=[[text stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"] stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
 return [NSString stringWithFormat:@"\"%@\"",escaped];
}
static NSString *compileArguments(MTLCompileOptions *options) {
 NSMutableArray<NSString *> *arguments=[NSMutableArray arrayWithObjects:@"--driver-mode=metal",@"-x",@"metal",nil];
 const NSUInteger version=options && options.languageVersion?(NSUInteger)options.languageVersion:(NSUInteger)MTLLanguageVersion3_0;
 const unsigned major=(unsigned)(version>>16),minor=(unsigned)(version&0xffff);
 [arguments addObject:[NSString stringWithFormat:major>=3?@"-std=metal%u.%u":@"-std=ios-metal%u.%u",major,minor]];
 const BOOL fast=options?((BOOL (*)(id,SEL))objc_msgSend)(options,sel_registerName("fastMathEnabled")):YES;
 [arguments addObject:fast?@"-ffast-math":@"-fno-fast-math"];
 if(options.preserveInvariance)[arguments addObject:@"-fpreserve-invariance"];
 if(options.optimizationLevel==MTLLibraryOptimizationLevelSize)[arguments addObject:@"-Os"];
 [options.preprocessorMacros enumerateKeysAndObjectsUsingBlock:^(NSString *name,NSObject *value,BOOL *) {
  [arguments addObject:compileArgument([NSString stringWithFormat:@"-D%@=%@",name,value])];
 }];
 [arguments addObject:compileArgument([@"-fmodules-cache-path=" stringByAppendingString:[NSTemporaryDirectory() stringByAppendingPathComponent:@"m2v-msl"]])];
 return [arguments componentsJoinedByString:@" "];
}
