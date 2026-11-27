/*
 * A55 SMP bring-up for sweet using XNU's legacy IOCPU interface and PSCI.
 * The architected timer and SGI1 already enter through pe_fiq/sleh_irq.
 * CPU_OFF, suspend and the A76 cores still require separate platform work.
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOCPU.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <arm64/proc_reg.h>
#include <machine/atomic.h>
#include <kern/clock.h>
#include <kern/processor.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include "SweetBoard.h"

extern "C" void sweet_secondary_entry(void);
extern "C" void sweet_x9_secondary_entry(void);
extern "C" boolean_t sweet_cpu_signal_is_enabled(unsigned int cpu);

/* X9 FDT hardware IDs use Aff1 = core index. The boot core must be 0. */
static bool
sweet_x9_cpu_valid(const ml_topology_info_t *topology, const ml_topology_cpu_t *cpu)
{
 return cpu != NULL && topology != NULL && cpu->cpu_id < 8 &&
     cpu->phys_id == (cpu->cpu_id << 8) && topology->boot_cpu != NULL &&
     topology->boot_cpu->cpu_id == 0 && topology->boot_cpu->phys_id == 0;
}

class SweetCPU;
static SweetCPU *sweet_cpus[MAX_CPUS];
static uint32_t sweet_cpus_registered;

/* SMCCC 1.0: x0 is the signed PSCI result; do not assume x1-x17 survive. */
static int32_t
sweet_psci_cpu_on(uint64_t target, uint64_t entry, uint64_t context)
{
	register uint64_t x0 __asm__("x0") = 0xC4000003ULL;
	register uint64_t x1 __asm__("x1") = target;
	register uint64_t x2 __asm__("x2") = entry;
	register uint64_t x3 __asm__("x3") = context;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
	    "x14", "x15", "x16", "x17", "cc", "memory");
	return (int32_t)x0;
}


/*
 * X9 CPU offline facility (boot arg x9cpuoff; design and firmware
 * evidence in x9/cpuoff/README.md). Only the MT6993 (x9) board is
 * supported; on other boards every entry point keeps the legacy
 * behaviour. Bit 0 of x9cpuoff arms the facility; with it clear,
 * quiesceCPU/haltCPU/initCPU(!boot) panic exactly as before.
 */
#define SWEET_PSCI_VERSION_FID       0x84000000ULL
#define SWEET_PSCI_CPU_SUSPEND_FID   0xC4000001ULL
#define SWEET_PSCI_CPU_OFF_FID       0x84000002ULL
#define SWEET_PSCI_AFFINITY_INFO_FID 0xC4000004ULL
#define SWEET_PSCI_FEATURES_FID      0x8400000AULL
/* DTS cpuoff arm,psci-suspend-param; also Linux cpu_psci_cpu_die's state. */
#define SWEET_PSCI_CPUOFF_STATE      0x00010000ULL
#define SWEET_PSCI_ALREADY_ON        (-4)

enum sweet_cpuoff_state : uint32_t {
	SWEET_CPUOFF_ONLINE = 0,
	SWEET_CPUOFF_QUIESCING = 1,
	SWEET_CPUOFF_OFF = 2,
	SWEET_CPUOFF_SUSPENDED = 3,
	SWEET_CPUOFF_ONLINING = 4,
	SWEET_CPUOFF_PARKED = 5,
};

static volatile uint32_t sweet_cpuoff_state[MAX_CPUS];
static volatile uint32_t sweet_cpuoff_booted[MAX_CPUS];
static volatile int sweet_cpuoff_mask_cache = -1;

static int
sweet_x9cpuoff_mask(void)
{
	int cached = __atomic_load_n(&sweet_cpuoff_mask_cache, __ATOMIC_ACQUIRE);
	if (cached >= 0) {
		return cached;
	}
	int v = 0;
	PE_parse_boot_argn("x9cpuoff", &v, sizeof(v));
	__atomic_store_n(&sweet_cpuoff_mask_cache, v, __ATOMIC_RELEASE);
	return v;
}

/* Also called from the overlay cpu_common.o (ml_cpu_can_exit gate). */
extern "C" int
sweet_x9_cpuoff_facility(void)
{
	return sweet_board_x9() && (sweet_x9cpuoff_mask() & 1) != 0;
}

