// SPDX-License-Identifier: MIT
// Shipped in the rev286 frontend (its on-device "no inventory" lines are in
// the r286 log). The rev287 round then proved the failure mode this file
// cannot see from inside: the plugin build compiles this frontend from
// .local/x9/MaliMetal/X9/General, a symlink into the main checkout, and
// that checkout had been reverted to a pre-priming state -- only the
// primelist was re-applied. Rev287 therefore shipped the inventory with a
// binary containing no priming code at all, and the session log held zero
// x9-prime lines because no code existed to emit one.
// build-general-metal.py now runs check-priming-consistency.py on both
// the source dir and the built bundle to refuse that mismatch.
//
// X9MetalPriming.inc -- pipeline priming for the X9 Metal frontend.
//
// Adapted from the mechanism (not the code) of UniXNU/AdrenoMetal's
// PrimeShaders.h: a background thread works through an inventory of system
// .metallib files once the desktop is quiet, so the first real use of a
// shader does not pay full compile + VkPipeline creation on the caller's
// frame. Progress is resumable across restarts and keyed by a build id.
//
// X9 differences from the AdrenoMetal original, both forced by this stack:
//  1. There is no native_bridge_metal_recent_commits(); the busy gate
//     samples this translation unit's own gCommits atomic (incremented in
//     NBCommandBuffer commit) over 250 ms windows instead.
//  2. PanVK creates VkPipelines at draw time, not at pipeline-state
//     creation (see the "V vkpipe" note in native_bridge_metal_telemetry),
//     so a primed render pipeline is only real after one dummy draw. The
//     draw pattern (BGRA8Unorm 64x64 target, 3 vertices, zero buffers) is
//     the one the frontend's own benchmark already uses on device.
//
// Inventory: <frontend image directory>/X9GeneralMetal.primelist (one
// absolute .metallib path per line), falling back to an embedded
// __DATA,__primelist section. No directory scanning: AdrenoMetal's notes
// record a scan stalling frames, and the render server's sandbox refuses
// most listings anyway. An empty inventory is a logged no-op.
//
// Scope of a prime pass, per library: every function is created (warms
// the AIR/metallib path and NativeMetalCompiler's on-disk cache), every
// kernel function gets a compute pipeline state, and vertex/fragment
// pairs named *_vert_* / *_frag_* (the QuartzCore convention the
// benchmark relies on) get a render pipeline state plus one dummy draw.
// Other render pairings are deliberately not guessed in this version.
//
// NATIVE_METAL_NO_PRIME=1 disables priming entirely (same naming style
// as NATIVE_METAL_NO_SHADER_CACHE in NativeMetalCompiler.inc).

extern "C" {
volatile uint32_t x9_prime_compiling, x9_prime_libraries_done,
    x9_prime_libraries_total, x9_prime_made, x9_prime_rate,
    x9_prime_functions_resumed, x9_prime_pass_complete,
    x9_prime_libraries_failed;
}

#include <sys/stat.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace x9prime {

// More than this many command-buffer commits in a 250 ms window means the
// desktop is animating (AdrenoMetal's 120 Hz calibration: idle desktop
// redraws are ~6 per 250 ms, animation is 15-30; their bar is 8).
static constexpr uint64_t kQuietCommits = 8;
// Give a permanently busy desktop its pass anyway after this long.
static constexpr uint64_t kBusyDeadlineMs = 90000;

static uint64_t primeNowMs() {return clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000000;}

static std::set<std::string> primeReadLines(const std::string &path) {
 std::set<std::string> lines;
 if(FILE *f=fopen(path.c_str(),"r")) {
  char line[1024];
  while(fgets(line,sizeof line,f)) {
   line[strcspn(line,"\n")]=0;
   if(line[0])lines.insert(line);
  }
  fclose(f);
 }
 return lines;
}

static void primeAppendLine(const std::string &path,const std::string &line) {
 if(FILE *f=fopen(path.c_str(),"a")) {fprintf(f,"%s\n",line.c_str());fclose(f);}
}

// The LC_UUID of the image this code is linked into, hex; "unknown" if the
// image has none. Same extraction pattern NativeMetalCompiler.inc uses
// for the MTLCompiler image.
static std::string primeImageUuid(const void *anchor) {
 Dl_info info;
 if(!dladdr(anchor,&info)||!info.dli_fbase)return "unknown";
 const struct mach_header_64 *header=(const struct mach_header_64 *)info.dli_fbase;
 const uint8_t *command=(const uint8_t *)(header+1);
 for(uint32_t i=0;i<header->ncmds;i++,command+=((const load_command *)command)->cmdsize)
  if(((const load_command *)command)->cmd==LC_UUID) {
   const uint8_t *uuid=((const uuid_command *)command)->uuid;
   char text[33]={};
   for(int k=0;k<16;k++)snprintf(text+2*k,3,"%02x",uuid[k]);
   return text;
  }
 return "unknown";
}

