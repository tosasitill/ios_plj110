/* iOS 16 IOHIDFamily-1915.140.3 ALS service. No BLControl wait bypass.
 * 20H392 CoreBrightness -[BLControl startHIDSystemClient] at 0x1af0a53bc
 * explicitly matches Sensor/AmbientLight (0x20/0x41); copyEvent asks for
 * type 12 at 0x1af054fbc. Use that path, not an invented Apple class name.
 * MT6993 physical transport remains separate: publishing a HID service is
 * not evidence that SCP or automatic panel brightness works. */
#include "../X9ALS.h"
#include "IOHIDEventServiceKeys_Private.h"

class X9AmbientLight : public IOHIDEventService {
	OSDeclareDefaultStructors(X9AmbientLight);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return kHIDPage_Sensor; }
	UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return kHIDUsage_Snsr_Light_AmbientLight; }
	UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 0x6993; }
	UInt32 getReportInterval() APPLE_KEXT_OVERRIDE { return 200000; }
	IOHIDEvent *copyEvent(IOHIDEventType type, IOHIDEvent *matching,
	    IOOptionBits options) APPLE_KEXT_OVERRIDE;
	IOHIDEvent *copyMatchingEvent(OSDictionary *matching) APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
private:
	static void poll(OSObject *owner, IOTimerEventSource *sender);
	static IOReturn configure(OSObject *owner, void *arg0, void *, void *, void *);
	void removeTimer();
	IOTimerEventSource *fTimer;
	UInt32 fIntervalUs;
	uint64_t fSequence;
	bool fIngressActive;
	bool fDataValid;
};
OSDefineMetaClassAndStructors(X9AmbientLight, IOHIDEventService);

bool
X9AmbientLight::start(IOService *provider)
{
	/* Compact sensor mask: bit0 ALS, bit1 pressure, bit2 GPS. Explicit
	 * per-device arguments override it (including diagnostic mode). */
	int sensors = 0; PE_parse_boot_argn("x9s", &sensors, sizeof(sensors));
	int enabled = (sensors&1) ? 1 : 0;
	PE_parse_boot_argn("x9_als", &enabled, sizeof(enabled));
	if (!sweet_early_is_x9() || (enabled != 1 && enabled != 2)) return false;
	/* One token fits the existing 607-byte boot line: 1=MTK, 2=diagnostic. */
	int diagnostic = enabled == 2;
	fIntervalUs = getReportInterval();
	if (!IOHIDEventService::start(provider)) return false;
	if (x9_als_init(diagnostic == 1)) return false;
	fIngressActive = true;
	fTimer = IOTimerEventSource::timerEventSource(this, &X9AmbientLight::poll);
	if (!fTimer || _hidWorkLoop->addEventSource(fTimer) != kIOReturnSuccess) {
		OSSafeReleaseNULL(fTimer);
		x9_als_shutdown();
		fIngressActive = false;
		return false;
	}
	setProperty("X9ALSBackend", diagnostic == 1 ? "diagnostic" : "mtk-scp-pending");
	setProperty("X9ALSDataValid", kOSBooleanFalse);
	setProperty("X9ALSDiagnostic", diagnostic == 1 ? kOSBooleanTrue : kOSBooleanFalse);
	setProperty("X9ALSPhysicalTransportReady", kOSBooleanFalse);
	registerService();
	fTimer->setTimeoutUS(fIntervalUs);
	IOLog("X9 ALS: HID registered usage=0x20/0x41 interval=%u us mode=%s; physical SCP transport NOT enabled\n",
	    fIntervalUs, diagnostic == 1 ? "diagnostic" : "MTK ingress waiting");
	return true;
}

void
X9AmbientLight::removeTimer()
{
	if (fTimer) {
		fTimer->cancelTimeout();
		fTimer->disable();
		if (_hidWorkLoop) _hidWorkLoop->removeEventSource(fTimer);
		OSSafeReleaseNULL(fTimer);
	}
	if (fIngressActive) {
		x9_als_shutdown();
		fIngressActive = false;
	}
}

void X9AmbientLight::stop(IOService *provider)
{
	removeTimer();
	IOHIDEventService::stop(provider);
}

void X9AmbientLight::free(void)
{
	removeTimer();
	IOHIDEventService::free();
}

