// SPDX-License-Identifier: BSD-2-Clause
// Event capability and per-client read-only mappings. No selector is wired yet.
static unsigned sweet_event_map_count;
static constexpr unsigned kSweetEventMapLimit = 512;

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

void
IOSurfaceRootUserClient::eventMapDrop(EventMap &record)
{
	if (record.map != NULL) {
		record.map->release();
		__atomic_sub_fetch(&sweet_event_map_count, 1, __ATOMIC_RELAXED);
	}
	OSSafeReleaseNULL(record.event);
	record = {};
}

/* fLock held, and caller retains the event independently. */
IOReturn
IOSurfaceRootUserClient::eventMapFindOrCreate(SweetSharedEventCounter *event, EventMap **out, bool *created)
{
	*out = NULL;
	*created = false;
	if (fEventMapsClosed) return kIOReturnNotOpen;
	EventMap *slot = NULL;
	for (auto &record : fEventMaps) {
		if (record.event == event) {
			*out = &record;
			return kIOReturnSuccess;
		}
		if (slot == NULL && record.event == NULL) slot = &record;
	}
	if (slot == NULL || !sweet_event_map_reserve()) return kIOReturnNoResources;
	IOMemoryMap *map = event->mapReadOnly(fTask);
	if (map == NULL || map->getAddress() == 0) {
		OSSafeReleaseNULL(map);
		__atomic_sub_fetch(&sweet_event_map_count, 1, __ATOMIC_RELAXED);
		return kIOReturnNoMemory;
	}
	event->retain();
	*slot = {event, map};
	*out = slot;
	*created = true;
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::eventCapabilityCreate(uint64_t *out)
{
	out[0] = out[1] = 0;
	if (current_task() != fTask) return kIOReturnNotPermitted;
	SweetSharedEventCounter *event = SweetSharedEventCounter::make();
	if (event == NULL) return kIOReturnNoResources;
	EventMap *record;
	bool created;
	EventMap rollback = {};
	IOLockLock(fLock);
	IOReturn result = eventMapFindOrCreate(event, &record, &created);
	if (result == kIOReturnSuccess) {
		mach_port_name_t name = 0;
		result = IOUserClient::copyPortNameForObjectInTask(fTask, event, &name);
		if (result == kIOReturnSuccess && (name == 0 || name == UINT32_MAX)) result = kIOReturnIPCError;
		if (result == kIOReturnSuccess) {
			out[0] = name;
			out[1] = record->map->getAddress();
		} else if (created) {
			rollback = *record;
			*record = {};
		}
	}
	IOLockUnlock(fLock);
	eventMapDrop(rollback);
	event->release();
	return result;
}

/* Retained kernel object; a plain integer, wrong class or foreign name is not authority. */
SweetSharedEventCounter *
IOSurfaceRootUserClient::eventCapabilityCopy(uint64_t name)
{
	if (current_task() != fTask || name == 0 || name >= UINT32_MAX) return NULL;
	OSObject *object = NULL;
	const IOReturn result = IOUserClient::copyObjectForPortNameInTask(fTask,
	    static_cast<mach_port_name_t>(name), &object);
	auto *event = result == kIOReturnSuccess ? OSDynamicCast(SweetSharedEventCounter, object) : NULL;
	if (event == NULL) OSSafeReleaseNULL(object);
	return event;
}

IOReturn
IOSurfaceRootUserClient::eventCapabilityImport(uint64_t name, uint64_t *out)
{
	out[0] = out[1] = out[2] = 0;
	SweetSharedEventCounter *event = eventCapabilityCopy(name);
	if (event == NULL) return kIOReturnBadArgument;
	EventMap *record;
	bool created;
	IOLockLock(fLock);
	const IOReturn result = eventMapFindOrCreate(event, &record, &created);
	if (result == kIOReturnSuccess) {
		out[0] = event->value();
		out[1] = record->map->getAddress();
		out[2] = event->traceID();
	}
	IOLockUnlock(fLock);
	event->release();
	return result;
}

void
IOSurfaceRootUserClient::eventMapsClose()
{
	EventMap detached[64];
	IOLockLock(fLock);
	fEventMapsClosed = true;
	memcpy(detached, fEventMaps, sizeof(detached));
	bzero(fEventMaps, sizeof(fEventMaps));
	IOLockUnlock(fLock);
	for (auto &record : detached) eventMapDrop(record);
}
