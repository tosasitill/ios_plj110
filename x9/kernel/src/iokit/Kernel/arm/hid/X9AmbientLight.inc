/* iOS 16 IOHIDFamily-1915.140.3 ALS service. No BLControl wait bypass.
 * 20H392 CoreBrightness -[BLControl startHIDSystemClient] at 0x1af0a53bc
 * explicitly matches Sensor/AmbientLight (0x20/0x41); copyEvent asks for
 * type 12 at 0x1af054fbc. Use that path, not an invented Apple class name.
 * MT6993 physical transport remains separate: publishing a HID service is
 * not evidence that SCP or automatic panel brightness works. */
#include "../X9ALS.h"
#include <sys/errno.h>
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
	UInt32 getReportInterval() APPLE_KEXT_OVERRIDE { return x9_als_get_report_interval_us(); }
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
	struct x9_als_status fStatus;
};
OSDefineMetaClassAndStructors(X9AmbientLight, IOHIDEventService);

bool
X9AmbientLight::start(IOService *provider)
{
	fTimer = NULL;
	fIngressActive = false;
	/* Compact sensor mask: bit0 ALS, bit1 pressure, bit2 GPS. Explicit
	 * per-device arguments override it (including diagnostic mode). */
	int sensors = 0; int has_sensors;
	has_sensors = PE_parse_boot_argn("x9s", &sensors, sizeof(sensors)) ? 1 : 0;
	int enabled = (sensors&1) ? 1 : 0;
	if (!PE_parse_boot_argn("x9_als", &enabled, sizeof(enabled)) && !has_sensors)
		enabled = sweet_x9_arg("x9_als", 0);
	if (!sweet_early_is_x9() || (enabled != 1 && enabled != 2 && enabled != 3)) return false;
	/* One token: 1=external ingress, 2=diagnostic, 3=native secure SCP. */
	int diagnostic = enabled == 2;
	bzero(&fStatus, sizeof(fStatus));
	fDataValid = false;
	fSequence = 0;
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
	/* 319: SpringBoard also matches IONameMatch=als. This is still the
	 * Sensor/AmbientLight HID service, not a fake AppleALS user client. */
	setName("als");
	setProperty("X9ALSBackend", diagnostic ? "diagnostic" : enabled == 3 ? "mtk-scp-native" : "mtk-scp-pending");
	setProperty("X9ALSDataValid", kOSBooleanFalse);
	setProperty("X9ALSDiagnostic", diagnostic == 1 ? kOSBooleanTrue : kOSBooleanFalse);
	setProperty("X9ALSPhysicalTransportReady", kOSBooleanFalse);
	if (enabled == 3 && x9_als_start_transport()) {
		removeTimer();
		return false;
	}
	registerService();
	fTimer->setTimeoutUS(fIntervalUs);
	IOLog("X9 ALS: HID registered name=als usage=0x20/0x41 interval=%u us mode=%s; real READY/ENABLE/DATA required\n",
	    fIntervalUs, diagnostic ? "diagnostic" : enabled == 3 ? "native SCP startup" : "MTK ingress waiting");
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
	struct x9_als_status status;
	x9_als_copy_status(&status);
	if (status.stage != self->fStatus.stage || status.error != self->fStatus.error ||
	    status.control_enabled != self->fStatus.control_enabled || status.gain != self->fStatus.gain ||
	    status.firmware_ready != self->fStatus.firmware_ready || status.sensor_ready != self->fStatus.sensor_ready ||
	    status.power_requested != self->fStatus.power_requested || status.layout_valid != self->fStatus.layout_valid) {
		self->setProperty("X9ALSStage", status.stage, 32);
		self->setProperty("X9ALSLastErrno", (UInt32)status.error, 32);
		self->setProperty("X9ALSGain", status.gain, 32);
		self->setProperty("X9ALSLayoutValid", status.layout_valid ? kOSBooleanTrue : kOSBooleanFalse);
		self->setProperty("X9ALSPowerRequested", status.power_requested ? kOSBooleanTrue : kOSBooleanFalse);
		self->setProperty("X9ALSFirmwareReady", status.firmware_ready ? kOSBooleanTrue : kOSBooleanFalse);
		self->setProperty("X9ALSSensorReady", status.sensor_ready ? kOSBooleanTrue : kOSBooleanFalse);
		self->setProperty("X9ALSPhysicalTransportReady",
		    status.firmware_ready && status.sensor_ready && status.control_enabled && !status.error ? kOSBooleanTrue : kOSBooleanFalse);
	}
	self->fStatus = status;
	UInt32 appliedUs = self->getReportInterval();
	if (appliedUs != self->fIntervalUs) {
		self->fIntervalUs = appliedUs;
		self->setProperty(kIOHIDReportIntervalKey, appliedUs, 32);
	}
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
		if (us < 20000 || us > 1000000) return kIOReturnBadArgument;
		int error = x9_als_set_report_interval_us((UInt32)us);
		if (error == EBUSY) return kIOReturnBusy;
		if (error == ENXIO) return kIOReturnNotReady;
		if (error) return kIOReturnIOError;
		/* Native batch is committed only after the actual matching ACK.
		 * poll() adopts the applied cadence, not the queued request. */
		self->fIntervalUs = self->getReportInterval();
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