static int64_t
sweet_psci_call(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fid;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
	    "x14", "x15", "x16", "x17", "cc", "memory");
	return (int64_t)x0;
}

extern "C" uint32_t
sweet_x9_cpuoff_get_state(unsigned int cpu)
{
	if (cpu >= MAX_CPUS) {
		return SWEET_CPUOFF_PARKED;
	}
	return __atomic_load_n(&sweet_cpuoff_state[cpu], __ATOMIC_ACQUIRE);
}

extern "C" void
sweet_x9_cpuoff_note_state(unsigned int cpu, uint32_t state)
{
	if (cpu < MAX_CPUS) {
		__atomic_store_n(&sweet_cpuoff_state[cpu], state, __ATOMIC_RELEASE);
	}
}

/* AFFINITY_INFO at the CPU's own affinity level: 0=ON, 1=OFF, 2=ON_PENDING,
 * or a negative PSCI error. Mirrors Linux cpu_psci_cpu_kill's death check. */
extern "C" int64_t
sweet_x9_psci_affinity_info(unsigned int cpu)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	if (cpu >= topology->num_cpus) {
		return -100;
	}
	return sweet_psci_call(SWEET_PSCI_AFFINITY_INFO_FID,
	    topology->cpus[cpu].phys_id, 0, 0);
}

/* Read-only firmware probe: version, feature availability, per-CPU
 * affinity snapshot. Logged once from startAll when the facility is on. */
extern "C" void
sweet_x9_cpuoff_probe(void)
{
	if (!sweet_x9_cpuoff_facility()) {
		return;
	}
	int64_t ver = sweet_psci_call(SWEET_PSCI_VERSION_FID, 0, 0, 0);
	int64_t f_off = sweet_psci_call(SWEET_PSCI_FEATURES_FID,
	    SWEET_PSCI_CPU_OFF_FID, 0, 0);
	int64_t f_sus = sweet_psci_call(SWEET_PSCI_FEATURES_FID,
	    SWEET_PSCI_CPU_SUSPEND_FID, 0, 0);
	IOLog("X9 CPUOFF: psci version=%lld.%lld features cpu_off=%lld "
	    "cpu_suspend=%lld x9cpuoff=%d depth=%s\n",
	    (long long)((ver >> 16) & 0x7fff), (long long)(ver & 0xffff),
	    (long long)f_off, (long long)f_sus, sweet_x9cpuoff_mask(),
	    (sweet_x9cpuoff_mask() & 16) ? "suspend" : "off");
	const ml_topology_info_t *topology = ml_get_topology_info();
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		IOLog("X9 CPUOFF: affinity cpu=%u mpidr=0x%x level0=%lld\n",
		    i, topology->cpus[i].phys_id,
		    (long long)sweet_x9_psci_affinity_info(i));
	}
}

class SweetCPU : public IOCPU {
	OSDeclareDefaultStructors(SweetCPU);
	const OSSymbol *cpuName = NULL;
	uint32_t initialized = 0; /* release/acquire publication after initCPU sends SGI */

public:
	static void startAll(void *arg, wait_result_t result);
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	void initCPU(bool boot) APPLE_KEXT_OVERRIDE;
	void quiesceCPU(void) APPLE_KEXT_OVERRIDE;
	kern_return_t startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr) APPLE_KEXT_OVERRIDE;
	uint32_t isInitialized(void) { return __atomic_load_n(&initialized, __ATOMIC_ACQUIRE); }
	void haltCPU(void) APPLE_KEXT_OVERRIDE;
	void signalCPU(IOCPU *target) APPLE_KEXT_OVERRIDE;
	const OSSymbol *getCPUName(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetCPU, IOCPU);

