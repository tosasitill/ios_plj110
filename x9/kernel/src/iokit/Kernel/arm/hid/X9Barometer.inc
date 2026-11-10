/* Native iOS 16 pressure delivery through the existing esuc HID queue.
 * 20H392 locationd callback 0x1004aaa0c tests type31, reads field0x1f0000,
 * converts to CLMotionTypePressure without rescaling. No Apple binary patch.
 * Do NOT publish availability until a real PRESSURE DATA record arrives,
 * except explicitly labelled x9_baro=2 diagnostics. */
#include "../X9Barometer.h"
#include <sys/errno.h>
#include "IOHIDEventServiceKeys_Private.h"

class X9Barometer : public IOHIDEventService {
	OSDeclareDefaultStructors(X9Barometer);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return kHIDPage_Sensor; }
	UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return kHIDUsage_Snsr_Environmental_AtmosphericPressure; }
	UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 0x6996; }
	UInt32 getReportInterval() APPLE_KEXT_OVERRIDE { return x9_baro_get_report_interval_us(); }
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
	bool fPublished;
};
OSDefineMetaClassAndStructors(X9Barometer, IOHIDEventService);

bool
X9Barometer::start(IOService *provider)
{
	int gb = 0; PE_parse_boot_argn("x9s", &gb, sizeof(gb));
	int enabled = (gb&2) ? 1 : 0;
	PE_parse_boot_argn("x9_baro", &enabled, sizeof(enabled));
	if (!sweet_early_is_x9() || (enabled != 1 && enabled != 2)) return false;
	/* One token fits the existing 607-byte boot line: 1=MTK, 2=diagnostic. */
	int diagnostic = enabled == 2;
	fIntervalUs = getReportInterval();
	if (!IOHIDEventService::start(provider)) return false;
	if (x9_baro_init(diagnostic == 1)) return false;
	fIntervalUs = getReportInterval();
	fIngressActive = true;
	fTimer = IOTimerEventSource::timerEventSource(this, &X9Barometer::poll);
	if (!fTimer || _hidWorkLoop->addEventSource(fTimer) != kIOReturnSuccess) {
		OSSafeReleaseNULL(fTimer);
		x9_baro_shutdown();
		fIngressActive = false;
		return false;
	}
	setProperty("X9BaroBackend", diagnostic == 1 ? "diagnostic" : "mtk-scp-pending");
	setProperty("X9BaroDataValid", kOSBooleanFalse);
	setProperty("X9BaroDiagnostic", diagnostic == 1 ? kOSBooleanTrue : kOSBooleanFalse);
	setProperty("X9BaroPhysicalTransportReady", kOSBooleanFalse);
	struct x9_scp_handoff layout;
	bool qualified = x9_baro_copy_scp_handoff(&layout) == 0;
	setProperty("X9SCPLiveLayoutQualified", qualified ? kOSBooleanTrue : kOSBooleanFalse);
	if (qualified) {
		setProperty("X9SCPDumpBytes", layout.dump_size, 32);
		setProperty("X9SCPRegionInfoSource", "TF-A GET_REGION_INFO; not SRAM+4");
	}
	if (diagnostic) { registerService(); fPublished = true; }
	fTimer->setTimeoutUS(fIntervalUs);
	IOLog("X9 BARO: HID initialized usage=0x20/0x31 interval=%u us mode=%s; waiting for SCP PRESSURE DATA (no physical enable)\n",
	    fIntervalUs, diagnostic == 1 ? "diagnostic" : "MTK ingress waiting");
	return true;
}

void
X9Barometer::removeTimer()
{
	if (fTimer) {
		fTimer->cancelTimeout();
		fTimer->disable();
		if (_hidWorkLoop) _hidWorkLoop->removeEventSource(fTimer);
		OSSafeReleaseNULL(fTimer);
	}
	if (fIngressActive) {
		x9_baro_shutdown();
		fIngressActive = false;
	}
}

void X9Barometer::stop(IOService *provider)
{
	removeTimer();
	IOHIDEventService::stop(provider);
}

void X9Barometer::free(void)
{
	removeTimer();
	IOHIDEventService::free();
}

IOHIDEvent *
X9Barometer::copyEvent(IOHIDEventType type, IOHIDEvent *matching __unused,
    IOOptionBits options)
{
	if (type != kIOHIDEventTypeAtmosphericPressure) return NULL;
	struct x9_baro_sample sample;
	if (x9_baro_copy_sample(&sample)) return NULL;
	/* Level is signed 16.16 kPa, not Pa and not integer hPa. */
	IOHIDEvent *event = IOHIDEvent::atmosphericPressureEvent(sample.host_time,
	    (IOFixed)sample.pressure_kpa_q16, (UInt32)sample.sequence, options);
	if (event) event->setSenderID(getRegistryEntryID());
	return event;
}