// Build identity: a change of the frontend image or of the Vulkan loader
// (PanVK) invalidates all recorded progress, like AdrenoMetal's
// runtime-build + freedreno-UUID pair. A plugin rebuild that changes
// neither keeps what was finished.
static std::string primeBuildId(const char *loaderPath) {
 std::string loader=loaderPath?loaderPath:"";
 const size_t slash=loader.find_last_of('/');
 if(slash!=std::string::npos)loader=loader.substr(slash+1);
 return primeImageUuid((const void *)&primeImageUuid)+"-"+loader+"-prime1";
}

static std::string primeDirectory() {
 static NSString *directory;
 static dispatch_once_t once;
 dispatch_once(&once,^{
  NSString *caches=NSSearchPathForDirectoriesInDomains(NSCachesDirectory,NSUserDomainMask,YES).firstObject;
  NSString *path=caches?[caches stringByAppendingPathComponent:@"native-metal-priming"]:nil;
  if(path && [[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil])directory=path;
 });
 return directory?std::string(directory.fileSystemRepresentation):std::string();
}

static std::vector<std::string> primeLibraries(std::string &note) {
 std::set<std::string> list;
 Dl_info info;
 if(dladdr((const void *)&primeLibraries,&info)&&info.dli_fbase) {
  std::string beside=std::string(info.dli_fname);
  const size_t slash=beside.find_last_of('/');
  if(slash!=std::string::npos) {
   beside=beside.substr(0,slash+1)+"X9GeneralMetal.primelist";
   list=primeReadLines(beside);
   if(!list.empty())note="list="+beside;
  }
 }
 if(list.empty()) {
  unsigned long size=0;
  const uint8_t *data=nullptr;
  if(dladdr((const void *)&primeLibraries,&info)&&info.dli_fbase)
   data=getsectiondata((const struct mach_header_64 *)info.dli_fbase,"__DATA","__primelist",&size);
  if(data&&size) {
   const std::string text((const char *)data,size);
   for(size_t at=0;at<text.size();) {
    size_t end=text.find('\n',at);if(end==std::string::npos)end=text.size();
    if(end>at)list.insert(text.substr(at,end-at));
    at=end+1;
   }
   if(!list.empty())note="list=embedded";
  }
 }
 if(list.empty())note="list=none";
 std::vector<std::string> found;
 std::set<std::pair<dev_t,ino_t>> seen;
 for(const std::string &path:list) {
  struct stat st={};
  if(path.empty()||path[0]!='/'||stat(path.c_str(),&st)||!S_ISREG(st.st_mode))continue;
  if(!seen.insert({st.st_dev,st.st_ino}).second)continue;
  found.push_back(path);
 }
 return found;
}

// Wait until a 250 ms window is quiet, after having seen the desktop busy
// at least once (a never-busy process starts immediately). Returns the
// largest commit count any window saw. Deadline: kBusyDeadlineMs.
static unsigned primeWaitForQuiet(bool *forced) {
 const uint64_t began=primeNowMs();
 bool busy=false;unsigned peak=0;
 if(forced)*forced=false;
 for(;;) {
  const uint64_t before=gCommits.load();
  struct timespec ts={0,250*1000*1000};
  nanosleep(&ts,nullptr);
  const uint64_t delta=gCommits.load()-before;
  if(delta>peak)peak=(unsigned)delta;
  x9_prime_rate=(uint32_t)peak;
  if(delta>kQuietCommits) {busy=true;x9_prime_compiling=2;}
  else return peak;   // never busy, or the first quiet window after busy
  if(primeNowMs()-began>kBusyDeadlineMs) {if(forced)*forced=true;return peak;}
 }
}

// Prime one library on `device`. Returns false only if the library itself
// could not be opened. Function/pipeline failures are counted, not fatal:
// a library the frontend refuses today may prime after a later fix, and
// the done-key below records the attempt so a pass is not repeated within
// the same build.
static bool primeLibrary(id<MTLDevice> device,NSString *path) {
 NSError *error=nil;
 id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
 if(!library)return false;
 NSArray<NSString *> *names=library.functionNames;
 @autoreleasepool {
  id<MTLCommandQueue> queue=nil;
  id<MTLTexture> target=nil;
  id<MTLBuffer> zeros=nil;
  for(NSString *name in names) {
   id<MTLFunction> function=[library newFunctionWithName:name];
   if(!function)continue;
   if(function.functionType==MTLFunctionTypeKernel) {
    NSError *perr=nil;
    id<MTLComputePipelineState> state=[device newComputePipelineStateWithFunction:function error:&perr];
    if(state)x9_prime_made++;
    continue;
   }
   if(function.functionType!=MTLFunctionTypeVertex)continue;
   NSString *fragmentName=[name stringByReplacingOccurrencesOfString:@"_vert_" withString:@"_frag_"];
   if([fragmentName isEqualToString:name]||![names containsObject:fragmentName])continue;
   id<MTLFunction> fragment=[library newFunctionWithName:fragmentName];
   if(!fragment)continue;
   MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];
   d.vertexFunction=function;d.fragmentFunction=fragment;
   d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
   NSError *perr=nil;
   id<MTLRenderPipelineState> state=[device newRenderPipelineStateWithDescriptor:d error:&perr];
   if(!state)continue;
   x9_prime_made++;
   // PanVK builds the VkPipeline at draw time: one dummy draw or the
   // prime did not happen. Same shape as native_bridge_metal_benchmark.
   if(!queue) {
    queue=[device newCommandQueue];
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
    td.usage=MTLTextureUsageRenderTarget;
    target=[device newTextureWithDescriptor:td];
    zeros=[device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
   }
   if(!queue||!target||!zeros)continue;
   MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
   pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
   id<MTLCommandBuffer> commands=[queue commandBuffer];
   id<MTLRenderCommandEncoder> encoder=[commands renderCommandEncoderWithDescriptor:pass];
   [encoder setRenderPipelineState:state];
   for(NSUInteger i=0;i<8;i++) {[encoder setVertexBuffer:zeros offset:0 atIndex:i];[encoder setFragmentBuffer:zeros offset:0 atIndex:i];}
   [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
   [encoder endEncoding];[commands commit];[commands waitUntilCompleted];
  }
 }
 return true;
}

struct PrimeContext { id<MTLDevice> device; std::string build; };

static void *primeThread(void *raw) {
 @autoreleasepool {
  PrimeContext *ctx=(PrimeContext *)raw;
  pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND,0);
  const uint64_t began=primeNowMs();
  std::string note;
  std::vector<std::string> libraries=primeLibraries(note);
  x9_prime_libraries_total=(uint32_t)libraries.size();
  if(libraries.empty()) {
   NSLog(@"x9-prime: no inventory (%s); nothing primed build=%s",note.c_str(),ctx->build.c_str());
   x9_prime_pass_complete=2;
   delete ctx;return nullptr;
  }
  NSLog(@"x9-prime: begin %s libraries=%u build=%s",note.c_str(),
        (unsigned)libraries.size(),ctx->build.c_str());
  const std::string dir=primeDirectory();
  const std::string donePath=dir.empty()?std::string():dir+"/prime-done";
  const std::set<std::string> done=donePath.empty()?std::set<std::string>():primeReadLines(donePath);
  unsigned finished=0,resumed=0;
  for(const std::string &path:libraries) {
   struct stat st={};
   if(stat(path.c_str(),&st)) {x9_prime_libraries_failed++;continue;}
   const std::string id=path+"\t"+std::to_string((long long)st.st_size)+"\t"+
       std::to_string((long long)st.st_mtimespec.tv_sec)+"\t"+ctx->build;
   if(done.count(id)) {resumed++;finished++;x9_prime_libraries_done=finished;continue;}
   bool forced=false;
   const unsigned peak=primeWaitForQuiet(&forced);
   if(forced)
    NSLog(@"x9-prime: quiet deadline before %s (peak=%u commits/250ms); proceeding busy",
          path.c_str(),peak);
   x9_prime_compiling=3;
   const bool ok=primeLibrary(ctx->device,[NSString stringWithUTF8String:path.c_str()]);
   x9_prime_compiling=0;
   if(ok) {finished++;if(!donePath.empty())primeAppendLine(donePath,id);}
   else x9_prime_libraries_failed++;
   x9_prime_libraries_done=finished;
  }
  x9_prime_functions_resumed=resumed;
  x9_prime_pass_complete=(finished==libraries.size()&&!x9_prime_libraries_failed)?1:2;
  NSLog(@"x9-prime: pass build=%s libraries=%u finished=%u resumed=%u failed=%u made=%u rate=%u ms=%llu",
        ctx->build.c_str(),(unsigned)libraries.size(),finished,resumed,
        (unsigned)x9_prime_libraries_failed,(unsigned)x9_prime_made,
        (unsigned)x9_prime_rate,(unsigned long long)(primeNowMs()-began));
  delete ctx;return nullptr;
 }
}

} // namespace x9prime

// Start priming for the process's one device. Called once from
// native_bridge_metal_open after the device object exists. `loader` is
// the path handed to nmb_device_open (the PanVK build's identity input).
static void x9_prime_start(id<MTLDevice> device,const char *loader) {
 static std::atomic<bool> started{false};
 if(!device||started.exchange(true))return;
 if(getenv("NATIVE_METAL_NO_PRIME")) {NSLog(@"x9-prime: disabled (NATIVE_METAL_NO_PRIME)");return;}
 auto *ctx=new x9prime::PrimeContext{(id<MTLDevice>)device,x9prime::primeBuildId(loader)};
 pthread_t thread;
 pthread_attr_t attr;
 if(pthread_attr_init(&attr)) {delete ctx;return;}
 pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
 const bool made=!pthread_create(&thread,&attr,&x9prime::primeThread,ctx);
 pthread_attr_destroy(&attr);
 if(!made)delete ctx;
}