bool
SweetCPU::start(IOService *provider)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	OSNumber *logical = OSDynamicCast(OSNumber, provider->getProperty("logical-cpu-id"));
	OSData *reg = OSDynamicCast(OSData, provider->getProperty("reg"));

	if (logical == NULL || reg == NULL || reg->getLength() != sizeof(uint32_t) ||
	    logical->unsigned32BitValue() >= topology->num_cpus) {
		return false;
	}
	const ml_topology_cpu_t *cpu = &topology->cpus[logical->unsigned32BitValue()];
	if (*(const uint32_t *)reg->getBytesNoCopy() != cpu->phys_id) {
		return false;
	}
	bool valid;
	if (sweet_board_x676c()) {
		/* x676c (MT6789): MPIDR Aff1 = core: 0x000-0x500 A55, 0x600/0x700 A76 */
		valid = cpu->phys_id <= 0x700 && (cpu->phys_id & 0xff) == 0;
	} else if (sweet_board_x9()) {
		/* Captured MT6993 FDT uses 0x000..0x700 at Aff1:Aff0. */
		valid = sweet_x9_cpu_valid(topology, cpu);
	} else {
		/* topaz (SM6225): MPIDR Aff1 = cluster, Aff0 = core: 0x0-0x3 A53, 0x100-0x103 A73 */
		valid = (cpu->phys_id >> 8) <= 1 && (cpu->phys_id & 0xff) <= 3;
	}
	if (!valid || topology->num_cpus > SWEET_SMP_MAX_CPUS) {
		panic("SWEET CPU: invalid topology cpu=%u mpidr=0x%x count=%u",
		    cpu->cpu_id, cpu->phys_id, topology->num_cpus);
	}
	if (!IOCPU::start(provider)) {
		return false;
	}
	cpuName = OSSymbol::withCStringNoCopy(sweet_board_x9() ? "MT6993 core" :
	    sweet_board_x676c() ? (cpu->phys_id >= 0x600 ? "Cortex-A76" : "Cortex-A55") : "Kryo 260");
	if (cpuName == NULL) {
		return false;
	}
	setCPUNumber(cpu->cpu_id);

	ml_processor_info_t info = {};
	info.cpu_id = reinterpret_cast<cpu_id_t>(this);
	info.phys_id = cpu->phys_id;
	info.log_id = cpu->cpu_id;
	info.cluster_id = cpu->cluster_id;
	info.cluster_type = cpu->cluster_type;
	info.l2_cache_id = cpu->l2_cache_id;
	info.l2_cache_size = cpu->l2_cache_size;
	info.l3_cache_id = cpu->l3_cache_id;
	info.l3_cache_size = cpu->l3_cache_size;
	perfmon_interrupt_handler_func pmi_handler = NULL;
	kern_return_t result = ml_processor_register(&info, &machProcessor, &ipi_handler, &pmi_handler);
	if (result != KERN_SUCCESS) {
		panic("SWEET CPU: ml_processor_register failed: %d", result);
	}
	if (pmi_handler != NULL) {
		panic("SWEET CPU: PMI routing is not implemented");
	}

	registerService();
	__atomic_store_n(&sweet_cpus[cpu->cpu_id], this, __ATOMIC_RELEASE);
	__atomic_add_fetch(&sweet_cpus_registered, 1, __ATOMIC_ACQ_REL);
	if (cpu == topology->boot_cpu) {
		thread_t thread;
		result = kernel_thread_start(&SweetCPU::startAll, NULL, &thread);
		if (result != KERN_SUCCESS) {
			panic("SWEET CPU: cannot create startup thread: %d", result);
		}
		thread_deallocate(thread);
	}
	return true;
}

