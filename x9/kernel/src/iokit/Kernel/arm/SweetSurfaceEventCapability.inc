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

IOReturn
IOSurfaceRootUserClient::eventExternalMethod(uint32_t selector, IOExternalMethodArguments *args)
{
	if (current_task() != fTask) return kIOReturnNotPermitted;
	const unsigned input = selector == 37 ? 2 : selector == 39 ? 5 : selector == 42 ? 4 : selector == 45 ? 3 :
	    (selector == 38 || selector == 41) ? 1 : 0;
	const unsigned output = selector == 36 ? 2 : selector == 38 ? 3 : 0;
	if (args->scalarInputCount != input || args->scalarOutputCount != output ||
	    (input && args->scalarInput == NULL) || (output && args->scalarOutput == NULL) ||
	    args->structureInputSize || args->structureOutputSize ||
	    args->structureInputDescriptor != NULL || args->structureOutputDescriptor != NULL ||
	    ((selector == 40) != (args->asyncWakePort != MACH_PORT_NULL))) {
		return kIOReturnBadArgument;
	}
	const uint64_t *in = args->scalarInput;
	switch (selector) {
	case 36: return eventCapabilityCreate(args->scalarOutput);
	case 38: return eventCapabilityImport(in[0], args->scalarOutput);
	case 40: return eventListenerAdd(args);
	case 41: return eventListenerRemove(in[0]);
	/* Surface-bound GPU ordering is not implemented. Never report signal
	 * success: QuartzCore's caller falls back on a nonzero result. */
	case 42: return kIOReturnUnsupported;
	case 37:
	case 39:
	case 45: break;
	default: return kIOReturnUnsupported;
	}
	SweetEventRight *right = sweet_event_right_copy(fTask, in[0]);
	if (right == NULL) return kIOReturnBadArgument;
	IOReturn result = kIOReturnSuccess;
	if (selector == 37) {
		right->queue()->advance(in[1]);
	} else if (selector == 45) {
		result = right->event()->waitFor(in[1], static_cast<uint32_t>(in[2]));
	} else {
		SweetEventListener *listener = eventListenerCopy(in[1]);
		if (listener == NULL) result = kIOReturnBadArgument;
		else {
			/* Serialize removal with registration. The worker takes sendLock
			 * only after releasing its queue lock. */
			IOLockLock(listener->sendLock);
			result = listener->canceled ? kIOReturnAborted :
			    right->queue()->subscribe(listener, in[3], in[2], in[4]);
			IOLockUnlock(listener->sendLock);
			listener->release();
		}
	}
	right->release();
	return result;
}
