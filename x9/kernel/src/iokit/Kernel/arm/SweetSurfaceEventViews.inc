// SPDX-License-Identifier: BSD-2-Clause

static unsigned sweet_event_map_count;
static constexpr unsigned kSweetEventMapLimit = 512;
static constexpr unsigned kSweetEventViewsPerObject = 64;

static bool
sweet_event_map_reserve()
{
	unsigned count = __atomic_load_n(&sweet_event_map_count, __ATOMIC_RELAXED);
	do {
		if (count >= kSweetEventMapLimit) return false;
	} while (!__atomic_compare_exchange_n(&sweet_event_map_count, &count, count + 1,
	    false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
	return true;
}

/* This owner has no task/client/reference back-pointer. The connection and
 * Mach-reference objects may retain it; neither is retained by the owner. */
class SweetEventViewOwner : public OSObject {
	OSDeclareDefaultStructors(SweetEventViewOwner);
	struct Record { SweetSharedEventCounter *event; IOMemoryMap *map; };
	Record views[kSweetEventViewsPerObject] = {};
	IOLock *lock = NULL;
	bool closed = false;
	static void drop(Record record);
public:
	static SweetEventViewOwner *make();
	IOReturn map(SweetSharedEventCounter *event, task_t task, uint64_t *va);
	void unmap(SweetSharedEventCounter *event);
	void close();
	void free() APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(SweetEventViewOwner, OSObject);

SweetEventViewOwner *
SweetEventViewOwner::make()
{
	auto *owner = OSTypeAlloc(SweetEventViewOwner);
	if (owner == NULL) return NULL;
	if (!owner->init() || (owner->lock = IOLockAlloc()) == NULL) {
		owner->release();
		return NULL;
	}
	return owner;
}

void
SweetEventViewOwner::drop(Record record)
{
	if (record.map != NULL) {
		record.map->release();
		__atomic_sub_fetch(&sweet_event_map_count, 1, __ATOMIC_RELAXED);
	}
	OSSafeReleaseNULL(record.event);
}

IOReturn
SweetEventViewOwner::map(SweetSharedEventCounter *event, task_t task, uint64_t *va)
{
	IOLockLock(lock);
	IOReturn result = kIOReturnNotOpen;
	Record *slot = NULL;
	if (!closed) {
		result = kIOReturnNoResources;
		for (auto &entry : views) {
			if (entry.event == event) {
				*va = entry.map->getAddress();
				result = kIOReturnSuccess;
				break;
			}
			if (entry.event == NULL && slot == NULL) slot = &entry;
		}
		if (result != kIOReturnSuccess && slot != NULL && sweet_event_map_reserve()) {
			IOMemoryMap *mapping = event->mapReadOnly(task);
			if (mapping == NULL || mapping->getAddress() == 0) {
				OSSafeReleaseNULL(mapping);
				__atomic_sub_fetch(&sweet_event_map_count, 1, __ATOMIC_RELAXED);
				result = kIOReturnNoMemory;
			} else {
				event->retain();
				*slot = {event, mapping};
				*va = mapping->getAddress();
				result = kIOReturnSuccess;
			}
		}
	}
	IOLockUnlock(lock);
	return result;
}

void
SweetEventViewOwner::unmap(SweetSharedEventCounter *event)
{
	Record detached = {};
	IOLockLock(lock);
	for (auto &entry : views) {
		if (entry.event == event) {
			detached = entry;
			entry = {};
			break;
		}
	}
	IOLockUnlock(lock);
	drop(detached);
}

void
SweetEventViewOwner::close()
{
	Record detached[kSweetEventViewsPerObject];
	IOLockLock(lock);
	closed = true;
	memcpy(detached, views, sizeof(detached));
	bzero(views, sizeof(views));
	IOLockUnlock(lock);
	for (auto &entry : detached) drop(entry);
}

void
SweetEventViewOwner::free()
{
	if (lock != NULL) {
		close();
		IOLockFree(lock);
	}
	OSObject::free();
}

/* Mach names identify this object, not the counter. Last-right cleanup
 * can detach its views even if the long-lived IOSurface connection stays open. */
class SweetEventRight : public OSObject {
	OSDeclareDefaultStructors(SweetEventRight);
	SweetSharedEventCounter *counter = NULL;
	SweetEventViewOwner *owners[kSweetEventViewsPerObject] = {};
	IOLock *lock = NULL;
public:
	static SweetEventRight *make();
	IOReturn attach(SweetEventViewOwner *owner, task_t task, uint64_t *va);
	SweetSharedEventCounter *event() const { return counter; }
	void free() APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(SweetEventRight, OSObject);

SweetEventRight *
SweetEventRight::make()
{
	auto *right = OSTypeAlloc(SweetEventRight);
	if (right == NULL) return NULL;
	if (!right->init() || (right->lock = IOLockAlloc()) == NULL ||
	    (right->counter = SweetSharedEventCounter::make()) == NULL) {
		right->release();
		return NULL;
	}
	return right;
}

IOReturn
SweetEventRight::attach(SweetEventViewOwner *owner, task_t task, uint64_t *va)
{
	if (owner == NULL) return kIOReturnNoResources;
	IOLockLock(lock);
	SweetEventViewOwner **slot = NULL;
	bool known = false;
	for (auto &entry : owners) {
		if (entry == owner) { known = true; break; }
		if (entry == NULL && slot == NULL) slot = &entry;
	}
	const IOReturn result = (known || slot != NULL) ? owner->map(counter, task, va) : kIOReturnNoResources;
	if (!known && result == kIOReturnSuccess) {
		owner->retain();
		*slot = owner;
	}
	IOLockUnlock(lock);
	return result;
}

void
SweetEventRight::free()
{
	/* Last reference: no attach can still be executing. Never hold a right
	 * lock while releasing an owner or touching the task map at teardown. */
	for (auto &owner : owners) {
		if (owner != NULL) {
			owner->unmap(counter);
			OSSafeReleaseNULL(owner);
		}
	}
	OSSafeReleaseNULL(counter);
	if (lock != NULL) IOLockFree(lock);
	OSObject::free();
}
