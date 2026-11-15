/* 20H392 backboardd: BKHIDUISensorConfiguratorFactory::_proximityServiceWrapper
 * 0x100046f0c matches DeviceUsagePage=0xff00, DeviceUsage=8. Its block at
 * 0x100047134 requires Placement=1. Discrete wrapper writes DetectionMode
 * (0..5) at 0x10002bfd4. Proximity processor reads field0xe0000 and bit6
 * at 0x10002cd48/0x10002cdd8: Receiver MASK, not ambient-light/biometric.
 * Keep native esuc ABI and original userspace untouched. */
#include "../X9Proximity.h"
#include "../X9ALS.h"
#include <sys/errno.h>
#include "IOHIDEventServiceKeys_Private.h"
class X9Proximity : public IOHIDEventService {
 OSDeclareDefaultStructors(X9Proximity);
public:
 bool start(IOService *) APPLE_KEXT_OVERRIDE;
 void stop(IOService *) APPLE_KEXT_OVERRIDE;
 void free(void) APPLE_KEXT_OVERRIDE;
 UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return 0xff00; }
 UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return 8; }
 UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 0x6998; }
 UInt32 getReportInterval() APPLE_KEXT_OVERRIDE { return fIntervalUs; }
 IOHIDEvent *copyEvent(IOHIDEventType,IOHIDEvent *,IOOptionBits) APPLE_KEXT_OVERRIDE;
 IOHIDEvent *copyMatchingEvent(OSDictionary *) APPLE_KEXT_OVERRIDE;
 IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