/* Do not wait in a nub's start(): matching can be asynchronous or serialized. */
void
SweetCPU::startAll(void *arg __unused, wait_result_t result __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t requested = 1;
	PE_parse_boot_argn("cpus", &requested, sizeof(requested));
	IOLog("SWEET CPU: requested=%u selected=%u (limit=%u)\n",
	    requested, topology->num_cpus, SWEET_SMP_MAX_CPUS);

	uint64_t deadline;
	nanoseconds_to_deadline(10 * NSEC_PER_SEC, &deadline);
	while (__atomic_load_n(&sweet_cpus_registered, __ATOMIC_ACQUIRE) != topology->num_cpus) {
		if (mach_absolute_time() > deadline) {
			for (unsigned int i = 0; i < topology->num_cpus; i++) {
				if (__atomic_load_n(&sweet_cpus[i], __ATOMIC_ACQUIRE) == NULL) {
					IOLog("SWEET CPU: cpu=%u NOT registered (IOKit matching timeout)\n", i);
				}
			}
			panic("SWEET CPU: CPU registration timeout");
		}
		IOSleep(1);
	}
	/*
	 * The shim enters xnu on the core the ADT lists first: topaz gold 0x100
	 * (its silver cluster has no 16K granule), x676c A55 0x000. Check against
	 * the ADT instead of a per-board MPIDR.
	 */
	if (topology->boot_cpu->cpu_id != 0 ||
	    topology->boot_cpu->phys_id != topology->cpus[0].phys_id) {
		panic("SWEET CPU: boot CPU 0x%x/%u is not the ADT's cpu0 (0x%x)",
		    topology->boot_cpu->phys_id, topology->boot_cpu->cpu_id,
		    topology->cpus[0].phys_id);
	}
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		SweetCPU *cpu = __atomic_load_n(&sweet_cpus[i], __ATOMIC_ACQUIRE);
		kern_return_t kr = processor_start(cpu->machProcessor);
		if (kr != KERN_SUCCESS) {
			panic("SWEET CPU: cpu=%u processor_start failed: %d", i, kr);
		}
		/* processor_start() is asynchronous for secondaries in this XNU. */
		nanoseconds_to_deadline(5 * NSEC_PER_SEC, &deadline);
		while (__atomic_load_n(&cpu->initialized, __ATOMIC_ACQUIRE) == 0 ||
		    !sweet_cpu_signal_is_enabled(i)) {
			if (mach_absolute_time() > deadline) {
				IOLog("SWEET CPU: cpu=%u mpidr=0x%x NOT alive after 5s\n",
				    i, topology->cpus[i].phys_id);
				panic("SWEET CPU: cpu=%u startup timeout (reset/MMU/GIC/SGI)", i);
			}
			IOSleep(1);
		}
		IOLog("SWEET CPU: cpu=%u mpidr=0x%x came alive; SGI1 OK\n",
		    i, topology->cpus[i].phys_id);
	}
	if (sweet_x9_cpuoff_facility()) {
		sweet_x9_cpuoff_probe();
	}
	/* Publish exactly once, after every selected CPU has completed initCPU(). */
	ml_cpu_init_completed();
	__atomic_load_n(&sweet_cpus[topology->boot_cpu->cpu_id], __ATOMIC_ACQUIRE)->cpusRunning();
	IOService::publishResource(gIOAllCPUInitializedKey, kOSBooleanTrue);
	ml_set_max_cpus(topology->max_cpu_id + 1);
	IOLog("SWEET CPU: %u/%u CPUs initialized, SGI1 verified\n",
	    topology->num_cpus, topology->num_cpus);
	thread_terminate(current_thread());
}

void
SweetCPU::free(void)
{
	OSSafeReleaseNULL(cpuName);
	IOCPU::free();
}

void
SweetCPU::initCPU(bool boot)
{
	if (!boot && !sweet_x9_cpuoff_facility()) {
		panic("SWEET CPU: cpu=%u resume is not implemented", getCPUNumber());
	}
	assert(cpu_number() == (int)getCPUNumber());

	ml_broadcast_cpu_event(CPU_BOOTED, getCPUNumber());

	/* Secondary startup runs at splsched: queue the self-IPI and return.
	 * startAll waits for its delivery after the startup thread unmasks IRQs.
	 * Waiting here with DAIF.I set would deadlock every secondary. */
	signalCPU(this);
	setCPUState(kIOCPUStateRunning);
	kprintf("SWEET CPU: cpu=%u entered initCPU mpidr=0x%llx EL%llu IRQs=%u\n",
	    getCPUNumber(), __builtin_arm_rsr64("MPIDR_EL1") & 0xffffULL,
	    __builtin_arm_rsr64("CurrentEL") >> 2, ml_get_interrupts_enabled());
	__atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&sweet_cpuoff_booted[getCPUNumber()], 1, __ATOMIC_RELEASE);
	if (!boot) {
		__atomic_store_n(&sweet_cpuoff_state[getCPUNumber()],
		    SWEET_CPUOFF_ONLINE, __ATOMIC_RELEASE);
		IOLog("X9 CPUOFF: cpu=%u hot-online complete\n", getCPUNumber());
	}
}

