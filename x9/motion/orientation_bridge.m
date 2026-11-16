/* Own-code adapter for iOS 16 CMDeviceOrientationManager. No binary offsets,
 * VM text patching, Apple file replacements or fabricated sensor readings.
 * Public entrypoints are confirmed in the original 20H392 CoreMotion:
 * start... 1919ba350, stop... 191c6f84c, available 1919baad0,
 * active 191c6f838, blocking getter 1919c022c. Leave BKOrientationManager's
 * orientation locks, app policy and downstream SpringBoard callbacks intact.
 * The stock getter/subscriber talks to an Apple motion userclient; a generic
 * accel HID alone cannot implement that port's proprietary selectors. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <os/log.h>
#include "../kernel/src/iokit/Kernel/arm/X9Motion.h"

typedef void (^X9OrientationHandler)(id, NSError *);
/* Dynamic class lookup avoids adding a link-time dependency on private
 * CoreMotion classes. Declaring the initializer gives ARC its init-family
 * ownership contract instead of an untyped objc_msgSend call. */
@protocol X9OrientationObject
- (instancetype)initWithOrientation:(int)value andTimestamp:(double)seconds;
@end
@interface X9OrientationState : NSObject
@property(nonatomic,strong) NSOperationQueue *queue;
@property(nonatomic,copy) X9OrientationHandler handler;
@property(nonatomic,strong) id orientation;
@property(nonatomic) BOOL running;
@property(nonatomic) uint64_t generation;
@end
@implementation X9OrientationState
@end
static char stateKey,queueKey;
static dispatch_queue_t motionQueue;
static NSHashTable<X9OrientationState *> *states;
static dispatch_source_t timer;
static Class orientationClass;
static uint64_t lastSequence,lastTicks,candidateTicks,orientationTicks;
static double gravity[3];
static int currentOrientation,candidateOrientation;
static BOOL filterValid,dataValid;
static mach_timebase_info_data_t timebase;