private:
 static void poll(OSObject *,IOTimerEventSource *);
 static IOReturn configure(OSObject *,void *,void *,void *,void *);
 void removeTimer();
 IOHIDEvent *makeEvent(const x9_prox_sample &,IOOptionBits);
 IOTimerEventSource *fTimer;
 uint64_t fSequence;
 UInt32 fIntervalUs,fDetectionMode;
 bool fIngressActive,fValid,fReplay;
 int fError;
};
OSDefineMetaClassAndStructors(X9Proximity,IOHIDEventService);
bool X9Proximity::start(IOService *provider)
{
 int sensors=0,mode=0; PE_parse_boot_argn("x9s",&sensors,sizeof(sensors));
 mode=(sensors&8)?3:0; PE_parse_boot_argn("x9_prox",&mode,sizeof(mode));
 if (!sweet_early_is_x9() || mode<1 || mode>3) return false;
 fTimer=NULL; fSequence=0; fIntervalUs=200000; fDetectionMode=0;
 fIngressActive=fValid=fReplay=false; fError=0;
 if (!IOHIDEventService::start(provider)) return false;
 if (x9_prox_init(mode)) return false;
 fIngressActive=true;
 fTimer=IOTimerEventSource::timerEventSource(this,&X9Proximity::poll);
 if (!fTimer || _hidWorkLoop->addEventSource(fTimer)!=kIOReturnSuccess) {
  OSSafeReleaseNULL(fTimer); x9_prox_shutdown(); fIngressActive=false; return false;
 }
 setProperty("Product","X9 Front Proximity");
 setProperty("Placement",(UInt32)1,32);
 setProperty("DetectionMode",(UInt32)0,32);
 setProperty("X9ProxBackend",mode==2?"diagnostic":mode==3?"shared-scp-waiting":"mtk-ingress");
 setProperty("X9ProxDataValid",kOSBooleanFalse);
 setProperty("X9ProxPhysicalTransportReady",kOSBooleanFalse);
 setProperty("X9ProxDiagnostic",mode==2?kOSBooleanTrue:kOSBooleanFalse);
 /* Publish identity so iOS can request DetectionMode. This is NOT a claim
  * of available samples: copyEvent remains NULL until real DATA arrives. */
 if (mode==3) {
  int error=x9_als_start_shared_transport();
  if (error) { x9_prox_native_disconnect(error); fError=error; }
  setProperty("X9ProxErrno",(UInt32)error,32);
 }
 registerService(); fTimer->setTimeoutUS(fIntervalUs);
 IOLog("X9 PROX: HID registered usage=ff00/8 Placement=1 mode=%d; OFF awaiting DetectionMode/shared SCP (no fabricated far event)\n",mode);
 return true;
}
void X9Proximity::removeTimer()
{
 if (fTimer) {
  fTimer->cancelTimeout(); fTimer->disable();
  if (_hidWorkLoop) _hidWorkLoop->removeEventSource(fTimer);
  OSSafeReleaseNULL(fTimer);
 }
 if (fIngressActive) { x9_prox_shutdown(); fIngressActive=false; }
}
void X9Proximity::stop(IOService *p) { removeTimer(); IOHIDEventService::stop(p); }
void X9Proximity::free(void) { removeTimer(); IOHIDEventService::free(); }
IOHIDEvent *X9Proximity::makeEvent(const x9_prox_sample &s,IOOptionBits options)
{
 IOHIDEvent *event=IOHIDEvent::proximityEvent(s.host_time,
     s.near?kIOHIDProximityDetectionReceiver:0,(UInt32)0,options);
 if (event) event->setSenderID(getRegistryEntryID());
 return event;
}
IOHIDEvent *X9Proximity::copyEvent(IOHIDEventType type,IOHIDEvent *matching __unused,IOOptionBits options)
{
 if (type!=kIOHIDEventTypeProximity) return NULL;
 x9_prox_sample sample; if (x9_prox_copy_sample(&sample)) return NULL;
 return makeEvent(sample,options);
}
IOHIDEvent *X9Proximity::copyMatchingEvent(OSDictionary *matching)
{
 if (!matching) return NULL;
 OSNumber *type=OSDynamicCast(OSNumber,matching->getObject(kIOHIDMatchingEventTypeKey));
 if (!type || type->unsigned64BitValue()!=kIOHIDEventTypeProximity) return NULL;
 return copyEvent(kIOHIDEventTypeProximity,NULL,0);
}
void X9Proximity::poll(OSObject *owner,IOTimerEventSource *sender)
{
 X9Proximity *self=(X9Proximity *)owner;
 x9_prox_status status; x9_prox_copy_status(&status);
 self->setProperty("X9ProxSCPEnabled",status.enabled?kOSBooleanTrue:kOSBooleanFalse);
 self->setProperty("X9ProxControlPending",status.pending?kOSBooleanTrue:kOSBooleanFalse);
 self->setProperty("X9ProxSCPReady",status.ready?kOSBooleanTrue:kOSBooleanFalse);
 if (status.error!=self->fError) {
  self->fError=status.error; self->setProperty("X9ProxErrno",(UInt32)status.error,32);
  IOLog("X9 PROX: transport errno=%d; no synthetic screen state\n",status.error);
 }
 x9_prox_sample sample;
 bool fresh=x9_prox_copy_sample(&sample)==0;
 if (fresh!=self->fValid) {
  self->fValid=fresh; self->setProperty("X9ProxDataValid",fresh?kOSBooleanTrue:kOSBooleanFalse);
  if (!fresh) self->removeProperty("X9ProxNear");
 }
 self->setProperty("X9ProxPhysicalTransportReady",fresh && status.mode==3?kOSBooleanTrue:kOSBooleanFalse);
 if (!fresh) self->fReplay=false;
 if (fresh && (sample.sequence!=self->fSequence || self->fReplay)) {
  IOHIDEvent *event=self->makeEvent(sample,0);
  if (event) {
   self->fReplay=false;
   self->fSequence=sample.sequence;
   self->setProperty("X9ProxNear",sample.near?kOSBooleanTrue:kOSBooleanFalse);
   self->setProperty("X9ProxOneshot",(UInt32)sample.oneshot,32);
   self->setProperty("X9ProxSteps",(UInt32)sample.steps,32);
   self->setProperty("X9ProxSequence",sample.sequence,64);
   self->dispatchEvent(event); event->release();
   IOLog("X9 PROX: dispatch seq=%llu near=%u mask=%x oneshot=%d steps=%d source=%s\n",
       sample.sequence,sample.near,sample.near?0x40:0,sample.oneshot,sample.steps,
       sample.diagnostic?"DIAGNOSTIC":status.mode==3?"SCP":"external-ingress");
  }
 }
 sender->setTimeoutUS(self->fIntervalUs);
}
IOReturn X9Proximity::configure(OSObject *owner,void *arg0,void *,void *,void *)
{
 X9Proximity *self=(X9Proximity *)owner; OSDictionary *p=(OSDictionary *)arg0;
 OSObject *d=p->getObject("DetectionMode"),*i=p->getObject(kIOHIDReportIntervalKey);
 UInt32 mode=self->fDetectionMode,us=self->fIntervalUs;
 if (d) {
  OSNumber *n=OSDynamicCast(OSNumber,d);
  if (!n || n->unsigned64BitValue()>5) return kIOReturnBadArgument;
  mode=n->unsigned32BitValue();
 }
 if (i) {
  OSNumber *n=OSDynamicCast(OSNumber,i);
  if (!n || n->unsigned64BitValue()<20000 || n->unsigned64BitValue()>1000000) return kIOReturnBadArgument;
  us=n->unsigned32BitValue();
 }
 if (d || i) {
  int error=x9_prox_set_enabled(mode!=0,us);
  if (error && error!=EAGAIN) return error==EBUSY?kIOReturnBusy:kIOReturnNotReady;
  /* 20H392 backboardd clears its filtered/object state on passive/monitoring
   * -> active. All nonzero modes share our physical on-change stream, so
   * stable near need not produce a new DATA sequence. Re-arm one still-valid
   * snapshot on the next poll, retaining its original timestamp/provenance.
   * This is HID state replay, not another measurement or an ENABLE ACK.
   * Keep the request latched across same-mode/cadence writes and allocation
   * failure; OFF or unavailable backend DATA cancels it. */
  if (!mode) self->fReplay=false;
  else if (self->fDetectionMode && mode!=self->fDetectionMode) self->fReplay=true;
  self->fDetectionMode=mode; self->fIntervalUs=us;
  self->setProperty("DetectionMode",mode,32);
  self->setProperty(kIOHIDReportIntervalKey,us,32);
  IOLog("X9 PROX: iOS DetectionMode=%u interval=%u requested -> %d%s\n",mode,us,error,
      error==EAGAIN?" (deferred until shared SCP READY/LIST)":"");
 }
 /* Original iOS also writes these; retain metadata, don't reset calibration
  * or claim unknown factory configuration has reached the chip. */
 const char *keys[]={"ReceiverProximityReset","ScreenUnlocked"};
 for (unsigned k=0;k<2;k++) if (p->getObject(keys[k])) {
  self->setProperty(keys[k],p->getObject(keys[k]));
  IOLog("X9 PROX: iOS property %s (metadata only; no AP register writes)\n",keys[k]);
 }
 return kIOReturnSuccess;
}
IOReturn X9Proximity::setProperties(OSObject *properties)
{
 OSDictionary *p=OSDynamicCast(OSDictionary,properties);
 if (!p) return kIOReturnBadArgument;
 return _hidGate?_hidGate->runAction(&X9Proximity::configure,p):kIOReturnNotReady;
}