IOHIDEvent *
X9Barometer::copyMatchingEvent(OSDictionary *matching)
{
	if (!matching) return NULL;
	OSNumber *type = OSDynamicCast(OSNumber, matching->getObject(kIOHIDMatchingEventTypeKey));
	if (!type || type->unsigned64BitValue() != kIOHIDEventTypeAtmosphericPressure) return NULL;
	return copyEvent(kIOHIDEventTypeAtmosphericPressure, NULL, 0);
}

void
X9Barometer::poll(OSObject *owner, IOTimerEventSource *sender)
{
	X9Barometer *self = (X9Barometer *)owner;
	UInt32 appliedUs = self->getReportInterval();
	if (appliedUs != self->fIntervalUs) {
		self->fIntervalUs = appliedUs;
		self->setProperty(kIOHIDReportIntervalKey, appliedUs, 32);
	}
	struct x9_baro_sample sample;
	bool fresh = x9_baro_copy_sample(&sample) == 0;
	if (fresh && !self->fPublished) {
		self->fPublished = true; self->registerService();
	}
	if (fresh != self->fDataValid) {
		self->fDataValid = fresh;
		self->setProperty("X9BaroDataValid", fresh ? kOSBooleanTrue : kOSBooleanFalse);
		if (!fresh) {
			self->removeProperty("X9BaroLastPressureQ16");
			self->setProperty("X9BaroPhysicalTransportReady", kOSBooleanFalse);
		}
		IOLog("X9 BARO: sample %s\n", fresh ? "valid" : "invalid/stale");
	}
	if (fresh && sample.sequence != self->fSequence) {
		IOHIDEvent *event = IOHIDEvent::atmosphericPressureEvent(sample.host_time,
		    (IOFixed)sample.pressure_kpa_q16, (UInt32)sample.sequence, 0);
		if (event) {
			self->fSequence = sample.sequence;
			self->setProperty("X9BaroLastPressureQ16", sample.pressure_kpa_q16, 32);
			self->setProperty("X9BaroSequence", sample.sequence, 64);
			self->setProperty("X9BaroPhysicalTransportReady", sample.diagnostic ? kOSBooleanFalse : kOSBooleanTrue);
			self->setProperty("X9BaroBackend", sample.diagnostic ? "diagnostic" : "mtk-scp-data");
			self->dispatchEvent(event);
			event->release();
			/* Log first dispatch and diagnostics, not every physical sample. */
			if (sample.sequence == 1 || sample.diagnostic)
				IOLog("X9 BARO: dispatch seq=%llu pressure_q16=%u source=%s\n",
				    sample.sequence, sample.pressure_kpa_q16, sample.diagnostic ? "DIAGNOSTIC" : "MTK");
		}
	}
	sender->setTimeoutUS(self->fIntervalUs);
}

IOReturn
X9Barometer::configure(OSObject *owner, void *arg0, void *, void *, void *)
{
	X9Barometer *self = (X9Barometer *)owner;
	OSDictionary *properties = (OSDictionary *)arg0;
	OSObject *interval = properties->getObject(kIOHIDReportIntervalKey);
	if (interval) {
		OSNumber *number = OSDynamicCast(OSNumber, interval);
		if (!number) return kIOReturnBadArgument;
		uint64_t us = number->unsigned64BitValue();
		if (us < 20000 || us > 1000000) return kIOReturnBadArgument;
		int error = x9_baro_set_report_interval_us((UInt32)us);
		if (error == ENOTSUP) return kIOReturnUnsupported;
		if (error == EBUSY) return kIOReturnBusy;
		if (error) return kIOReturnNotReady;
		self->fIntervalUs = (UInt32)us;
		self->setProperty(kIOHIDReportIntervalKey, self->fIntervalUs, 32);
		if (self->fTimer) self->fTimer->setTimeoutUS(self->fIntervalUs);
	}
	return kIOReturnSuccess;
}

IOReturn
X9Barometer::setProperties(OSObject *properties)
{
	OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
	if (!dict) return kIOReturnBadArgument;
	if (!_hidGate) return kIOReturnNotReady;
	return _hidGate->runAction(&X9Barometer::configure, dict);
}
