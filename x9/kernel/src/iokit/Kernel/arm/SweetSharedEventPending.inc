// SPDX-License-Identifier: BSD-2-Clause
// All advances of an attached counter MUST go through this object.
// A caller must retain this object throughout each method, including drainOne.
class SweetSharedEventPending : public OSObject {
	OSDeclareDefaultStructors(SweetSharedEventPending);
	struct Entry {
		SweetEventListener *listener;
		io_user_reference_t block, event;
		uint64_t target, sequence;
		bool inFlight, canceled;
	};
	IOLock *fLock = NULL;
	SweetSharedEventCounter *fCounter = NULL;
	Entry fEntries[64] = {};
	uint64_t fNextSequence = 0;
	bool fClosed = false;
	thread_call_t fWorker = NULL;
	static void worker(thread_call_param_t arg, thread_call_param_t);
	void scheduleLocked(bool delayed);
	bool drainOneEx(IOReturn *sendResult, SweetEventListener *const *blocked,
	    unsigned blockedCount, SweetEventListener **timedOutListener);
public:
	static SweetSharedEventPending *make(SweetSharedEventCounter *counter);
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn subscribe(SweetEventListener *listener, io_user_reference_t block,
	    io_user_reference_t event, uint64_t target);
	bool advance(uint64_t next);
	unsigned cancelListener(SweetEventListener *listener);
	void close();
	// Claims one ready entry. A claimed entry cannot be canceled; send is outside fLock.
	bool drainOne(IOReturn *sendResult);
};
OSDefineMetaClassAndStructors(SweetSharedEventPending, OSObject);

