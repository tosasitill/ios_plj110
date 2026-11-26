// Exercise actual capability bookkeeping; simulated task ports and maps only.
#define main shared_counter_host
#include "shared_event_counter_host.cpp"
#undef main

#include <map>
#include <utility>
#define OSDynamicCast(cls, obj) dynamic_cast<cls *>(obj)
using mach_port_name_t = uint32_t;
static constexpr IOReturn kIOReturnNoResources = 10, kIOReturnNoMemory = 11,
    kIOReturnBadArgument = 12, kIOReturnNotOpen = 13, kIOReturnIPCError = 14,
    kIOReturnNotPermitted = 15, kIOReturnUnsupported = 16;

static task_t caller;
static task_t current_task() { return caller; }
struct IOUserClient {
	static std::map<std::pair<task_t, mach_port_name_t>, OSObject *> rights;
	static bool failExport;
	static IOReturn copyPortNameForObjectInTask(task_t task, OSObject *object, mach_port_name_t *out)
	{
		*out = failExport ? 0 : 42;
		if (*out) {
			object->retain();
			assert(rights.emplace(std::make_pair(task, *out), object).second);
		}
		return kIOReturnSuccess;
	}
	static IOReturn copyObjectForPortNameInTask(task_t task, mach_port_name_t name, OSObject **out)
	{
		const auto it = rights.find({task, name});
		if (it == rights.end()) { *out = nullptr; return kIOReturnIPCError; }
		*out = it->second;
		(*out)->retain();
		return kIOReturnSuccess;
	}
};
std::map<std::pair<task_t, mach_port_name_t>, OSObject *> IOUserClient::rights;
bool IOUserClient::failExport;

static IOLock surfaceLock;
static IOLock *sweet_surface_lock = &surfaceLock;
using io_user_reference_t = uint64_t;
static constexpr uintptr_t MACH_PORT_NULL = 0;
struct SweetEventListener : OSObject {
	IOLock *sendLock = IOLockAlloc();
	bool canceled = false;
	void free() override { IOLockFree(sendLock); }
};
struct IOExternalMethodArguments {
	unsigned scalarInputCount = 0, scalarOutputCount = 0;
	const uint64_t *scalarInput = nullptr;
	uint64_t *scalarOutput = nullptr;
	unsigned structureInputSize = 0, structureOutputSize = 0;
	void *structureInputDescriptor = nullptr, *structureOutputDescriptor = nullptr;
	uintptr_t asyncWakePort = 0;
};
// The pending worker has its own fixture; here only capture wired calls.
struct SweetSharedEventPending : OSObject {
	SweetSharedEventCounter *event;
	unsigned queued = 0;
	static SweetSharedEventPending *make(SweetSharedEventCounter *event) {
		auto *q = new SweetSharedEventPending; q->event = event; return q;
	}
	void close() { queued = 0; }
	unsigned cancelListener(SweetEventListener *) { unsigned n = queued; queued = 0; return n; }
	bool advance(uint64_t value) { return event->advance(value); }
	IOReturn subscribe(SweetEventListener *, uint64_t block, uint64_t context, uint64_t threshold) {
		assert(block == 7 && context == 6 && threshold == 12);
		++queued;
		return kIOReturnSuccess;
	}
};
#include "../../kernel/src/iokit/Kernel/arm/SweetSurfaceEventViews.inc"

struct IOSurfaceRootUserClient {
	task_t fTask;
	IOLock *fLock;
	SweetEventViewOwner *fEventViews = SweetEventViewOwner::make();
	~IOSurfaceRootUserClient() { eventMapsClose(); fEventViews->release(); }
	IOReturn eventCapabilityCreate(uint64_t *);
	SweetSharedEventCounter *eventCapabilityCopy(uint64_t);
	IOReturn eventCapabilityImport(uint64_t, uint64_t *);
	void eventMapsClose();
	IOReturn eventExternalMethod(uint32_t, IOExternalMethodArguments *);
	SweetEventListener *listener = nullptr;
	IOReturn eventListenerAdd(IOExternalMethodArguments *) { return kIOReturnUnsupported; }
	IOReturn eventListenerRemove(io_user_reference_t key) {
		if (key != 99 || listener == nullptr) return kIOReturnBadArgument;
		sweet_event_cancel_listener(listener);
		listener->release();
		listener = nullptr;
		return kIOReturnSuccess;
	}
	SweetEventListener *eventListenerCopy(io_user_reference_t key) {
		if (key != 99 || listener == nullptr) return nullptr;
		listener->retain(); return listener;
	}
};