void
SweetCPU::signalCPU(IOCPU *target)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	unsigned int logical = target->getCPUNumber();
	if (logical >= topology->num_cpus) {
		panic("SWEET CPU: invalid SGI target %u", logical);
	}
	/* Use the TARGET affinity, including Aff1, never the sender's MPIDR. */
	uint64_t mpidr = topology->cpus[logical].phys_id;
	unsigned int aff0 = mpidr & 0xff;
	if (aff0 >= 16) {
		panic("SWEET CPU: SGI target requires unsupported range selector");
	}
	uint64_t sgi = (((mpidr >> 32) & 0xff) << 48) |
	    (((mpidr >> 16) & 0xff) << 32) |
	    (((mpidr >> 8) & 0xff) << 16) |
	    ((uint64_t)SWEET_IPI_SGI << 24) | (1ULL << aff0);
	__builtin_arm_dsb(DSB_ISHST);
	__builtin_arm_wsr64("ICC_SGI1R_EL1", sgi);
	__builtin_arm_isb(ISB_SY);
}

kern_return_t
SweetCPU::startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr)
{
	const ml_topology_cpu_t *cpu = &ml_get_topology_info()->cpus[getCPUNumber()];
	/* cpu_start() passes NULL,NULL: x20/x21 are set by LowResetVectorBase. */
	if (start_paddr != 0 || arg_paddr != 0 || cpu == ml_get_topology_info()->boot_cpu) {
		panic("SWEET CPU: cpu=%u unsupported restart", getCPUNumber());
	}
	unsigned int me = getCPUNumber();
	uint32_t off_state = __atomic_load_n(&sweet_cpuoff_state[me], __ATOMIC_ACQUIRE);
	bool hot = __atomic_load_n(&sweet_cpuoff_booted[me], __ATOMIC_ACQUIRE) != 0;
	if (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE) != 0 ||
	    (hot && off_state != SWEET_CPUOFF_OFF &&
	    off_state != SWEET_CPUOFF_SUSPENDED)) {
		panic("SWEET CPU: cpu=%u unsupported restart", me);
	}
	if (hot) {
		__atomic_store_n(&sweet_cpuoff_state[me], SWEET_CPUOFF_ONLINING,
		    __ATOMIC_RELEASE);
	}
	vm_offset_t entry = ml_static_vtop((vm_offset_t)(sweet_board_x9() ? &sweet_x9_secondary_entry : &sweet_secondary_entry));
	IOLog("SWEET CPU: cpu=%u CPU_ON target=0x%x entry_pa=0x%llx context=%u\n",
	    getCPUNumber(), cpu->phys_id, (uint64_t)entry, getCPUNumber());
	__builtin_arm_dsb(DSB_SY);
	int32_t rc = sweet_psci_cpu_on(cpu->phys_id, entry, getCPUNumber());
	IOLog("SWEET CPU: cpu=%u PSCI CPU_ON rc=%d\n", getCPUNumber(), rc);
	/* cpu_start() discards PE_cpu_start's return; do not silently carry on. */
	if (rc != 0 && !hot) {
		panic("SWEET CPU: cpu=%u NOT alive: PSCI CPU_ON rc=%d", getCPUNumber(), rc);
	}
	if (rc != 0) {
		/* Hot re-online failure must not panic the machine for one core.
		 * The cpuoff manager confirms liveness itself (initialized flag
		 * + AFFINITY_INFO) and blacklists a core that does not come
		 * back; see x9/cpuoff/README.md. */
		IOLog("X9 CPUOFF: cpu=%u CPU_ON rc=%d on hot re-online\n", me, rc);
		if (rc == SWEET_PSCI_ALREADY_ON && off_state == SWEET_CPUOFF_SUSPENDED) {
			/* Suspend-depth wake hypothesis (probe item): the SGI is
			 * the wake event for a core parked in CPU_SUSPEND. */
			signalCPU(this);
			IOLog("X9 CPUOFF: cpu=%u SGI wake issued after ALREADY_ON\n", me);
			return KERN_SUCCESS;
		}
		__atomic_store_n(&sweet_cpuoff_state[me], SWEET_CPUOFF_PARKED,
		    __ATOMIC_RELEASE);
		IOLog("X9 CPUOFF: cpu=%u re-online failed; core parked until reboot\n", me);
	}
	return KERN_SUCCESS;
}

