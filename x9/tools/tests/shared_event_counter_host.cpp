// Host fixture: compile the real counter against minimal IOKit stand-ins.
// No Mach messages, VM mappings or kernel scheduling are executed here.
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#define APPLE_KEXT_OVERRIDE override
#define OSDeclareDefaultStructors(x)
#define OSDefineMetaClassAndStructors(x, y)
#define OSTypeAlloc(x) new x
#define OSSafeReleaseNULL(p) do { if (p) { (p)->release(); (p) = nullptr; } } while (0)

struct OSObject {
	unsigned refs = 1;
	virtual ~OSObject() = default;
	virtual void free() {}
	bool init() { return true; }
	void retain() { ++refs; }
	void release() { if (!--refs) { free(); delete this; } }
};
using task_t = void *;
using IOReturn = int;
static int kernelTask;
static task_t kernel_task = &kernelTask;
static constexpr size_t page_size = 4096;
static constexpr unsigned kIOMapAnywhere = 1, kIOMapReadOnly = 0x1000;
static constexpr unsigned kIODirectionInOut = 3, kIOMemoryKernelUserShared = 8;
static constexpr unsigned NSEC_PER_MSEC = 1000000;
static constexpr int kIOReturnSuccess = 0, kIOReturnTimeout = 1, kIOReturnAborted = 2;
static constexpr int THREAD_ABORTSAFE = 1, THREAD_AWAKENED = 1, THREAD_TIMED_OUT = 2;

struct IOLock { bool held = false; };
static IOLock *IOLockAlloc() { return new IOLock; }
static void IOLockFree(IOLock *lock) { assert(!lock->held); delete lock; }
static void IOLockLock(IOLock *lock) { assert(!lock->held); lock->held = true; }
static void IOLockUnlock(IOLock *lock) { assert(lock->held); lock->held = false; }
static unsigned wakes, sleeps;
static void IOLockWakeup(IOLock *lock, void *, bool one) { assert(lock->held && !one); ++wakes; }

struct SleepStep { int state; uint64_t value; };
static SleepStep scripted[4];
static int IOLockSleepDeadline(IOLock *lock, void *event, uint64_t deadline, int mode)
{
	assert(lock->held && deadline == 7 * NSEC_PER_MSEC && mode == THREAD_ABORTSAFE);
	assert(sleeps < 4);
	const auto step = scripted[sleeps++];
	__atomic_store_n(static_cast<uint64_t *>(event), step.value, __ATOMIC_RELEASE);
	return step.state;
}
static void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *out)
{ *out = uint64_t(interval) * scale; }

struct IOMemoryMap : OSObject {
	unsigned options = 0;
	const uint64_t *bytes = nullptr;
	uint64_t getAddress() const { return reinterpret_cast<uintptr_t>(bytes); }
};

static bool failPage;
struct IOBufferMemoryDescriptor : OSObject {
	alignas(uint64_t) uint8_t bytes[page_size];
	static IOBufferMemoryDescriptor *inTaskWithOptions(task_t task, unsigned options, size_t size, size_t alignment)
	{
		assert(task == kernel_task && options == (kIODirectionInOut | kIOMemoryKernelUserShared));
		assert(size == page_size && alignment == page_size);
		if (failPage) return nullptr;
		auto p = new IOBufferMemoryDescriptor;
		memset(p->bytes, 0x55, sizeof(p->bytes));
		return p;
	}
	void *getBytesNoCopy() { return bytes; }
	IOMemoryMap *createMappingInTask(task_t task, uint64_t address, unsigned options)
	{
		assert(task && task != kernel_task && !address);
		auto m = new IOMemoryMap;
		m->options = options;
		m->bytes = reinterpret_cast<const uint64_t *>(bytes);
		return m;
	}
};

#include "../../kernel/src/iokit/Kernel/arm/SweetSharedEventCounter.inc"

int main()
{
	failPage = true;
	assert(!SweetSharedEventCounter::make() && sweet_event_counter_objects == 0);
	failPage = false;
	auto event = SweetSharedEventCounter::make();
	assert(event && event->value() == 0);
	assert(!event->mapReadOnly(nullptr) && !event->mapReadOnly(kernel_task));
	int client;
	auto map = event->mapReadOnly(&client);
	assert(map && map->options == (kIOMapAnywhere | kIOMapReadOnly));
	const auto *raw = reinterpret_cast<const unsigned char *>(map->bytes);
	for (size_t i = 0; i != page_size; ++i) assert(raw[i] == 0);

	assert(event->advance(10) && !event->advance(10) && !event->advance(9));
	assert(event->value() == 10 && *map->bytes == 10 && wakes == 1);
	assert(event->waitFor(10, 0) == kIOReturnSuccess);
	assert(event->waitFor(11, 0) == kIOReturnTimeout && sleeps == 0);
	scripted[0] = {THREAD_AWAKENED, 10};   // Recheck after a spurious wake.
	scripted[1] = {THREAD_TIMED_OUT, 15};  // Predicate wins at the boundary.
	assert(event->waitFor(15, 7) == kIOReturnSuccess && sleeps == 2);
	scripted[2] = {THREAD_TIMED_OUT, 15};
	assert(event->waitFor(16, 7) == kIOReturnTimeout);
	map->release();
	event->release();

	SweetSharedEventCounter *held[kSweetEventCounterLimit];
	for (auto &e : held) { e = SweetSharedEventCounter::make(); assert(e); }
	assert(!SweetSharedEventCounter::make());
	for (auto e : held) e->release();
	assert(sweet_event_counter_objects == 0);
	return 0;
}
