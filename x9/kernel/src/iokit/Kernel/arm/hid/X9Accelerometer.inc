/* 20H392 BKOrientationManager 100079fe4 matches built-in ff00/3.
 * BKAccelerometerInterface 100043cc8..100043cf8 reads type13 XYZ (16.16 g).
 * CMDeviceOrientationManager's separate Apple motion port is adapted by the
 * own-code iOS bridge; this esuc service also feeds ordinary accel clients. */
#include "../X9Motion.h"
#include "../X9ALS.h"
#include <sys/errno.h>
#include "IOHIDEventServiceKeys_Private.h"
class X9Accelerometer : public IOHIDEventService {
 OSDeclareDefaultStructors(X9Accelerometer);
public:
 bool start(IOService *) APPLE_KEXT_OVERRIDE;
 void stop(IOService *) APPLE_KEXT_OVERRIDE;
 void free(void) APPLE_KEXT_OVERRIDE;
 UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return 0xff00; }
 UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return 3; }
 UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 0x6991; }
 UInt32 getReportInterval() APPLE_KEXT_OVERRIDE { return fIntervalUs; }
 IOHIDEvent *copyEvent(IOHIDEventType,IOHIDEvent *,IOOptionBits) APPLE_KEXT_OVERRIDE;
 IOHIDEvent *copyMatchingEvent(OSDictionary *) APPLE_KEXT_OVERRIDE;
 IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
