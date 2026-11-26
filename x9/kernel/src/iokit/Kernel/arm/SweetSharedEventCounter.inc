// SPDX-License-Identifier: BSD-2-Clause
// Included by SweetSurface.cpp. No user client selector publishes this object yet.

class SweetSharedEventCounter : public OSObject {
	OSDeclareDefaultStructors(SweetSharedEventCounter);

	IOBufferMemoryDescriptor *fStorage = NULL;
	IOLock *fCounterLock = NULL;
	uint64_t *fCounter = NULL;
	bool fCharged = false;
	uint64_t fTraceID = 0;

public:
	static SweetSharedEventCounter *make();
	void free() APPLE_KEXT_OVERRIDE;
	// Return an owned map; the caller must retain the event until unmapping.
	IOMemoryMap *mapReadOnly(task_t task);
	uint64_t value() const;
	uint64_t traceID() const { return fTraceID; }
	bool advance(uint64_t next);
	IOReturn waitFor(uint64_t target, uint32_t timeoutMs);
};
OSDefineMetaClassAndStructors(SweetSharedEventCounter, OSObject);

static unsigned sweet_event_counter_objects;
static uint64_t sweet_event_trace_id;
static constexpr unsigned kSweetEventCounterLimit = 128;

SweetSharedEventCounter *
SweetSharedEventCounter::make()
{
	unsigned count = __atomic_load_n(&sweet_event_counter_objects, __ATOMIC_RELAXED);
	do {
		if (count >= kSweetEventCounterLimit) {
			return NULL;
		}
	} while (!__atomic_compare_exchange_n(&sweet_event_counter_objects, &count,
	    count + 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));

	SweetSharedEventCounter *event = OSTypeAlloc(SweetSharedEventCounter);
	if (event == NULL || !event->init()) {
		OSSafeReleaseNULL(event);
		__atomic_sub_fetch(&sweet_event_counter_objects, 1, __ATOMIC_RELAXED);
		return NULL;
	}
	event->fCharged = true;
	event->fCounterLock = IOLockAlloc();
	if (event->fCounterLock != NULL) {
		event->fStorage = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
		    kIODirectionInOut | kIOMemoryKernelUserShared, page_size, page_size);
	}
	void *bytes = event->fStorage == NULL ? NULL : event->fStorage->getBytesNoCopy();
	if (bytes == NULL) {
		event->release();
		return NULL;
	}
	// The mapping contains only this counter; never expose neighboring data.
	bzero(bytes, page_size);
	event->fCounter = static_cast<uint64_t *>(bytes);
	event->fTraceID = __atomic_add_fetch(&sweet_event_trace_id, 1, __ATOMIC_RELAXED);
	return event;
}

void
SweetSharedEventCounter::free()
{
	OSSafeReleaseNULL(fStorage);
	if (fCounterLock != NULL) {
		IOLockFree(fCounterLock);
	}
	if (fCharged) {
		__atomic_sub_fetch(&sweet_event_counter_objects, 1, __ATOMIC_RELAXED);
	}
	OSObject::free();
}

IOMemoryMap *
SweetSharedEventCounter::mapReadOnly(task_t task)
{
	if (task == NULL || task == kernel_task) {
		return NULL;
	}
	return fStorage->createMappingInTask(task, 0, kIOMapAnywhere | kIOMapReadOnly);
}

uint64_t
SweetSharedEventCounter::value() const
{
	return __atomic_load_n(fCounter, __ATOMIC_ACQUIRE);
}

bool
SweetSharedEventCounter::advance(uint64_t next)
{
	IOLockLock(fCounterLock);
	const bool advanced = next > value();
	if (advanced) {
		__atomic_store_n(fCounter, next, __ATOMIC_RELEASE);
		IOLockWakeup(fCounterLock, fCounter, false);
	}
	IOLockUnlock(fCounterLock);
	return advanced;
}

IOReturn
SweetSharedEventCounter::waitFor(uint64_t target, uint32_t timeoutMs)
{
	uint64_t deadline = 0;
	if (timeoutMs != 0) {
		clock_interval_to_deadline(timeoutMs, NSEC_PER_MSEC, &deadline);
	}
	IOLockLock(fCounterLock);
	IOReturn result = kIOReturnSuccess;
	while (value() < target) {
		if (timeoutMs == 0) {
			result = kIOReturnTimeout;
			break;
		}
		const int state = IOLockSleepDeadline(fCounterLock, fCounter, deadline, THREAD_ABORTSAFE);
		// Inspect the predicate again even when timeout and a signal coincide.
		if (value() >= target) {
			break;
		}
		if (state != THREAD_AWAKENED) {
			result = state == THREAD_TIMED_OUT ? kIOReturnTimeout : kIOReturnAborted;
			break;
		}
	}
	IOLockUnlock(fCounterLock);
	return result;
}
