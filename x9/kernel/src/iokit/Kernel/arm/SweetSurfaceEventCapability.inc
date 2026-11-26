// SPDX-License-Identifier: BSD-2-Clause
// Event capability and per-client read-only mappings. No selector is wired yet.

IOReturn
IOSurfaceRootUserClient::eventCapabilityCreate(uint64_t *out)
{
	out[0] = out[1] = 0;
	if (current_task() != fTask) return kIOReturnNotPermitted;
	if (fEventViews == NULL) return kIOReturnNoResources;
	SweetEventRight *right = SweetEventRight::make();
	if (right == NULL) return kIOReturnNoResources;
	uint64_t va = 0;
	IOLockLock(fLock);
	IOReturn result = right->attach(fEventViews, fTask, &va);
	if (result == kIOReturnSuccess) {
		mach_port_name_t name = 0;
		result = IOUserClient::copyPortNameForObjectInTask(fTask, right, &name);
		if (result == kIOReturnSuccess && (name == 0 || name == UINT32_MAX)) result = kIOReturnIPCError;
		if (result == kIOReturnSuccess) {
			out[0] = name;
			out[1] = va;
		}
	}
	IOLockUnlock(fLock);
	right->release(); // Unwinds the attached view on a failed publication.
	return result;
}

static SweetEventRight *
sweet_event_right_copy(task_t task, uint64_t name)
{
	if (current_task() != task || name == 0 || name >= UINT32_MAX) return NULL;
	OSObject *object = NULL;
	const IOReturn result = IOUserClient::copyObjectForPortNameInTask(task,
	    static_cast<mach_port_name_t>(name), &object);
	auto *right = result == kIOReturnSuccess ? OSDynamicCast(SweetEventRight, object) : NULL;
	if (right == NULL) OSSafeReleaseNULL(object);
	return right;
}

SweetSharedEventCounter *
IOSurfaceRootUserClient::eventCapabilityCopy(uint64_t name)
{
	SweetEventRight *right = sweet_event_right_copy(fTask, name);
	if (right == NULL) return NULL;
	SweetSharedEventCounter *event = right->event();
	event->retain();
	right->release();
	return event;
}

IOReturn
IOSurfaceRootUserClient::eventCapabilityImport(uint64_t name, uint64_t *out)
{
	out[0] = out[1] = out[2] = 0;
	SweetEventRight *right = sweet_event_right_copy(fTask, name);
	if (right == NULL) return kIOReturnBadArgument;
	IOLockLock(fLock);
	const IOReturn result = right->attach(fEventViews, fTask, &out[1]);
	if (result == kIOReturnSuccess) {
		out[0] = right->event()->value();
		out[2] = right->event()->traceID();
	}
	IOLockUnlock(fLock);
	right->release();
	return result;
}

void
IOSurfaceRootUserClient::eventMapsClose()
{
	IOLockLock(fLock);
	if (fEventViews != NULL) fEventViews->close();
	IOLockUnlock(fLock);
}
