#import "X9MetalPolicy.h"
#import "X9Metal.h"
#import <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <grp.h>
#include <errno.h>
#import <objc/objc-exception.h>
static int console=-1;
static void say(const char *format,...) {
 char text[640];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);
 if(console>=0){char record[768];int n=snprintf(record,sizeof(record),"X9 MTLTEST: caller=%s uid=%u %s\n",getprogname(),(unsigned)geteuid(),text);if(n>0)write(console,record,(size_t)n<sizeof(record)?(size_t)n:sizeof(record)-1);}
 else {fprintf(stdout,"X9 MTLTEST: caller=%s uid=%u %s\n",getprogname(),(unsigned)geteuid(),text);fflush(stdout);}
}
static objc_exception_preprocessor priorPreprocessor;
static id observeException(id exception) {
 if([exception isKindOfClass:[NSException class]]) {
  NSException *e=exception;
  say("Objective-C exception name=%s reason=%s",e.name.UTF8String,e.reason.UTF8String);
 }else say("Objective-C exception class=%s",object_getClassName(exception));
 return priorPreprocessor?priorPreprocessor(exception):exception;
}
static BOOL run(id<MTLDevice> device,id<MTLComputePipelineState> pipeline,unsigned count,unsigned local,unsigned seed) {
 NSUInteger bytes=count*4,offset=16,total=bytes+32;
 id<MTLBuffer> a=[device newBufferWithLength:total options:MTLResourceStorageModeShared];
 id<MTLBuffer> b=[device newBufferWithLength:total options:MTLResourceStorageModeShared];
 id<MTLBuffer> c=[device newBufferWithLength:total options:MTLResourceStorageModeShared];
 if(!a||!b||!c){say("buffer allocation FAILED");return NO;}
 memset(a.contents,0xa5,total);memset(b.contents,0xa5,total);memset(c.contents,0xa5,total);
 uint32_t *av=(uint32_t *)((char *)a.contents+offset),*bv=(uint32_t *)((char *)b.contents+offset),*cv=(uint32_t *)((char *)c.contents+offset);
 for(unsigned i=0;i<count;i++){av[i]=0xfffffff0u+seed*0x1234567u+i*0x89abcdefu;bv[i]=0x31u+seed*0x1020304u-i*0x07060503u;}
 id<MTLCommandQueue> queue=[device newCommandQueue];id<MTLCommandBuffer> command=[queue commandBuffer];
 id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
 if(!queue||!command||!encoder){say("queue/encoder creation FAILED");return NO;}
 [encoder setComputePipelineState:pipeline];[encoder setBuffer:a offset:offset atIndex:0];
 [encoder setBuffer:b offset:offset atIndex:1];[encoder setBuffer:c offset:offset atIndex:2];
 [encoder dispatchThreadgroups:MTLSizeMake(count/local,1,1) threadsPerThreadgroup:MTLSizeMake(local,1,1)];[encoder endEncoding];
 __block BOOL callback=NO;
 [command addCompletedHandler:^(id<MTLCommandBuffer> completed){callback=completed.status==MTLCommandBufferStatusCompleted;}];
 [command commit];[command waitUntilCompleted];
 if(command.status!=MTLCommandBufferStatusCompleted||command.error||!callback) {
  say("commit FAILED status=%lu error=%s",(unsigned long)command.status,command.error.description.UTF8String);return NO;
 }
 for(unsigned i=0;i<count;i++)if(cv[i]!=(uint32_t)(av[i]+bv[i])) {
  say("readback FAILED index=%u expected=%08x actual=%08x",i,av[i]+bv[i],cv[i]);return NO;
 }
 for(NSUInteger i=0;i<total;i++)if((i<offset||i>=offset+bytes)&&((unsigned char *)c.contents)[i]!=0xa5) {
  say("output guard FAILED byte=%lu",(unsigned long)i);return NO;
 }
 say("Metal buffer -> encoder -> commit -> GPU -> readback PASS count=%u local=%u offset=16 callback=completed",count,local);return YES;
}
int main(int argc,char **argv) {
 BOOL mobileChild=argc==2&&!strcmp(argv[1],"--mobile-child");
 @autoreleasepool {
  char args[2048]={0};size_t argBytes=sizeof(args)-1;BOOL opted=NO;
  if(sysctlbyname("kern.bootargs",args,&argBytes,NULL,0))return mobileChild?5:0;
  char *save=NULL;for(char *word=strtok_r(args," \t\r\n",&save);word;word=strtok_r(NULL," \t\r\n",&save))
   if(x9_metal_mode_from_token(word))opted=YES;
  if(!opted)return mobileChild?5:0;
  console=open("/dev/console",O_WRONLY|O_NOCTTY);
  priorPreprocessor=objc_setExceptionPreprocessor(observeException);
  say("BEGIN system Metal discovery caller=%s uid=%u gid=%u; native add_u32; image=%s",getprogname(),(unsigned)geteuid(),(unsigned)getegid(),argc?argv[0]:"unknown");
  BOOL mobile=!strcmp(getprogname(),"x9-metal-mobile");
  if(mobileChild!=mobile||(mobile&&(geteuid()!=501||getegid()!=501))){say("mobile launch identity FAILED");return 5;}
  BOOL ready=NO;
  for(unsigned wait=0;wait<60&&!ready;wait++) {
   io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("SweetX9Compute"));
   if(service){IOObjectRelease(service);ready=YES;}else usleep(500000);
  }
  if(!ready){say("service unavailable after30s; discovery not attempted");return 1;}
  NSError *error=nil;id<MTLDevice> device=MTLCreateSystemDefaultDevice();
  if(!device){say("device FAILED: %s",error.description.UTF8String);return 1;}
  say("MTLCreateSystemDefaultDevice returned class=%s name=%s",NSStringFromClass([(id)device class]).UTF8String,device.name.UTF8String);
  if(![NSStringFromClass([(id)device class]) isEqualToString:@"X9Device"]) {say("unexpected device class");return 1;}
  id<MTLLibrary> library=[device newDefaultLibrary];id<MTLFunction> function=[library newFunctionWithName:@"add_u32"];
  id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:function error:&error];
  if(!library||!function||!pipeline){say("pipeline FAILED: %s",error.description.UTF8String);return 2;}
  const unsigned counts[]={1,16,128,256},locals[]={1,16,32,128};
  for(unsigned i=0;i<4;i++)if(!run(device,pipeline,counts[i],locals[i],i+1+(mobile?501u:0u)))return 3;
  /* Unsupported function and source compilation must not silently succeed. */
  if([library newFunctionWithName:@"unknown_kernel"]){say("unknown function unexpectedly accepted");return 4;}
  error=nil;
  if([device newLibraryWithSource:@"kernel void arbitrary() {}" options:nil error:&error]||!error) {
   say("unsupported MSL compilation unexpectedly accepted");return 4;
  }
  say("PASS4/4 system Metal compute,401 arithmetic results; caller=%s uid=%u; unsupported source/function rejected",getprogname(),(unsigned)geteuid());
  if(!mobile) {
   pid_t child=fork();
   if(child<0){say("mobile fork FAILED errno=%d",errno);return 6;}
   if(child==0) {
    /* After fork use only direct system calls until exec; inherited Metal
     * objects/locks and IOKit connections are never used in the child. */
    if(setgroups(0,NULL)||setgid(501)||setuid(501))_exit(126);
    char *childArgs[]={"/private/var/x9-metal-pan168/x9-metal-mobile","--mobile-child",NULL};
    char *childEnv[]={"PATH=/usr/bin:/bin:/usr/sbin:/sbin","HOME=/private/var/mobile",NULL};
    execve(childArgs[0],childArgs,childEnv);_exit(127);
   }
   say("mobile child started pid=%d; waiting for independent UID501 exec",child);
   int status=0;BOOL finished=NO;
   for(unsigned tick=0;tick<900;tick++) {
    pid_t result=waitpid(child,&status,WNOHANG);
    if(result==child){finished=YES;break;}
    if(result<0&&errno!=EINTR){say("mobile wait FAILED errno=%d",errno);return 6;}
    usleep(100000);
   }
   if(!finished){say("mobile deadline90s; child left alive, no GPU reset or forced termination");return 6;}
   if(!WIFEXITED(status)||WEXITSTATUS(status)){say("mobile child FAILED waitstatus=%d",status);return 6;}
   say("SYSTEM PAIR PASS8/8 root+UID501,802 arithmetic results; child exec completed");
  }
  if(console>=0)close(console);return 0;
 }
}