SweetSharedEventPending *
SweetSharedEventPending::make(SweetSharedEventCounter *counter)
{
	if (counter == NULL) return NULL;
	SweetSharedEventPending *q = OSTypeAlloc(SweetSharedEventPending);
	if (q == NULL || !q->init()) { OSSafeReleaseNULL(q); return NULL; }
	q->fLock = IOLockAlloc();
	if (q->fLock == NULL) { q->release(); return NULL; }
	q->fWorker = thread_call_allocate_with_options(&SweetSharedEventPending::worker, q,
	    THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	if (q->fWorker == NULL) { q->release(); return NULL; }
	counter->retain();
	q->fCounter = counter;
	return q;
}

void
SweetSharedEventPending::free()
{
	if (fLock != NULL) { close(); IOLockFree(fLock); }
	if (fWorker != NULL) thread_call_free(fWorker);
	OSSafeReleaseNULL(fCounter);
	OSObject::free();
}

IOReturn
SweetSharedEventPending::subscribe(SweetEventListener *listener,
    io_user_reference_t block, io_user_reference_t event, uint64_t target)
{
	if (listener == NULL || block == 0) return kIOReturnBadArgument;
	IOReturn ret = kIOReturnNoResources;
	IOLockLock(fLock);
	if (fClosed) ret = kIOReturnNotOpen;
	else for (unsigned i = 0; i < 64; ++i) {
		if (fEntries[i].listener == NULL) {
			// Never wrap sequence while old entries may still exist.
			if (fNextSequence == UINT64_MAX) break;
			listener->retain();
			fEntries[i] = {listener, block, event, target, ++fNextSequence, false, false};
			if (fCounter->value() >= target) scheduleLocked(false);
			ret = kIOReturnSuccess;
			break;
		}
	}
	IOLockUnlock(fLock);
	return ret;
}

bool
SweetSharedEventPending::advance(uint64_t next)
{
	IOLockLock(fLock);
	const uint64_t previous = fCounter->value();
	const bool changed = !fClosed && fCounter->advance(next);
	if (changed) {
		// Already-ready entries were scheduled by subscribe, a previous
		// advance, or the worker's delayed retry. Leave that backoff intact.
		for (const auto &entry : fEntries) {
			if (entry.listener != NULL && !entry.canceled &&
			    previous < entry.target && entry.target <= next) {
				scheduleLocked(false);
				break;
			}
		}
	}
	IOLockUnlock(fLock);
	return changed;
}

unsigned
SweetSharedEventPending::cancelListener(SweetEventListener *listener)
{
	if (listener == NULL) return 0;
	SweetEventListener *released[64];
	unsigned n = 0, canceled = 0;
	IOLockLock(fLock);
	for (unsigned i = 0; i < 64; ++i) if (fEntries[i].listener == listener && !fEntries[i].canceled) {
		++canceled;
		if (fEntries[i].inFlight) fEntries[i].canceled = true;
		else { released[n++] = fEntries[i].listener; fEntries[i] = {}; }
	}
	IOLockUnlock(fLock);
	for (unsigned i = 0; i < n; ++i) released[i]->release();
	return canceled;
}

void
SweetSharedEventPending::close()
{
	SweetEventListener *released[64];
	unsigned n = 0;
	IOLockLock(fLock);
	fClosed = true;
	IOLockUnlock(fLock);
	// Owner serializes close/free and stops external calls before teardown.
	if (fWorker != NULL) thread_call_cancel_wait(fWorker);
	IOLockLock(fLock);
	for (unsigned i = 0; i < 64; ++i) if (fEntries[i].listener != NULL) {
		released[n++] = fEntries[i].listener;
		fEntries[i] = {};
	}
	IOLockUnlock(fLock);
	for (unsigned i = 0; i < n; ++i) released[i]->release();
}

void
SweetSharedEventPending::scheduleLocked(bool delayed)
{
	if (fClosed || fWorker == NULL) return;
	if (!delayed) thread_call_enter(fWorker);
	else {
		uint64_t deadline;
		clock_interval_to_deadline(10, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(fWorker, deadline);
	}
}

void
SweetSharedEventPending::worker(thread_call_param_t arg, __unused thread_call_param_t ignored)
{
	auto *self = static_cast<SweetSharedEventPending *>(arg);
	SweetEventListener *blocked[64] = {};
	unsigned blockedCount = 0, attempts = 0;
	IOReturn result;
	SweetEventListener *timedOut = NULL;
	while (attempts < 64 && self->drainOneEx(&result, blocked, blockedCount, &timedOut)) {
		++attempts;
		if (timedOut != NULL) {
			blocked[blockedCount++] = timedOut; // retained until this pass ends
			if (blockedCount == 64) break;
		}
	}
	if (blockedCount != 0 || attempts == 64) {
		IOLockLock(self->fLock);
		self->scheduleLocked(blockedCount != 0);
		IOLockUnlock(self->fLock);
	}
	for (unsigned i = 0; i < blockedCount; ++i) blocked[i]->release();
}

bool
SweetSharedEventPending::drainOne(IOReturn *sendResult)
{
	return drainOneEx(sendResult, NULL, 0, NULL);
}

bool
SweetSharedEventPending::drainOneEx(IOReturn *sendResult,
    SweetEventListener *const *blocked, unsigned blockedCount,
    SweetEventListener **timedOutListener)
{
	Entry ready = {};
	unsigned index = 64;
	if (timedOutListener != NULL) *timedOutListener = NULL;
	IOLockLock(fLock);
	if (!fClosed) for (unsigned i = 0; i < 64; ++i) {
		Entry &candidate = fEntries[i];
		if (candidate.listener == NULL || candidate.inFlight || candidate.canceled ||
		    fCounter->value() < candidate.target) continue;
		bool skip = false;
		for (unsigned j = 0; j < blockedCount; ++j)
			if (blocked[j] == candidate.listener) { skip = true; break; }
		// Earlier ready notifications for this listener (including in-flight)
		// must complete first, even when slots have been reused out of order.
		for (unsigned j = 0; !skip && j < 64; ++j) {
			Entry &other = fEntries[j];
			if (other.listener == candidate.listener && !other.canceled &&
			    other.sequence < candidate.sequence && fCounter->value() >= other.target)
				skip = true;
		}
		if (!skip && (index == 64 || candidate.sequence < fEntries[index].sequence))
			index = i;
	}
	if (index != 64) { fEntries[index].inFlight = true; ready = fEntries[index]; }
	IOLockUnlock(fLock);
	if (index == 64) return false;
	const IOReturn ret = IOSurfaceRootUserClient::eventListenerDeliver(
	    ready.listener, ready.block, ready.event, ready.target);
	bool release = false;
	IOLockLock(fLock);
	if (ret != MACH_SEND_TIMED_OUT || fClosed || fEntries[index].canceled) {
		fEntries[index] = {};
		release = true;
	} else {
		fEntries[index].inFlight = false;
		if (timedOutListener != NULL) {
			ready.listener->retain();
			*timedOutListener = ready.listener;
		}
	}
	IOLockUnlock(fLock);
	if (release) ready.listener->release();
	if (ret != kIOReturnSuccess && ret != MACH_SEND_TIMED_OUT)
		IOLog("SWEET SURF EVENT: async send failed 0x%x\n", ret);
	if (sendResult != NULL) *sendResult = ret;
	return true;
}