void
SweetCPU::quiesceCPU(void)
{
	unsigned int cpu = getCPUNumber();
	if (!sweet_x9_cpuoff_facility() || cpu == 0) {
		panic("SWEET CPU: CPU quiesce is not implemented");
	}
	/* The little cluster (cpu0-3) is never offlined: the factory core_ctl
	 * keeps min_cpus={4,0,0}, and cpu0 is the boot CPU. */
	if (cpu < 4) {
		panic("SWEET CPU: cpu=%u quiesce refused (little cluster)", cpu);
	}
	/*
	 * We run on the target CPU (its idle thread, on the interrupt stack)
	 * from cpu_sleep() via PE_cpu_machine_quiesce(). The frozen path has
	 * already migrated this CPU's timers (ml_cpu_down), disabled CPU
	 * signaling and cleaned the D-cache to PoC. Returning from here
	 * makes cpu_sleep() panic ("zombie processor"), so the success path
	 * must not return: PSCI CPU_OFF / CPU_SUSPEND(powerdown) do not
	 * return on success. If the firmware refuses, park in WFI instead
	 * of returning; the scheduler has already removed this CPU and the
	 * manager treats it as lost for this boot.
	 */
	__atomic_store_n(&sweet_cpuoff_state[cpu], SWEET_CPUOFF_QUIESCING,
	    __ATOMIC_RELEASE);
	__atomic_store_n(&initialized, 0, __ATOMIC_RELEASE);
	setCPUState(kIOCPUStateStopped);
	int suspend = (sweet_x9cpuoff_mask() & 16) != 0;
	IOLog("X9 CPUOFF: cpu=%u quiesce depth=%s\n", cpu,
	    suspend ? "suspend" : "off");
	__asm__ volatile("msr DAIFSet, #0xf" ::: "memory");
	const ml_topology_cpu_t *tcpu = &ml_get_topology_info()->cpus[cpu];
	vm_offset_t entry = ml_static_vtop((vm_offset_t)(sweet_board_x9() ?
	    &sweet_x9_secondary_entry : &sweet_secondary_entry));
	int64_t rc = 0;
	for (int attempt = 0; attempt < 4; attempt++) {
		bool use_suspend = suspend && attempt < 2;
		if (use_suspend) {
			rc = sweet_psci_call(SWEET_PSCI_CPU_SUSPEND_FID,
			    SWEET_PSCI_CPUOFF_STATE, entry, cpu);
		} else {
			rc = sweet_psci_call(SWEET_PSCI_CPU_OFF_FID,
			    SWEET_PSCI_CPUOFF_STATE, 0, 0);
		}
		/* Any return is a refusal: a successful powerdown does not
		 * come back here (wake re-enters at the reset vector). */
		IOLog("X9 CPUOFF: cpu=%u psci %s attempt=%d rc=%lld\n", cpu,
		    use_suspend ? "CPU_SUSPEND" : "CPU_OFF", attempt,
		    (long long)rc);
		(void)tcpu;
	}
	__atomic_store_n(&sweet_cpuoff_state[cpu], SWEET_CPUOFF_PARKED,
	    __ATOMIC_RELEASE);
	IOLog("X9 CPUOFF: cpu=%u PARKED after psci refusal rc=%lld; "
	    "core lost until reboot\n", cpu, (long long)rc);
	for (;;) {
		__asm__ volatile("wfi" ::: "memory");
	}
}

void
SweetCPU::haltCPU(void)
{
	panic("SWEET CPU: CPU halt/suspend is not implemented");
}

const OSSymbol *
SweetCPU::getCPUName(void)
{
	return cpuName;
}

/* Manager-side liveness check: the target CPU's initCPU publication. */
extern "C" uint32_t
sweet_x9_cpu_is_initialized(unsigned int cpu)
{
	if (cpu >= MAX_CPUS) {
		return 0;
	}
	SweetCPU *c = __atomic_load_n(&sweet_cpus[cpu], __ATOMIC_ACQUIRE);
	return c != NULL ? c->isInitialized() : 0;
}
#endif /* SWEET */
