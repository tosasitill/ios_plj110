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
    kIOReturnNotPermitted = 15;

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
	assert(a.eventCapabilityCreate(out) == kIOReturnSuccess);
	const uint64_t name = out[0], va = out[1];
	assert(name == 42 && va && sweet_event_map_count == 1);
	auto *event = a.eventCapabilityCopy(name);
	assert(event && event->advance(7));
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