private:
 static void poll(OSObject *,IOTimerEventSource *);
 static IOReturn configure(OSObject *,void *,void *,void *,void *);
 IOHIDEvent *makeEvent(const x9_motion_sample &,IOOptionBits);
 void cleanup();
 IOTimerEventSource *fTimer;
 UInt32 fIntervalUs;
 uint64_t fSequence;
 bool fActive,fValid;
 int fError;
};
OSDefineMetaClassAndStructors(X9Accelerometer,IOHIDEventService);
bool X9Accelerometer::start(IOService *p)
{
 int sensors=0,mode=0;PE_parse_boot_argn("x9s",&sensors,sizeof(sensors));
 mode=(sensors&16)?3:0;PE_parse_boot_argn("x9_motion",&mode,sizeof(mode));
 if (!sweet_early_is_x9() || mode!=3) return false;
 fTimer=NULL;fIntervalUs=20000;fSequence=0;fActive=fValid=false;fError=0;
 if (!IOHIDEventService::start(p) || x9_motion_init()) return false;
 fActive=true;
 fTimer=IOTimerEventSource::timerEventSource(this,&X9Accelerometer::poll);
 if (!fTimer || _hidWorkLoop->addEventSource(fTimer)!=kIOReturnSuccess) {
  OSSafeReleaseNULL(fTimer);cleanup();return false;
 }
 setName("accel");setProperty("name","accel");setProperty("Product","X9 Accelerometer");
 setProperty("X9MotionBackend","shared-scp");setProperty("X9MotionDataValid",kOSBooleanFalse);
 int e=x9_als_start_shared_transport();
 if (e) x9_motion_native_disconnect(e);
 registerService();fTimer->setTimeoutUS(20000);
 IOLog("X9 MOTION: HID registered accel built-in ff00/3 native SCP errno=%d; no synthetic orientation\n",e);
 return true;
}
void X9Accelerometer::cleanup()
{
 if (fTimer) {
  fTimer->cancelTimeout();fTimer->disable();
  if (_hidWorkLoop) _hidWorkLoop->removeEventSource(fTimer);
  OSSafeReleaseNULL(fTimer);
 }
 if (fActive) { x9_motion_shutdown();fActive=false; }
}
void X9Accelerometer::stop(IOService *p) { cleanup();IOHIDEventService::stop(p); }
void X9Accelerometer::free() { cleanup();IOHIDEventService::free(); }
IOHIDEvent *X9Accelerometer::makeEvent(const x9_motion_sample &s,IOOptionBits options)
{
 IOHIDEvent *e=IOHIDEvent::accelerometerEvent(s.host_time,s.fixed_g[0],s.fixed_g[1],s.fixed_g[2],
     kIOHIDMotionTypeNormal,0,(UInt32)s.sequence,options);
 if (e) e->setSenderID(getRegistryEntryID());
 return e;
}
IOHIDEvent *X9Accelerometer::copyEvent(IOHIDEventType t,IOHIDEvent *m __unused,IOOptionBits o)
{
 if (t!=kIOHIDEventTypeAccelerometer) return NULL;
 x9_motion_sample s;if (x9_motion_copy_sample(&s)) return NULL;
 return makeEvent(s,o);
}
IOHIDEvent *X9Accelerometer::copyMatchingEvent(OSDictionary *m)
{
 if (!m) return NULL;
 OSNumber *t=OSDynamicCast(OSNumber,m->getObject(kIOHIDMatchingEventTypeKey));
 return t && t->unsigned64BitValue()==kIOHIDEventTypeAccelerometer?copyEvent(kIOHIDEventTypeAccelerometer,NULL,0):NULL;
}
void X9Accelerometer::poll(OSObject *o,IOTimerEventSource *timer)
{
 X9Accelerometer *self=(X9Accelerometer *)o;
 x9_motion_status st;x9_motion_copy_status(&st);
 if (st.error!=self->fError) {
  self->fError=st.error;self->setProperty("X9MotionErrno",(UInt32)st.error,32);
  IOLog("X9 MOTION: SCP errno=%d\n",st.error);
 }
 self->setProperty("X9MotionSCPEnabled",st.enabled?kOSBooleanTrue:kOSBooleanFalse);
 x9_motion_sample s;bool fresh=x9_motion_copy_sample(&s)==0;
 if (fresh!=self->fValid) {
  self->fValid=fresh;self->setProperty("X9MotionDataValid",fresh?kOSBooleanTrue:kOSBooleanFalse);
  IOLog("X9 MOTION: accelerometer %s\n",fresh?"valid":"stale/unavailable");
 }
 if (fresh && self->fIntervalUs && s.sequence!=self->fSequence) {
  IOHIDEvent *e=self->makeEvent(s,0);
  if (e) {
   self->fSequence=s.sequence;self->dispatchEvent(e);e->release();
   if (s.sequence==1) IOLog("X9 MOTION: DATA raw=%d/%d/%d fixed-g=%d/%d/%d seq=%llu\n",
       s.raw[0],s.raw[1],s.raw[2],s.fixed_g[0],s.fixed_g[1],s.fixed_g[2],s.sequence);
  }
 }
 timer->setTimeoutUS(self->fIntervalUs?self->fIntervalUs:20000);
}
IOReturn X9Accelerometer::configure(OSObject *o,void *arg,void *,void *,void *)
{
 X9Accelerometer *self=(X9Accelerometer *)o;OSDictionary *p=(OSDictionary *)arg;
 OSObject *v=p->getObject(kIOHIDReportIntervalKey);
 if (v) {
  OSNumber *n=OSDynamicCast(OSNumber,v);
  if (!n) return kIOReturnBadArgument;
  uint64_t us=n->unsigned64BitValue();
  if (us && (us<10000 || us>1000000)) return kIOReturnBadArgument;
  /* ReportInterval=0 is an original backboardd startup write. It suppresses
   * push events, NOT copyEvent or the iOS orientation bridge's physical stream.
   * Keep physical 50 Hz for that bridge; slower HID clients only decimate. */
  int e=x9_motion_set_interval(us && us<20000?(UInt32)us:20000);
  if (e) return kIOReturnNotReady;
  self->fIntervalUs=(UInt32)us;self->setProperty(kIOHIDReportIntervalKey,(UInt32)us,32);
  if (self->fTimer) self->fTimer->setTimeoutUS(us?(UInt32)us:20000);
  IOLog("X9 MOTION: iOS ReportInterval=%llu us (native cadence <=20ms)\n",us);
 }
 /* Shake thresholds are retained as client metadata only. Never synthesize
  * shake events without implementing/qualifying Apple's detection contract. */
 if (p->getObject("Shake")) self->setProperty("Shake",p->getObject("Shake"));
 return kIOReturnSuccess;
}
IOReturn X9Accelerometer::setProperties(OSObject *p)
{
 OSDictionary *d=OSDynamicCast(OSDictionary,p);
 return !d?kIOReturnBadArgument:_hidGate?_hidGate->runAction(&X9Accelerometer::configure,d):kIOReturnNotReady;
}