#include "../../kernel/src/iokit/Kernel/arm/SweetSurfaceEventCapability.inc"

int main()
{
	int taskA, taskB;
	IOLock lockA, lockB;
	IOSurfaceRootUserClient a{&taskA, &lockA}, b{&taskB, &lockB};
	uint64_t out[3] = {};
	caller = &taskA;
	IOUserClient::failExport = true;
	assert(a.eventCapabilityCreate(out) == kIOReturnIPCError);
	assert(!out[0] && !out[1] && !sweet_event_map_count && !sweet_event_counter_objects);
	IOUserClient::failExport = false;
	IOExternalMethodArguments create{};
	create.scalarOutputCount = 1;
	create.scalarOutput = out;
	assert(a.eventExternalMethod(36, &create) == kIOReturnBadArgument);
	create.scalarOutputCount = 2;
	assert(a.eventExternalMethod(36, &create) == kIOReturnSuccess);
	const uint64_t name = out[0], va = out[1];
	assert(name == 42 && va && sweet_event_map_count == 1);
	auto *event = a.eventCapabilityCopy(name);
	assert(event);
	uint64_t input[5] = {name, 7};
	IOExternalMethodArguments call{};
	call.scalarInput = input; call.scalarInputCount = 2;
	assert(a.eventExternalMethod(37, &call) == kIOReturnSuccess && event->value() == 7);
	call.scalarInputCount = 3;
	input[1] = 8;
	assert(a.eventExternalMethod(45, &call) == kIOReturnTimeout);
	a.listener = new SweetEventListener;
	input[1] = 99; input[2] = 6; input[3] = 7; input[4] = 12;
	call.scalarInputCount = 5;
	assert(a.eventExternalMethod(39, &call) == kIOReturnSuccess && sweet_event_queues[0]->queued == 1);
	input[0] = 99; call.scalarInputCount = 1;
	assert(a.eventExternalMethod(41, &call) == kIOReturnSuccess && sweet_event_queues[0]->queued == 0);
	assert(a.eventCapabilityImport(name, out) == kIOReturnSuccess);
	assert(out[0] == 7 && out[1] == va && out[2] && sweet_event_map_count == 1);
	assert(!b.eventCapabilityCopy(name)); // Cannot use a connection owned by another task.
	assert(!a.eventCapabilityCopy(UINT64_C(0x10000002a)));

	caller = &taskB;
	assert(b.eventCapabilityImport(name, out) == kIOReturnBadArgument);
	OSObject *foreign = new OSObject;
	IOUserClient::rights[{&taskB, 43}] = foreign;
	assert(b.eventCapabilityImport(43, out) == kIOReturnBadArgument && foreign->refs == 1);
	// Fixture explicitly installs a second, authorized right to the same object.
	auto *sharedRight = IOUserClient::rights.at({&taskA, 42});
	sharedRight->retain();
	IOUserClient::rights[{&taskB, 44}] = sharedRight;
	assert(b.eventCapabilityImport(44, out) == kIOReturnSuccess);
	assert(sweet_event_map_count == 2 && out[0] == 7 && out[2] == event->traceID());
	IOUserClient::rights.erase({&taskA, 42});
	sharedRight->release();
	assert(sweet_event_map_count == 2); // One authorized foreign right still exists.
	a.eventMapsClose();
	assert(sweet_event_map_count == 1);
	IOUserClient::rights.erase({&taskB, 44});
	sharedRight->release();
	assert(!sweet_event_map_count); // Last right detached B's view while B is still open.
	assert(b.eventCapabilityCreate(out) == kIOReturnSuccess);
	b.eventMapsClose();
	assert(!sweet_event_map_count && b.eventCapabilityImport(42, out) == kIOReturnNotOpen);
	for (const auto &item : IOUserClient::rights) item.second->release();
	IOUserClient::rights.clear();
	event->release();
	assert(!sweet_event_counter_objects);
}
