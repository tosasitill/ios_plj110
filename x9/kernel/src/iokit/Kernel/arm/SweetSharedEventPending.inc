// SPDX-License-Identifier: BSD-2-Clause
// Unwired. All advances of an attached counter MUST go through this object.
// A caller must retain this object throughout each method, including drainOne.
class SweetSharedEventPending : public OSObject {
	OSDeclareDefaultStructors(SweetSharedEventPending);
	struct Entry {
		SweetEventListener *listener;
		io_user_reference_t block, event;
		uint64_t target;
	};
	IOLock *fLock = NULL;
	SweetSharedEventCounter *fCounter = NULL;
	Entry fEntries[64] = {};
	bool fClosed = false;
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
	counter->retain();
	q->fCounter = counter;
	return q;
}

void
SweetSharedEventPending::free()
{
	if (fLock != NULL) { close(); IOLockFree(fLock); }
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
			listener->retain();
			fEntries[i] = {listener, block, event, target};
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
	const bool changed = !fClosed && fCounter->advance(next);
	IOLockUnlock(fLock);
	return changed;
}

unsigned
SweetSharedEventPending::cancelListener(SweetEventListener *listener)
{
	if (listener == NULL) return 0;
	SweetEventListener *released[64];
	unsigned n = 0;
	IOLockLock(fLock);
	for (unsigned i = 0; i < 64; ++i) if (fEntries[i].listener == listener) {
		released[n++] = fEntries[i].listener;
		fEntries[i] = {};
	}
	IOLockUnlock(fLock);
	for (unsigned i = 0; i < n; ++i) released[i]->release();
	return n;
}

void
SweetSharedEventPending::close()
{
	SweetEventListener *released[64];
	unsigned n = 0;
	IOLockLock(fLock);
	fClosed = true;
	for (unsigned i = 0; i < 64; ++i) if (fEntries[i].listener != NULL) {
		released[n++] = fEntries[i].listener;
		fEntries[i] = {};
	}
	IOLockUnlock(fLock);
	for (unsigned i = 0; i < n; ++i) released[i]->release();
}

bool
SweetSharedEventPending::drainOne(IOReturn *sendResult)
{
	Entry ready = {};
	IOLockLock(fLock);
	if (!fClosed) for (unsigned i = 0; i < 64; ++i) {
		if (fEntries[i].listener != NULL && fCounter->value() >= fEntries[i].target) {
			ready = fEntries[i];
			fEntries[i] = {};
			break;
		}
	}
	IOLockUnlock(fLock);
	if (ready.listener == NULL) return false;
	const IOReturn ret = IOSurfaceRootUserClient::eventListenerDeliver(
	    ready.listener, ready.block, ready.event, ready.target);
	ready.listener->release();
	if (sendResult != NULL) *sendResult = ret;
	return true;
}