static void note(NSString *text)
{
 const char *s=text.UTF8String;
 if (!s) return;
 os_log(OS_LOG_DEFAULT,"X9 MOTION: %{public}s",s);
 (void)sysctlbyname("kern.x9_motion_note",NULL,NULL,(void *)s,strlen(s)+1);
}
static void onQueue(dispatch_block_t block)
{
 if (dispatch_get_specific(&queueKey)) block();
 else dispatch_sync(motionQueue,block);
}
static BOOL physicalAvailable(void)
{
 struct x9_motion_status s;size_t n=sizeof(s);
 return sysctlbyname("kern.x9_motion_status",&s,&n,NULL,0)==0 &&
     n==sizeof(s) && s.version==X9_MOTION_ABI && s.bytes==sizeof(s) &&
     s.active && s.configured && !s.error;
}
static id makeOrientation(int value,uint64_t ticks)
{
 double seconds=(double)ticks*timebase.numer/timebase.denom/1e9;
 return [(id<X9OrientationObject>)[orientationClass alloc]
     initWithOrientation:value andTimestamp:seconds];
}
static X9OrientationState *stateFor(id owner)
{
 X9OrientationState *s=objc_getAssociatedObject(owner,&stateKey);
 if (!s) {
  s=[X9OrientationState new];
  objc_setAssociatedObject(owner,&stateKey,s,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  [states addObject:s];
 }
 return s;
}
static void deliver(X9OrientationState *state,id value)
{
 if (!state.running || !state.queue || !state.handler || !value) return;
 state.orientation=value;
 uint64_t generation=state.generation;
 __weak X9OrientationState *weak=state;
 [state.queue addOperationWithBlock:^{
  __block X9OrientationHandler handler=nil;
  onQueue(^{
   X9OrientationState *s=weak;
   if (dataValid && s.running && s.generation==generation && s.orientation==value)
    handler=s.handler;
  });
  if (handler) handler(value,nil);
 }];
}
/* CMDeviceOrientation enum 0..6, as used by the stock manager/BS mapping.
 * iOS acceleration points down: upright y<0, face-up z<0, top edge left
 * (LandscapeLeft) x<0. Keep ambiguous
 * tilts in the last confirmed direction; reject free-fall and large shocks.
 * This is a gravity orientation estimator, NOT magnetic heading/attitude. */
static int classify(const double g[3])
{
 double x=fabs(g[0]),y=fabs(g[1]),z=fabs(g[2]);
 double norm=x*x+y*y+z*z;
 if (norm<0.36 || norm>1.96) return 0;
 if (z>0.85 && z>x+0.20 && z>y+0.20) return g[2]<0?5:6;
 if (y>0.55 && y>x+0.15) return g[1]<0?1:2;
 if (x>0.55 && x>y+0.15) return g[0]<0?3:4;
 return 0;
}
static void poll(void)
{
 struct x9_motion_sample s;size_t n=sizeof(s);
 BOOL fresh=sysctlbyname("kern.x9_motion_sample",&s,&n,NULL,0)==0 &&
     n==sizeof(s) && s.version==X9_MOTION_ABI && s.bytes==sizeof(s);
 if (!fresh) {
  if (dataValid) note(@"DATA unavailable/stale; no synthetic orientation callback");
  dataValid=filterValid=NO;currentOrientation=candidateOrientation=0;
  for (X9OrientationState *state in states) state.orientation=nil;
  return;
 }
 if (s.sequence==lastSequence) return; /* never retimestamp a repeated sample */
 lastSequence=s.sequence;
 if (!dataValid) note(@"real SCP acceleration available");
 dataValid=YES;
 double dt=filterValid && s.host_time>lastTicks?
     (double)(s.host_time-lastTicks)*timebase.numer/timebase.denom/1e9:0;
 if (!filterValid || dt>0.5 || dt<=0) {
  for (int i=0;i<3;i++) gravity[i]=(double)s.fixed_g[i]/65536;
  filterValid=YES;candidateOrientation=0;
 } else {
  double alpha=dt/(0.12+dt);
  for (int i=0;i<3;i++) gravity[i]+=alpha*((double)s.fixed_g[i]/65536-gravity[i]);
 }
 lastTicks=s.host_time;
 int candidate=classify(gravity);
 if (!candidate) { candidateOrientation=0;return; }
 if (candidate!=candidateOrientation) {
  candidateOrientation=candidate;candidateTicks=s.host_time;return;
 }
 double stable=(double)(s.host_time-candidateTicks)*timebase.numer/timebase.denom/1e9;
 if (stable<0.15 || candidate==currentOrientation) return;
 currentOrientation=candidate;
 orientationTicks=s.host_time;
 id value=makeOrientation(candidate,s.host_time);
 if (!value) { currentOrientation=0;note(@"CMDeviceOrientation allocation failed");return; }
 for (X9OrientationState *state in states) {
  state.orientation=value;
  deliver(state,value);
 }
 note([NSString stringWithFormat:@"orientation=%d raw=%d/%d/%d seq=%llu",
     candidate,s.raw[0],s.raw[1],s.raw[2],s.sequence]);
}
static BOOL available(id self,SEL cmd) { (void)self;(void)cmd;return physicalAvailable(); }
static BOOL active(id self,SEL cmd)
{
 (void)cmd;__block BOOL result=NO;
 onQueue(^{ result=stateFor(self).running; });return result;
}
static void start(id self,SEL cmd,NSOperationQueue *queue,X9OrientationHandler handler)
{
 (void)cmd;
 if (!queue || !handler) return;
 onQueue(^{
  X9OrientationState *s=stateFor(self);
  s.generation++;s.running=NO;
  poll(); /* copy-only startup: do not wait for a proprietary Apple port */
  s.queue=queue;s.handler=handler;s.running=YES;
  if (dataValid && currentOrientation) {
   if (!s.orientation) s.orientation=makeOrientation(currentOrientation,orientationTicks);
   deliver(s,s.orientation);
  }
 });
 note(@"CMDeviceOrientationManager subscriber started (stock NSOperationQueue contract)");
}
static void stop(id self,SEL cmd)
{
 (void)cmd;
 onQueue(^{
  X9OrientationState *s=stateFor(self);s.generation++;s.running=NO;
  s.handler=nil;s.queue=nil;s.orientation=nil;
 });
 note(@"CMDeviceOrientationManager subscriber stopped");
}
static id blocking(id self,SEL cmd)
{
 (void)cmd;__block id value=nil;
 onQueue(^{
  poll();X9OrientationState *s=stateFor(self);
  if (dataValid && currentOrientation) {
   if (!s.orientation) s.orientation=makeOrientation(currentOrientation,orientationTicks);
   value=s.orientation;
  }
 });
 return value; /* NULL before valid DATA, not an invented default portrait */
}
__attribute__((constructor)) static void install(void)
{
 @autoreleasepool {
  NSString *exe=NSProcessInfo.processInfo.arguments.firstObject;
  if (![exe.lastPathComponent isEqualToString:@"backboardd"]) return;
  if (NSProcessInfo.processInfo.operatingSystemVersion.majorVersion!=16) {
   note(@"not iOS 16; stock manager retained");return;
  }
  if (![[NSBundle bundleWithPath:@"/System/Library/Frameworks/CoreMotion.framework"] load]) {
   note(@"CoreMotion load failed; stock manager retained");return;
  }
  Class manager=NSClassFromString(@"CMDeviceOrientationManager");
  orientationClass=NSClassFromString(@"CMDeviceOrientation");
  const char *names[]={"isDeviceOrientationAvailable","isDeviceOrientationActive",
      "startDeviceOrientationUpdatesToQueue:withHandler:","stopDeviceOrientationUpdates","deviceOrientationBlocking"};
  IMP imps[]={(IMP)available,(IMP)active,(IMP)start,(IMP)stop,(IMP)blocking};
  const char *encodings[]={"B16@0:8","B16@0:8","v32@0:8@16@?24",
      "v16@0:8","@16@0:8"};
  Method methods[5];
  Method initializer=orientationClass?class_getInstanceMethod(orientationClass,
      sel_registerName("initWithOrientation:andTimestamp:")):NULL;
  if (!manager || !initializer ||
      strcmp(method_getTypeEncoding(initializer),"@28@0:8i16d20")) {
   note(@"CoreMotion class contract missing; stock manager retained");return;
  }
  for (unsigned i=0;i<5;i++) {
   methods[i]=class_getInstanceMethod(manager,sel_registerName(names[i]));
   if (!methods[i] || strcmp(method_getTypeEncoding(methods[i]),encodings[i])) {
    note(@"CoreMotion method contract missing; stock manager retained");return;
   }
  }
  mach_timebase_info(&timebase);
  motionQueue=dispatch_queue_create("x9.orientation",DISPATCH_QUEUE_SERIAL);
  dispatch_queue_set_specific(motionQueue,&queueKey,&queueKey,NULL);
  states=[NSHashTable weakObjectsHashTable];
  timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,motionQueue);
  dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,0),20*NSEC_PER_MSEC,2*NSEC_PER_MSEC);
  dispatch_source_set_event_handler(timer,^{ @autoreleasepool { poll(); } });
  for (unsigned i=0;i<5;i++) method_setImplementation(methods[i],imps[i]);
  dispatch_resume(timer);
  note(@"iOS 16 direction adapter installed; BK orientation lock/policy unchanged");
 }
}
