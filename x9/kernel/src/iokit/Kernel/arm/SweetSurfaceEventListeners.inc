/* Real listener ownership helpers, not yet connected to external selectors. */
bool
SweetEventListener::initReference(IOExternalMethodArguments *args)
{
	sendLock = NULL;
	ownsRight = false;
	bzero(ref, sizeof(ref));
	if (!OSObject::init()) return false;
	if (args == NULL || args->asyncWakePort == MACH_PORT_NULL ||
	    args->asyncReference == NULL || args->asyncReferenceCount < 3 ||
	    args->asyncReferenceCount > kOSAsyncRef64Count) return false;
	/* The kernel-provided array is in kernel memory, not a user pointer. */
	memcpy(ref, args->asyncReference, args->asyncReferenceCount * sizeof(ref[0]));
	key = ref[kIOAsyncCalloutRefconIndex];
	sendLock = IOLockAlloc();
	return sendLock != NULL;
}

void
SweetEventListener::free()
{
	if (ownsRight) IOUserClient::releaseAsyncReference64(ref);
	if (sendLock != NULL) IOLockFree(sendLock);
	OSObject::free();
}

IOReturn
IOSurfaceRootUserClient::eventListenerAdd(IOExternalMethodArguments *args)
{
	if (args == NULL || args->asyncWakePort == MACH_PORT_NULL ||
	    args->asyncReference == NULL || args->asyncReferenceCount < 3 ||
	    args->asyncReferenceCount > kOSAsyncRef64Count) return kIOReturnBadArgument;
	SweetEventListener *listener = new SweetEventListener;
	if (listener == NULL) return kIOReturnNoMemory;
	if (!listener->initReference(args)) {
		listener->release(); /* MIG still owns the right on failure. */
		return kIOReturnNoMemory;
	}
	IOReturn result = kIOReturnNoResources;
	IOLockLock(fLock);
	if (fEventListenersClosed) result = kIOReturnNotOpen;
	else {
		unsigned freeIndex = 64;
		for (unsigned i = 0; i < 64; ++i) {
			if (fEventListeners[i] == NULL) {
				if (freeIndex == 64) freeIndex = i;
			} else if (fEventListeners[i]->key == listener->key) {
				result = kIOReturnBadArgument;
				freeIndex = 64;
				break;
			}
		}
		if (freeIndex < 64) {
			/* Successful MIG reply transfers this send right exactly once. */
			listener->ownsRight = true;
			fEventListeners[freeIndex] = listener;
			result = kIOReturnSuccess;
		}
	}
	IOLockUnlock(fLock);
	if (result != kIOReturnSuccess) listener->release();
	return result;
}

SweetEventListener *
IOSurfaceRootUserClient::eventListenerCopy(io_user_reference_t key)
{
	SweetEventListener *result = NULL;
	IOLockLock(fLock);
	if (!fEventListenersClosed) {
		for (unsigned i = 0; i < 64; ++i) {
			if (fEventListeners[i] != NULL && fEventListeners[i]->key == key) {
				result = fEventListeners[i];
				result->retain();
				break;
			}
		}
	}
	IOLockUnlock(fLock);
	return result;
}

IOReturn
IOSurfaceRootUserClient::eventListenerRemove(io_user_reference_t key)
{
	SweetEventListener *listener = NULL;
	IOLockLock(fLock);
	for (unsigned i = 0; i < 64; ++i) {
		if (fEventListeners[i] != NULL && fEventListeners[i]->key == key) {
			listener = fEventListeners[i];
			fEventListeners[i] = NULL;
			break;
		}
	}
	IOLockUnlock(fLock);
	if (listener == NULL) return kIOReturnBadArgument;
	listener->release();
	return kIOReturnSuccess;
}

void
IOSurfaceRootUserClient::eventListenersClose()
{
	SweetEventListener *removed[64];
	IOLockLock(fLock);
	fEventListenersClosed = true;
	memcpy(removed, fEventListeners, sizeof(removed));
	bzero(fEventListeners, sizeof(fEventListeners));
	IOLockUnlock(fLock);
	for (unsigned i = 0; i < 64; ++i) if (removed[i] != NULL) removed[i]->release();
}

IOReturn
IOSurfaceRootUserClient::eventListenerDeliver(SweetEventListener *listener,
    io_user_reference_t block, io_user_reference_t event, uint64_t value)
{
	if (listener == NULL || block == 0) return kIOReturnBadArgument;
	io_user_reference_t payload[4] = { block, event, value, value >> 32 };
	/* A retained listener keeps the right alive; serialize ref[0] error flags. */
	IOLockLock(listener->sendLock);
	IOReturn result = sendAsyncResult64(listener->ref, kIOReturnSuccess, payload, 4);
	IOLockUnlock(listener->sendLock);
	return result;
}