IOHIDEvent *
X9AmbientLight::copyEvent(IOHIDEventType type, IOHIDEvent *matching __unused,
    IOOptionBits options)
{
	if (type != kIOHIDEventTypeAmbientLightSensor) return NULL;
	struct x9_als_sample sample;
	if (x9_als_copy_sample(&sample)) return NULL;
	/* The SCP backend currently supplies calibrated lux only, not raw color
	 * channels or CCT. Leave them zero rather than synthesizing True Tone. */
	IOHIDEvent *event = IOHIDEvent::ambientLightSensorEvent(sample.host_time,
	    sample.lux, (UInt32)0, (UInt32)0, (UInt32)0, (UInt32)0, options);
	if (event) event->setSenderID(getRegistryEntryID());
	return event;
}

IOHIDEvent *
X9AmbientLight::copyMatchingEvent(OSDictionary *matching)
{
	if (!matching) return NULL;
	OSNumber *type = OSDynamicCast(OSNumber, matching->getObject(kIOHIDMatchingEventTypeKey));
	if (!type || type->unsigned64BitValue() != kIOHIDEventTypeAmbientLightSensor) return NULL;
	return copyEvent(kIOHIDEventTypeAmbientLightSensor, NULL, 0);
}

void
X9AmbientLight::poll(OSObject *owner, IOTimerEventSource *sender)
{
	X9AmbientLight *self = (X9AmbientLight *)owner;
	struct x9_als_sample sample;
	bool fresh = x9_als_copy_sample(&sample) == 0;
	if (fresh != self->fDataValid) {
		self->fDataValid = fresh;
		self->setProperty("X9ALSDataValid", fresh ? kOSBooleanTrue : kOSBooleanFalse);
		if (!fresh) self->removeProperty("X9ALSLastLux");
		IOLog("X9 ALS: sample %s\n", fresh ? "valid" : "invalid/stale");
	}
	if (fresh && sample.sequence != self->fSequence) {
		IOHIDEvent *event = IOHIDEvent::ambientLightSensorEvent(sample.host_time,
		    sample.lux, (UInt32)0, (UInt32)0, (UInt32)0, (UInt32)0, 0);
		if (event) {
			self->fSequence = sample.sequence;
			self->setProperty("X9ALSLastLux", sample.lux, 32);
			self->setProperty("X9ALSSequence", sample.sequence, 64);
			self->setProperty("X9ALSBackend", sample.diagnostic ? "diagnostic" : "mtk-scp-data");
			self->dispatchEvent(event);
			event->release();
			/* Log first dispatch and diagnostics, not every physical sample. */
			if (sample.sequence == 1 || sample.diagnostic)
				IOLog("X9 ALS: dispatch seq=%llu lux=%u source=%s\n",
				    sample.sequence, sample.lux, sample.diagnostic ? "DIAGNOSTIC" : "MTK");
		}
	}
	sender->setTimeoutUS(self->fIntervalUs);
}

IOReturn
X9AmbientLight::configure(OSObject *owner, void *arg0, void *, void *, void *)
{
	X9AmbientLight *self = (X9AmbientLight *)owner;
	OSDictionary *properties = (OSDictionary *)arg0;
	OSObject *interval = properties->getObject(kIOHIDReportIntervalKey);
	if (interval) {
		OSNumber *number = OSDynamicCast(OSNumber, interval);
		if (!number) return kIOReturnBadArgument;
		uint64_t us = number->unsigned64BitValue();
		/* Limit HID delivery polling, not the as-yet-unimplemented SCP batch. */
		if (us < 20000 || us > 1000000) return kIOReturnBadArgument;
		self->fIntervalUs = (UInt32)us;
		self->setProperty(kIOHIDReportIntervalKey, self->fIntervalUs, 32);
		if (self->fTimer) self->fTimer->setTimeoutUS(self->fIntervalUs);
	}
	return kIOReturnSuccess;
}

IOReturn
X9AmbientLight::setProperties(OSObject *properties)
{
	OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
	if (!dict) return kIOReturnBadArgument;
	if (!_hidGate) return kIOReturnNotReady;
	return _hidGate->runAction(&X9AmbientLight::configure, dict);
}
