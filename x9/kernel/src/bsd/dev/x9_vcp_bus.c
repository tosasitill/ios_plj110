/* Native MMIO, bounded dual-core READY and OUT0/IN1 lifecycle. Captured
 * mtk-mbox.ko trigger_irq writes SET; check_send reads optional status or SET;
 * clr_irq writes CLR; read_recv reads optional status or CLR. Current FDT's
 * exact reg-names have no optional status registers: use the native fallback.
 * Only thread-context polling is implemented, not GIC IRQ registration. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <arm/memory_types.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>
#include <pexpert/device_tree.h>
#include "x9_vcp_bus.h"
#include "x9_vcp_handoff.h"
#include "x9_mm_power.h"
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern int sweet_early_is_x9(void);
LCK_GRP_DECLARE(vcp_bus_group, "x9-vcp-bus");
LCK_MTX_DECLARE(vcp_bus_lock, &vcp_bus_group);
#define VCP_BUS_RING 32u
#define VCP_BUS_PAGE 16384u
#define VCP_BUS_MAPS 5u
struct vcp_bus_instance {
    struct x9_vcp_init init;
    struct x9_vcp_bus_reverse_ops ops;
    struct x9_vcp_bus_instance_status status;
};
struct x9_vcp_bus {
    struct x9_vcp_bus_owner owner;
    struct x9_vcp_shm_region shared, logger;
    struct x9_vcp_bus_status status;
    struct x9_vcp_handoff layout;
    vm_offset_t mmio[VCP_BUS_MAPS]; /* VCP0, VCP2, MMUP1, cfg0, cfg1 */
    vm_offset_t map[VCP_BUS_MAPS]; /* page-aligned VA for actual unmap */
    uint8_t boot[2][X9_VCP_BOOT_GPR_BYTES];
    uint64_t power_child;
    uint32_t head;
    uint8_t ring[VCP_BUS_RING][X9_VCP_WIRE_BYTES];
    uint8_t tx[X9_VCP_WIRE_BYTES];
    const struct x9_vcp_bus_call_log *call; /* lifetime/IO exclusion, not a wire token */
    uint64_t clients[X9_VCP_INSTANCE_SLOT_COUNT]; /* host lifetime tokens */
    struct vcp_bus_instance instances[X9_VCP_INSTANCE_SLOT_COUNT];
    struct vcp_bus_instance *caller; /* frozen under the whole-call reservation */
    bool exposed, tx_pending, stop_attempted;
};
static struct x9_vcp_bus *singleton;
static uint64_t next_epoch = 1;
static uint64_t next_client_token = 1;
static uint32_t get32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint64_t get64(const uint8_t *p)
{ return get32(p) | (uint64_t)get32(p+4)<<32; }
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8*i)); }
static uint64_t now_ns(void)
{
    uint64_t ticks, ns;
    clock_get_uptime(&ticks); absolutetime_to_nanoseconds(ticks, &ns);
    return ns;
}
static int deadline(uint32_t ms, uint64_t *out)
{
    uint64_t ns = now_ns();
    if (!ms || ms > 3000 || ns > UINT64_MAX - (uint64_t)ms * 1000000) return EINVAL;
    *out = ns + (uint64_t)ms * 1000000;
    return 0;
}
static int identity(struct x9_vcp_bus *b)
{ return !b || b != singleton ? EINVAL : 0; }
static int fault(struct x9_vcp_bus *b, int e)
{
    if (!b->status.last_errno) b->status.last_errno = (uint32_t)e;
    b->status.phase = X9_VCP_BUS_QUARANTINED;
    return (int)b->status.last_errno;
}
static int guard(struct x9_vcp_bus *b)
{
    int e = x9_mm_power_validate_pin(b->owner.power_token, b->power_child, X9_MM_POWER_VCP);
    if (!e) e = b->owner.guard(b->owner.cookie, b->status.epoch, b->status.phase);
    return e;
}
static uint32_t read32(struct x9_vcp_bus *b, unsigned box, unsigned offset)
{
    uint32_t v = *(volatile uint32_t *)(b->mmio[box] + offset);
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}
static void write32(struct x9_vcp_bus *b, unsigned box, unsigned offset, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)(b->mmio[box] + offset) = v;
    __asm__ volatile("dsb sy" : : : "memory");
}
static void snapshot(struct x9_vcp_bus *b, unsigned box, unsigned offset,
                        uint8_t *dst, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i += 4) put32(dst + i, read32(b, box, offset + i));
}
static int copy_topology(struct x9_vcp_handoff *h)
{
    DTEntry chosen; const void *p = NULL; unsigned int bytes = 0;
    if (!sweet_early_is_x9() || SecureDTLookupEntry(NULL,"/chosen",&chosen) != kSuccess ||
        SecureDTGetProperty(chosen,"x9-vcp-handoff",&p,&bytes) != kSuccess ||
        !p || bytes != sizeof(*h)) return ENXIO;
    memcpy(h,p,sizeof(*h));
    return x9_vcp_handoff_valid(h) ? 0 : ENXIO;
}
static int boot_images(struct x9_vcp_bus *b)
{
    const struct x9_vcp_shm_region *s = &b->shared;
    const struct x9_vcp_shm_region *m = &b->logger;
    struct x9_vcp_boot_snapshot observed;
    uint32_t shared, logger[2];
    int e;
    /* The qualified CURRENT FDT selects a complete layout, not merely a
     * total allocation size. Do not accept an arbitrary subregion as logger
     * or let the firmware's low27 alias conversion wrap the global block. */
    if (b->layout.global_layout != X9_VCP_GLOBAL_LAYOUT_CAPTURE1 ||
        s->bytes != X9_VCP_GLOBAL_BYTES || m->iova < s->iova ||
        m->iova-s->iova != X9_VCP_GLOBAL_LOGGER_OFFSET ||
        m->bytes != X9_VCP_GLOBAL_LOGGER_BYTES ||
        s->bytes > (UINT64_C(1)<<27)-(s->iova & 0x7ffffff)) return ERANGE;
    e = x9_vcp_pack_iova(s->iova,&shared);
    if (!m->bytes || m->iova >= (UINT64_C(1)<<36) ||
        m->bytes >= (UINT64_C(1)<<36)-m->iova) return ERANGE;
    if (!e) e = x9_vcp_pack_iova(m->iova,&logger[X9_VCP_BUS_CORE_MMUP]);
    if (!e) e = x9_vcp_pack_iova(m->iova+m->bytes,&logger[X9_VCP_BUS_CORE_VCP]);
    if (e) return e;
    /* One real logger block, not two. Captured RV55 ffcc..ffd8 puts its END
     * in protection-table entry10 via 10e52; MMUP 15506..1553c uses its START
     * to split regions10/11 via 15eb8. The GPR name is not an allocation ABI.
     * The parser below still bounds the entire shared extent and logger. */
    for (unsigned core = 0; core < 2; core++) {
        put32(b->boot[core],shared); put32(b->boot[core]+4,s->bytes);
        put32(b->boot[core]+8,logger[core]);
    }
    return x9_vcp_parse_boot_gprs(X9_VCP_BOOT_ABI_MT6993_CAPTURE1,s,m,
        b->boot[X9_VCP_BUS_CORE_MMUP],X9_VCP_BOOT_GPR_BYTES,
        b->boot[X9_VCP_BUS_CORE_VCP],X9_VCP_BOOT_GPR_BYTES,&observed);
}
static void free_bus(struct x9_vcp_bus *b)
{
    for (unsigned i = 0; i < VCP_BUS_MAPS; i++)
        if (b->map[i]) ml_io_unmap(b->map[i],VCP_BUS_PAGE);
    singleton = NULL;
    kfree_type(struct x9_vcp_bus,b);
}
int x9_vcp_bus_create(const struct x9_vcp_bus_owner *o, struct x9_vcp_bus **out)
{
    struct x9_vcp_bus *b;
    int e;
    if (!o || !out || *out || !o->power_token || !o->guard ||
        !o->halt_and_drain || !o->resources_retired) return EINVAL;
    /* Early io_map ignores the unmappable flag and returns boot-permanent VA.
     * This controller owns reclaimable mappings, so never create it there. */
    if (startup_phase < STARTUP_SUB_KMEM) return ENXIO;
    lck_mtx_lock(&vcp_bus_lock);
    if (singleton) { lck_mtx_unlock(&vcp_bus_lock); return EBUSY; }
    if (!next_epoch || next_epoch == UINT64_MAX) {
        lck_mtx_unlock(&vcp_bus_lock); return EOVERFLOW;
    }
    b = kalloc_type(struct x9_vcp_bus,Z_WAITOK|Z_ZERO);
    if (!b) { lck_mtx_unlock(&vcp_bus_lock); return ENOMEM; }
    singleton = b; b->owner = *o; b->status.epoch = next_epoch++;
    b->status.phase = X9_VCP_BUS_RESERVED;
    e = copy_topology(&b->layout);
    if (!e) e = x9_mm_power_pin(o->power_token,X9_MM_POWER_VCP,&b->power_child);
    if (e) {
        int off = b->power_child ? x9_mm_power_unpin(o->power_token,&b->power_child,X9_MM_POWER_VCP) : 0;
        if (off) { fault(b,e); *out = b; } /* retained cleanup handle, not success */
        else free_bus(b);
    } else *out = b;
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
int x9_vcp_bus_bind(struct x9_vcp_bus *b, uint64_t epoch,
    const struct x9_vcp_shm_region *shared, const struct x9_vcp_shm_region *logger)
{
    const unsigned indices[] = {0,2,4};
    int e;
    if (!shared || !logger || !epoch) return EINVAL;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b); if (e) goto out;
    if (epoch != b->status.epoch) { e = ESTALE; goto out; }
    if (b->status.phase != X9_VCP_BUS_RESERVED) { e = EBUSY; goto out; }
    b->shared = *shared; b->logger = *logger;
    e = boot_images(b);
    if (!e) e = guard(b);
    for (unsigned i = 0; i < VCP_BUS_MAPS && !e; i++) {
        vm_offset_t pa = i < 3 ? b->layout.mbox[indices[i]].base : b->layout.cfg_base[i-3];
        vm_offset_t offset = pa & (VCP_BUS_PAGE-1);
        b->map[i] = ml_io_map_unmappable(pa-offset,VCP_BUS_PAGE,VM_WIMG_IO);
        if (!b->map[i]) e = ENOMEM;
        else b->mmio[i] = b->map[i]+offset;
    }
    /* Owner may have installed DMA before bind. Never implicitly unpin/free
     * that session, even if no GPR/reset/slot has yet been exposed by us. */
    if (e) e = fault(b,e);
    else b->status.phase = X9_VCP_BUS_COLD;
out:
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
/* One irreversible publication/release attempt, already journalled by start.
 * Current KO reset_vcp writes these exact three GPRs, then c200052c cmd6/3,
 * x2=1, x3..7=0. Only actual bus READY consumption advances to the next core. */
static int release_core(struct x9_vcp_bus *b, unsigned core, uint64_t end)
{
    uint8_t first[X9_VCP_BOOT_GPR_BYTES], second[X9_VCP_BOOT_GPR_BYTES];
    const unsigned cfg = 3 + core, ready_box = core == X9_VCP_BUS_CORE_MMUP ? 2 : 1;
    int e;
    for (unsigned i = 0; i < X9_VCP_BOOT_GPR_BYTES; i += 4) {
        e = guard(b); if (e) return e;
        if (now_ns() >= end) return ETIMEDOUT;
        write32(b,cfg,0x44+i,get32(b->boot[core]+i));
    }
    e = guard(b); if (e) return e;
    snapshot(b,cfg,0x44,first,sizeof(first)); snapshot(b,cfg,0x44,second,sizeof(second));
    if (memcmp(first,second,sizeof(first)) || memcmp(first,b->boot[core],sizeof(first))) return EIO;
    if (read32(b,ready_box,0x10c)&(1u<<7)) return EPROTO;
    e = guard(b); if (e) return e;
    if (now_ns() >= end) return ETIMEDOUT;
    {
        register uint64_t x0 __asm__("x0") = 0xc200052cu;
        register uint64_t x1 __asm__("x1") = core == X9_VCP_BUS_CORE_MMUP ? 6u : 3u;
        register uint64_t x2 __asm__("x2") = 1, x3 __asm__("x3") = 0;
        register uint64_t x4 __asm__("x4") = 0, x5 __asm__("x5") = 0;
        register uint64_t x6 __asm__("x6") = 0, x7 __asm__("x7") = 0;
        __asm__ volatile("dsb sy; smc #0; dsb sy; isb"
            : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4),
              "+r"(x5), "+r"(x6), "+r"(x7) : : "memory");
        b->status.reset_smc_a0[core] = x0;
        b->status.reset_smc_bitmap |= 1u << core;
        if ((uint32_t)x0) return EIO;
    }
    return guard(b);
}
/* All accesses serialized by bus_lock; firmware must hold a receive slot
 * unchanged until this AP clears that specific pin. Never clear a foreign pin.
 * An uncertain clear may expose a next packet: quarantine, do not clear twice. */
static int ready_pin(struct x9_vcp_bus *b, unsigned box, unsigned controller,
                        unsigned channel, unsigned ready_index)
{
    uint8_t first[4], second[4]; struct x9_vcp_ready_event event;
    const uint32_t mask = 1u << 7;
    int e = guard(b);
    if (e) return e;
    if (!(read32(b,box,0x10c) & mask)) return 0;
    if (!(b->status.released_bitmap & (1u << ready_index)) ||
        (b->status.ready_bitmap & (1u << ready_index))) return EPROTO;
    snapshot(b,box,56,first,4); snapshot(b,box,56,second,4);
    e = guard(b);
    if (e) return e;
    if (memcmp(first,second,4) || !(read32(b,box,0x10c)&mask)) return EIO;
    e = x9_vcp_parse_ready_event(X9_VCP_READY_ABI_MT6993_CAPTURE1,controller,channel,
        b->layout.cores,b->layout.sram_bytes,first,4,&event);
    if (e) return e;
    if (event.ready_index != ready_index) return EPROTO;
    b->exposed = true;
    write32(b,box,0x10c,mask);
    if (read32(b,box,0x10c)&mask) return EIO;
    b->status.ready_bitmap |= 1u << ready_index;
    return 0;
}
static int collect(struct x9_vcp_bus *b)
{
    uint8_t first[X9_VCP_WIRE_BYTES], second[X9_VCP_WIRE_BYTES];
    const uint32_t mask = 1u << 9;
    unsigned tail;
    int e = ready_pin(b,1,X9_VCP_CONTROLLER_VCP,X9_VCP_READY_CHANNEL,0);
    if (!e) e = ready_pin(b,2,X9_VCP_CONTROLLER_MMUP,X9_MMUP_READY_CHANNEL,1);
    if (!e) e = guard(b);
    if (e) return e;
    if (!(read32(b,0,0x10c)&mask)) return 0;
    if (b->status.queued == VCP_BUS_RING) return ENOSPC; /* leave firmware slot latched */
    snapshot(b,0,72,first,sizeof(first)); snapshot(b,0,72,second,sizeof(second));
    e = guard(b);
    if (e) return e;
    if (memcmp(first,second,sizeof(first)) || !(read32(b,0,0x10c)&mask)) return EIO;
    if (!get32(first+4) || get32(first+4) > X9_VCP_PAYLOAD_BYTES) return EPROTO;
    tail = (b->head + b->status.queued) % VCP_BUS_RING;
    memcpy(b->ring[tail],first,sizeof(first));
    b->status.queued++; b->status.received++; /* retain bytes BEFORE ACK */
    b->exposed = true;
    write32(b,0,0x10c,mask);
    if (read32(b,0,0x10c)&mask) return EIO;
    return 0;
}
int x9_vcp_bus_start(struct x9_vcp_bus *b, uint32_t timeout_ms)
{
    uint64_t end = 0;
    unsigned loops = 0;
    int e = deadline(timeout_ms,&end);
    if (e) return e;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b);
    if (e) goto out;
    if (b->status.phase != X9_VCP_BUS_COLD) { e = EBUSY; goto out; }
    e = guard(b);
    if (e) goto out; /* still private, no reset or slot write */
    if ((read32(b,0,0x100)&1u) || (read32(b,0,0x10c)&(1u<<9)) ||
        (read32(b,1,0x10c)&(1u<<7)) || (read32(b,2,0x10c)&(1u<<7))) {
        e = EBUSY; goto out; /* do not erase inherited traffic */
    }
    e = guard(b);
    if (e || now_ns() >= end) { if (!e) e = ETIMEDOUT; goto out; }
    b->exposed = true; b->status.phase = X9_VCP_BUS_STARTING;
    b->status.released_bitmap = 1u << X9_VCP_BUS_CORE_MMUP;
    e = release_core(b,X9_VCP_BUS_CORE_MMUP,end);
    if (e) { e = fault(b,e); goto out; }
    b->status.phase = X9_VCP_BUS_WAIT_READY;
    do {
        if (now_ns() >= end || ++loops > 300000) { e = ETIMEDOUT; break; }
        e = collect(b);
        if (e == ENOSPC) e = 0; /* retain RX backpressure, still collect READY */
        if (e || b->status.ready_bitmap == 3) break;
        if (b->status.ready_bitmap == (1u << X9_VCP_BUS_CORE_MMUP) &&
            b->status.released_bitmap == (1u << X9_VCP_BUS_CORE_MMUP)) {
            e = guard(b);
            if (!e && now_ns() >= end) e = ETIMEDOUT;
            if (e) break;
            /* Captured reset_vcp waits for core1's physical READY before
             * programming/releasing core0. Record before call: never retry
             * an uncertain SMC/GPR publication, even if release fails. */
            b->status.phase = X9_VCP_BUS_STARTING;
            b->status.released_bitmap |= 1u << X9_VCP_BUS_CORE_VCP;
            e = release_core(b,X9_VCP_BUS_CORE_VCP,end);
            if (e) break;
            b->status.phase = X9_VCP_BUS_WAIT_READY;
        }
        delay(10);
    } while (1);
    if (!e && now_ns() >= end) e = ETIMEDOUT;
    if (!e) e = guard(b);
    if (e) e = fault(b,e);
    else b->status.phase = X9_VCP_BUS_RUNNING;
out:
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
static int send_until(struct x9_vcp_bus *b, const uint8_t packet[X9_VCP_WIRE_BYTES],
    uint64_t end, const struct x9_vcp_bus_call_log *call)
{
    uint8_t input[X9_VCP_WIRE_BYTES]; unsigned loops = 0;
    int e;
    if (!packet) return EINVAL;
    memcpy(input,packet,sizeof(input));
    if ((get32(input) != 1 && get32(input) != 2) || !get32(input+4) ||
        get32(input+4) > X9_VCP_PAYLOAD_BYTES) return EINVAL;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b); if (e) goto out;
    if (b->call != call) { e = EBUSY; goto out; }
    if (b->status.phase != X9_VCP_BUS_RUNNING || b->status.ready_bitmap != 3 ||
        b->tx_pending) { e = EBUSY; goto out; }
    if (now_ns() >= end) { e = ETIMEDOUT; goto out; }
    e = guard(b); if (e) { e = fault(b,e); goto out; }
    if (read32(b,0,0x100)&1u) { e = fault(b,EBUSY); goto out; }
    memcpy(b->tx,input,sizeof(input));
    b->tx_pending = true; b->exposed = true;
    for (unsigned i = 0; i < sizeof(input); i += 4) {
        e = guard(b); if (e) { e = fault(b,e); goto out; }
        if (now_ns() >= end) { e = fault(b,ETIMEDOUT); goto out; }
        write32(b,0,i,get32(input+i));
    }
    e = guard(b); if (e) { e = fault(b,e); goto out; }
    if (read32(b,0,0x100)&1u) { e = fault(b,EBUSY); goto out; }
    if (now_ns() >= end) { e = fault(b,ETIMEDOUT); goto out; }
    write32(b,0,0x100,1u); b->status.sent++;
    do {
        if (now_ns() >= end || ++loops > 300000) { e = ETIMEDOUT; break; }
        e = collect(b);
        if (e == ENOSPC) e = 0; /* no RX ACK; peer TX consumption is independent */
        if (e) break;
        if (!(read32(b,0,0x100)&1u)) break;
        delay(10);
    } while (1);
    if (!e && now_ns() >= end) e = ETIMEDOUT;
    if (!e) e = guard(b);
    if (e) e = fault(b,e);
    else { b->tx_pending = false; b->status.tx_consumed++; }
out:
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
int x9_vcp_bus_send(struct x9_vcp_bus *b, const uint8_t packet[X9_VCP_WIRE_BYTES], uint32_t timeout_ms)
{
    uint64_t end = 0;
    int e = deadline(timeout_ms,&end);
    return e ? e : send_until(b,packet,end,NULL);
}
static int poll_for(struct x9_vcp_bus *b, const struct x9_vcp_bus_call_log *call)
{
    int e;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b);
    if (!e && b->call != call) e = EBUSY;
    if (!e && b->status.phase != X9_VCP_BUS_RUNNING) e = EBUSY;
    if (!e) { e = collect(b); if (e && e != ENOSPC) e = fault(b,e); }
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
int x9_vcp_bus_poll(struct x9_vcp_bus *b)
{ return poll_for(b,NULL); }
static int receive_for(struct x9_vcp_bus *b, uint8_t out[X9_VCP_WIRE_BYTES],
    const struct x9_vcp_bus_call_log *call)
{
    int e;
    if (!out) return EINVAL;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b);
    if (!e && b->call != call) e = EBUSY;
    /* Already-owned bytes remain readable for fault handling after quarantine.
     * This does not resume MMIO, acknowledge or make stale epochs current. */
    if (!e && !b->status.queued) e = EAGAIN;
    if (!e) {
        memcpy(out,b->ring[b->head],X9_VCP_WIRE_BYTES);
        b->head = (b->head + 1) % VCP_BUS_RING; b->status.queued--;
    }
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
int x9_vcp_bus_receive(struct x9_vcp_bus *b, uint8_t out[X9_VCP_WIRE_BYTES])
{ return receive_for(b,out,NULL); }
int x9_vcp_bus_get_status(struct x9_vcp_bus *b, struct x9_vcp_bus_status *out)
{
    int e;
    if (!out) return EINVAL;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b); if (!e) *out = b->status;
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
/* bus lock held; shared input is resident immutable host storage. */
static int check_shared_locked(struct x9_vcp_bus *b, uint64_t epoch,
    const struct x9_vcp_shm_region *shared)
{
    int e = identity(b);
    if (!e && (epoch != b->status.epoch || shared->iova != b->shared.iova ||
        shared->bytes != b->shared.bytes)) e = ESTALE;
    if (!e && (b->call || b->status.phase != X9_VCP_BUS_RUNNING ||
        b->status.ready_bitmap != 3)) e = EBUSY;
    if (!e) e = guard(b);
    return e;
}
int x9_vcp_bus_check_shared(struct x9_vcp_bus *b, uint64_t epoch,
    const struct x9_vcp_shm_region *shared)
{
    int e;
    if (!shared) return EINVAL;
    lck_mtx_lock(&vcp_bus_lock);
    e = check_shared_locked(b,epoch,shared);
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
int x9_vcp_bus_retain_shared(struct x9_vcp_bus *b, uint64_t epoch,
    const struct x9_vcp_shm_region *shared, uint64_t *token)
{
    unsigned index;
    int e;
    if (!shared || !token || *token) return EINVAL;
    lck_mtx_lock(&vcp_bus_lock);
    e = check_shared_locked(b,epoch,shared);
    if (!e && (!next_client_token || next_client_token == UINT64_MAX)) e = EOVERFLOW;
    if (!e) {
        for (index = 0; index < X9_VCP_INSTANCE_SLOT_COUNT; index++)
            if (!b->clients[index]) break;
        if (index == X9_VCP_INSTANCE_SLOT_COUNT) e = ENOSPC;
        else {
            b->clients[index] = next_client_token++;
            b->status.retained_clients++;
            *token = b->clients[index];
        }
    }
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
int x9_vcp_bus_release_shared(struct x9_vcp_bus *b, uint64_t epoch, uint64_t *token)
{
    unsigned index;
    int e;
    if (!token || !*token || *token == UINT64_MAX) return EINVAL;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b);
    if (!e && epoch != b->status.epoch) e = ESTALE;
    if (!e) {
        for (unsigned n = 0; n < X9_VCP_INSTANCE_SLOT_COUNT; n++)
            if (b->instances[n].status.host_token == *token &&
                !b->instances[n].status.retired) { e = EBUSY; break; }
    }
    if (!e) {
        for (index = 0; index < X9_VCP_INSTANCE_SLOT_COUNT; index++)
            if (b->clients[index] == *token) break;
        if (index == X9_VCP_INSTANCE_SLOT_COUNT) e = ESTALE;
        else if (!b->status.retained_clients) e = EFAULT;
        else {
            b->clients[index] = 0; b->status.retained_clients--; *token = 0;
        }
    }
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
static bool same_init(const struct x9_vcp_init *a, const struct x9_vcp_init *z)
{
    return a->service_id == z->service_id && a->context_id == z->context_id &&
        a->instance_token == z->instance_token;
}
static bool same_ops(const struct x9_vcp_bus_reverse_ops *a,
    const struct x9_vcp_bus_reverse_ops *z)
{
    /* Compare defined fields, not uninitialized structure padding. */
    return a->cookie == z->cookie && a->route_enter == z->route_enter &&
        a->route_leave == z->route_leave && a->prepare_output == z->prepare_output &&
        a->prepare_start == z->prepare_start &&
        a->observe_start_reply == z->observe_start_reply &&
        a->observe_query_reply == z->observe_query_reply &&
        a->notification == z->notification &&
        a->recheck == z->recheck && a->shm == z->shm && a->alloc == z->alloc &&
        a->clock == z->clock && a->wait == z->wait && a->vp == z->vp;
}
int x9_vcp_bus_register_instance(struct x9_vcp_bus *b, uint64_t epoch,
    uint64_t token, const struct x9_vcp_init *init, const struct x9_vcp_bus_reverse_ops *ops)
{
    struct x9_vcp_init q;
    struct x9_vcp_bus_reverse_ops handlers;
    uint8_t encoded[X9_VCP_WIRE_BYTES];
    unsigned hold, slot;
    int e;
    if (!init || !ops || !token || token == UINT64_MAX) return EINVAL;
    q = *init; handlers = *ops;
    if (!handlers.cookie || !handlers.route_enter || !handlers.route_leave) return EINVAL;
    e = x9_vcp_encode_init(&q,encoded);
    if (e) return e;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b);
    if (!e) e = check_shared_locked(b,epoch,&b->shared);
    if (!e) {
        for (hold = 0; hold < X9_VCP_INSTANCE_SLOT_COUNT; hold++)
            if (b->clients[hold] == token) break;
        if (hold == X9_VCP_INSTANCE_SLOT_COUNT) e = ESTALE;
    }
    if (!e) {
        for (slot = 0; slot < X9_VCP_INSTANCE_SLOT_COUNT; slot++) {
            const struct x9_vcp_bus_instance_status *s = &b->instances[slot].status;
            if (s->instance_token == q.instance_token || s->host_token == token) {
                e = EEXIST; break;
            }
        }
    }
    if (!e) {
        for (slot = 0; slot < X9_VCP_INSTANCE_SLOT_COUNT; slot++)
            if (!b->instances[slot].status.instance_token) break;
        if (slot == X9_VCP_INSTANCE_SLOT_COUNT) e = ENOSPC;
        else {
            struct vcp_bus_instance *r = &b->instances[slot];
            r->init = q; r->ops = handlers;
            r->status.epoch = epoch; r->status.host_token = token;
            r->status.instance_token = q.instance_token;
            b->status.registered_instances++; b->status.retained_routes++;
        }
    }
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
static struct vcp_bus_instance *find_host(struct x9_vcp_bus *b, uint64_t token)
{
    for (unsigned n = 0; n < X9_VCP_INSTANCE_SLOT_COUNT; n++)
        if (b->instances[n].status.host_token == token) return &b->instances[n];
    return NULL;
}
int x9_vcp_bus_unregister_instance(struct x9_vcp_bus *b, uint64_t epoch, uint64_t token)
{
    struct vcp_bus_instance *r = NULL;
    int e;
    if (!token || token == UINT64_MAX) return EINVAL;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b);
    if (!e && epoch != b->status.epoch) e = ESTALE;
    if (!e) { r = find_host(b,token); if (!r) e = ESTALE; }
    if (!e && r->status.retired) e = EALREADY;
    if (!e && (b->call || (r->status.exposed && b->status.phase != X9_VCP_BUS_STOPPED))) e = EBUSY;
    if (!e && !b->status.retained_routes) e = EFAULT;
    if (!e) {
        r->status.retired = 1; memset(&r->ops,0,sizeof(r->ops));
        b->status.retained_routes--; /* keep identity and final journal */
    }
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
int x9_vcp_bus_get_instance_status(struct x9_vcp_bus *b, uint64_t epoch,
    uint64_t token, struct x9_vcp_bus_instance_status *out)
{
    struct vcp_bus_instance *r = NULL;
    int e;
    if (!out || !token || token == UINT64_MAX) return EINVAL;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b);
    if (!e && epoch != b->status.epoch) e = ESTALE;
    if (!e) { r = find_host(b,token); if (!r) e = ESTALE; }
    if (!e) *out = r->status;
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
/* Captured vcp_wait_core_stop_timeout 4418..45b0 reads cfg+70, requires
 * gated(bit0), H0 halt(bit1), optional H1 halt(bit2), and !AXI busy(bit4).
 * The twohart flags are not interchangeable: CURRENT VCP=1, MMUP=0.
 * Unlike the stock diagnostic timeout path, this never continues on timeout.
 * No SHUT/reset mutation is justified by this read-only verification. In
 * particular a partial bootstrap fault needs real owner recovery, not a
 * guessed normal-running shutdown sequence or a constant 0x34 signature. */
static int verify_halted(struct x9_vcp_bus *b)
{
    uint64_t end = 0;
    int e = deadline(500,&end);
    if (e) return e;
    b->status.halt_checked_bitmap = 0;
    for (unsigned poll = 0; poll < 50000; poll++) {
        uint32_t checked = 0;
        e = guard(b);
        if (e) return e;
        if (now_ns() >= end) return ETIMEDOUT;
        for (unsigned sample = 0; sample < 2; sample++) {
            for (unsigned core = 0; core < 2; core++)
                b->status.halt_status[sample][core] = read32(b,3+core,0x70);
        }
        b->status.halt_samples++;
        e = guard(b);
        if (e) return e;
        if (now_ns() >= end) return ETIMEDOUT;
        for (unsigned core = 0; core < 2; core++) {
            uint32_t need = b->layout.twohart[core] ? 7u : 3u;
            uint32_t a = b->status.halt_status[0][core];
            uint32_t z = b->status.halt_status[1][core];
            /* Ignore reserved/debug bits, including absent MMUP H1. BOTH
             * independent physical samples must show all required idle
             * bits; sampling only one core or checking AXI alone is unsafe. */
            if ((a & (need|16u)) == need && (z & (need|16u)) == need)
                checked |= 1u << core;
        }
        if (checked == 3) {
            b->status.halt_checked_bitmap = checked;
            return 0;
        }
        delay(10);
    }
    return ETIMEDOUT;
}
int x9_vcp_bus_stop(struct x9_vcp_bus *b)
{
    int e;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b); if (e) goto out;
    if (!b->exposed || b->stop_attempted || b->status.phase == X9_VCP_BUS_STOPPED) {
        e = EBUSY; goto out;
    }
    b->stop_attempted = true;
    /* Reject future MMIO/start/send while keeping the object undiscardable.
     * Do not hold bus_lock while an actual owner joins bus-using workers. */
    b->status.phase = X9_VCP_BUS_STOPPING;
    lck_mtx_unlock(&vcp_bus_lock);
    e = x9_mm_power_validate_pin(b->owner.power_token,b->power_child,X9_MM_POWER_VCP);
    if (!e) e = b->owner.halt_and_drain(b->owner.cookie,b->status.epoch);
    lck_mtx_lock(&vcp_bus_lock);
    /* Callback return0 is not physical quiescence. The retained native MM
     * child plus owner clock/reset authority must still permit actual reads.
     * Any ambiguity retains bus/IO mappings/pins, with no second stop attempt. */
    if (!e) e = verify_halted(b);
    if (e) e = fault(b,e);
    else { b->status.phase = X9_VCP_BUS_STOPPED; b->tx_pending = false; }
out:
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
int x9_vcp_bus_discard(struct x9_vcp_bus **slot)
{
    struct x9_vcp_bus *b;
    int e;
    if (!slot) return EINVAL;
    lck_mtx_lock(&vcp_bus_lock);
    b = *slot; e = identity(b);
    if (!e && (b->call || b->status.queued || b->status.retained_clients || b->status.retained_routes ||
        (b->exposed && b->status.phase != X9_VCP_BUS_STOPPED))) e = EBUSY;
    /* STOPPED is not permission to use an old snapshot forever. Retained
     * ownership plus fresh physical idle must still hold before unpin/free.
     * A spontaneously restarted/changed core is quarantined, never reset. */
    if (!e && b->exposed) {
        e = verify_halted(b);
        if (e) e = fault(b,e);
    }
    if (!e) e = b->owner.resources_retired(b->owner.cookie,b->status.epoch);
    if (!e && b->power_child)
        e = x9_mm_power_unpin(b->owner.power_token,&b->power_child,X9_MM_POWER_VCP);
    if (!e) { *slot = NULL; free_bus(b); }
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}

/* Reserve IO for the entire control transaction, not just a single send.
 * Keep bus lifetime pinned while reverse callbacks execute outside the lock. */
static int call_begin(struct x9_vcp_bus *b, const struct x9_vcp_init *init,
    const struct x9_vcp_bus_reverse_ops *ops, struct x9_vcp_bus_call_log *log,
    bool *service_ready)
{
    struct vcp_bus_instance *r = NULL;
    int e;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b);
    if (!e && (b->call || b->status.phase != X9_VCP_BUS_RUNNING ||
        b->status.ready_bitmap != 3 || b->tx_pending)) e = EBUSY;
    if (!e) {
        for (unsigned n = 0; n < X9_VCP_INSTANCE_SLOT_COUNT; n++)
            if (b->instances[n].status.instance_token == init->instance_token) {
                r = &b->instances[n]; break;
            }
        if (!r || r->status.retired || r->status.epoch != b->status.epoch ||
            !same_init(&r->init,init) || !same_ops(&r->ops,ops)) e = ESTALE;
    }
    if (!e) { e = guard(b); if (e) e = fault(b,e); }
    if (!e) {
        b->caller = r; b->call = log; log->epoch = b->status.epoch;
        *service_ready = b->status.service_shm_replied != 0;
    }
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
static int call_check(struct x9_vcp_bus *b, const struct x9_vcp_bus_call_log *log,
    uint64_t end)
{
    int e;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b);
    if (!e && (b->call != log || log->epoch != b->status.epoch)) e = ESTALE;
    if (!e && b->status.phase != X9_VCP_BUS_RUNNING)
        e = b->status.last_errno ? (int)b->status.last_errno : ECANCELED;
    if (!e && now_ns() >= end) e = ETIMEDOUT;
    if (!e) e = guard(b);
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
static int call_end(struct x9_vcp_bus *b, struct x9_vcp_bus_call_log *log,
    uint64_t end, int error, bool prepared_service)
{
    int e;
    lck_mtx_lock(&vcp_bus_lock);
    e = identity(b);
    if (!e && b->call != log) e = ESTALE;
    if (!e) {
        if (!error && b->status.phase != X9_VCP_BUS_RUNNING)
            error = b->status.last_errno ? (int)b->status.last_errno : ECANCELED;
        if (!error && now_ns() >= end) error = ETIMEDOUT;
        if (!error) error = guard(b);
        /* A concurrent stop may be joining this call: do not clobber its
         * STOPPING/STOPPED phase. No retry, inferred rollback or DMA free. */
        if (error && b->status.phase == X9_VCP_BUS_RUNNING) error = fault(b,error);
        if (!error && prepared_service) b->status.service_shm_replied = 1;
        b->caller = NULL; b->call = NULL;
        e = error;
    }
    log->last_errno = (uint32_t)e;
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
static int reverse_reply(const struct x9_vcp_init *q,
    const struct x9_vcp_bus_reverse_ops *ops, uint64_t epoch, uint64_t end,
    const uint8_t in[X9_VCP_WIRE_BYTES], uint8_t out[X9_VCP_WIRE_BYTES], bool allow_shm)
{
    uint32_t message = get32(in+8);
    int e;
    memset(out,0,X9_VCP_WIRE_BYTES);
    switch (message) {
    case X9_VCP_CHECK_CODEC: {
        struct x9_vcp_codec_check r;
        e = x9_vcp_parse_codec_check(q,in,X9_VCP_WIRE_BYTES,&r);
        if (!e) e = x9_vcp_encode_hevc_codec_reply(q,X9_VCP_FOURCC_HEVC,&r,out);
        return e;
    }
    case X9_VCP_MEM_ALLOC: {
        /* Firmware worker 327ef6..327fc2 dequeues AP's message FIRST, then
         * calls 11782 to request initial SHM before dispatching INIT/QUERY.
         * Waiting for unsolicited c005 before sending would deadlock. */
        if (get32(in+32) == X9_VCP_MEM_SHM) {
            struct x9_vcp_shm_request r;
            struct x9_vcp_shm_mapping m = {0};
            e = x9_vcp_parse_shm_request(in,X9_VCP_WIRE_BYTES,&r);
            if (!e && !allow_shm) e = EALREADY;
            if (!e && (!ops || !ops->shm)) e = ENOTSUP;
            if (!e) e = ops->shm(ops->cookie,epoch,end,&r,&m);
            if (!e) e = x9_vcp_encode_shm_reply(&r,&m,out);
        } else {
            struct x9_vcp_mem_request r;
            struct x9_vcp_mem_allocation a = {0};
            e = x9_vcp_parse_mem_request(q,in,X9_VCP_WIRE_BYTES,&r);
            if (!e && (!ops || !ops->alloc)) e = ENOTSUP;
            if (!e) e = ops->alloc(ops->cookie,epoch,end,&r,&a);
            if (!e) e = x9_vcp_encode_mem_alloc_reply(q,&r,&a,out);
        }
        return e;
    }
    case X9_VCP_LOCK_CORE: case X9_VCP_UNLOCK_CORE:
    case X9_VCP_LOCK_LAT: case X9_VCP_UNLOCK_LAT: {
        struct x9_vcp_clock_request r;
        e = x9_vcp_parse_clock_request(q,in,X9_VCP_WIRE_BYTES,&r);
        if (!e && (!ops || !ops->clock)) e = ENOTSUP;
        if (!e) e = ops->clock(ops->cookie,epoch,end,&r);
        if (!e) e = x9_vcp_encode_clock_reply(q,&r,1,out);
        return e;
    }
    case X9_VCP_WAITISR: {
        struct x9_vcp_wait_request r;
        enum x9_vcp_wait_outcome outcome = X9_VCP_WAIT_PENDING;
        uint64_t now = now_ns(), wait_end = end;
        /* Captured 500ms per-WAITISR ceiling AND the whole-call deadline. */
        if (now > UINT64_MAX - (uint64_t)X9_VCP_WAITISR_TIMEOUT_MS*1000000) return EOVERFLOW;
        if (now + (uint64_t)X9_VCP_WAITISR_TIMEOUT_MS*1000000 < wait_end)
            wait_end = now + (uint64_t)X9_VCP_WAITISR_TIMEOUT_MS*1000000;
        e = x9_vcp_parse_wait_request(q,in,X9_VCP_WIRE_BYTES,&r);
        if (!e && (!ops || !ops->wait)) e = ENOTSUP;
        if (!e) e = ops->wait(ops->cookie,epoch,wait_end,&r,&outcome);
        if (!e) e = x9_vcp_encode_wait_reply(q,&r,outcome,out);
        return e;
    }
    case X9_VCP_GET_KERNEL_PARAM: {
        struct x9_vcp_vp_request r;
        struct x9_vcp_common_snapshot common = {0};
        uint32_t bitdepth = 0;
        e = x9_vcp_parse_vp_request(q,in,X9_VCP_WIRE_BYTES,&r);
        if (!e && (!ops || !ops->vp)) e = ENOTSUP;
        if (!e) e = ops->vp(ops->cookie,epoch,end,&r,&common,&bitdepth);
        if (!e) e = x9_vcp_encode_vp_reply(q,&r,&common,bitdepth,out);
        return e;
    }
    default:
        /* Includes c006: record matching alone cannot justify d006/free.
         * No guessing another instance's ACK/notification. Keep the bytes. */
        return ENOTSUP;
    }
}
/* The reservation freezes registry/cookies even while stop changes the bus
 * phase. Queries of route evidence take bus_lock; never mutate journal bytes
 * without it. Packet identity is NOT a firmware pointer or context lookup. */
static int route_packet_begin(struct x9_vcp_bus *b, struct x9_vcp_bus_call_log *log,
    struct vcp_bus_instance **recipient, struct x9_vcp_bus_route_journal *journal)
{
    uint64_t token = get64(log->last_rx+16);
    struct vcp_bus_instance *r = NULL;
    bool shm = get32(log->last_rx+8) == X9_VCP_MEM_ALLOC &&
        get32(log->last_rx+32) == X9_VCP_MEM_SHM;
    int e = 0;
    memset(journal,0,sizeof(*journal)); *recipient = NULL;
    lck_mtx_lock(&vcp_bus_lock);
    if (b->call != log || !b->caller || log->epoch != b->status.epoch) e = ESTALE;
    if (!e) {
        if (shm) r = b->caller; /* sentinel/type8 is bus-global, typed parser below */
        else for (unsigned n = 0; n < X9_VCP_INSTANCE_SLOT_COUNT; n++)
            if (b->instances[n].status.instance_token == token) {
                r = &b->instances[n]; break;
            }
        if (!r || r->status.retired || !r->status.exposed ||
            r->status.epoch != log->epoch) e = ESTALE;
    }
    if (!e && (r->status.packets == UINT64_MAX ||
        (r != b->caller && r->status.foreign_packets == UINT64_MAX))) e = EOVERFLOW;
    if (!e) {
        journal->epoch = log->epoch; journal->instance_token = r->init.instance_token;
        journal->sequence = ++r->status.packets;
        journal->foreign = r != b->caller;
        memcpy(journal->rx,log->last_rx,sizeof(journal->rx));
        if (journal->foreign) { r->status.foreign_packets++; log->foreign_packets++; }
        log->routed_packets++; log->last_recipient_token = r->init.instance_token;
        r->status.last = *journal; *recipient = r;
    }
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
static void route_packet_end(struct x9_vcp_bus *b, struct vcp_bus_instance *r,
    struct x9_vcp_bus_route_journal *journal, int error)
{
    journal->last_errno = (uint32_t)error;
    lck_mtx_lock(&vcp_bus_lock);
    if (!b->call || !b->caller || r->status.retired ||
        journal->epoch != b->status.epoch)
        panic("X9 VCP: lost routed packet reservation");
    r->status.last = *journal;
    lck_mtx_unlock(&vcp_bus_lock);
    /* Preserve recipient error state and unlock AFTER the physical reply or
     * its failure. No owner-state lock is acquired while bus_lock is held. */
    if (journal->foreign && journal->entered) r->ops.route_leave(r->ops.cookie,journal);
}
static int call_expose(struct x9_vcp_bus *b, struct x9_vcp_bus_call_log *log)
{
    int e = 0;
    lck_mtx_lock(&vcp_bus_lock);
    if (b->call != log || !b->caller || log->epoch != b->status.epoch) e = ESTALE;
    else b->caller->status.exposed = 1; /* before ANY uncertain wire attempt */
    lck_mtx_unlock(&vcp_bus_lock);
    return e;
}
/* Shared by every RX path. Only THIS caller's parsed b005 may fill output
 * and terminate its a005 wait; foreign b005/e000 are routed but never ACKs.
 * SHM is the existing one-time bus service negotiation, not an instance. */
static int dispatch_packet(struct x9_vcp_bus *b, uint64_t end,
    struct x9_vcp_bus_call_log *log, bool *service_ready, bool *prepared_service,
    struct x9_vcp_output_notification *output)
{
    struct x9_vcp_bus_route_journal journal;
    struct vcp_bus_instance *r = NULL;
    uint32_t message = get32(log->last_rx+8);
    int e = route_packet_begin(b,log,&r,&journal);
    if (e) return e;
    if (message == X9_VCP_INIT_DONE || message == X9_VCP_START_DONE ||
        message == X9_VCP_QUERY_CAP_DONE) e = EPROTO;
    if (!e && journal.foreign) {
        journal.enter_attempted = 1;
        e = r->ops.route_enter(r->ops.cookie,log->epoch,end);
        if (!e) journal.entered = 1;
    }
    if (!e) e = call_check(b,log,end);
    if (!e && (message == X9_VCP_FRAME_BUFFER_DONE || message == X9_VCP_OUTPUT_ASYNC)) {
        struct x9_vcp_output_notification n;
        journal.notification = 1;
        e = x9_vcp_parse_output_notification(&r->init,log->last_rx,X9_VCP_WIRE_BYTES,&n);
        /* Retain signed firmware error too, but never a foreign notification. */
        if (!e && output && !journal.foreign && n.message_id == X9_VCP_FRAME_BUFFER_DONE)
            *output = n;
        if (!e && !r->ops.notification) e = ENOTSUP;
        if (!e) e = r->ops.notification(r->ops.cookie,log->epoch,end,&n);
        if (!e && n.firmware_status) e = EIO;
        if (!e) e = call_check(b,log,end);
        if (!e) log->notifications++;
    } else if (!e) {
        e = reverse_reply(&r->init,&r->ops,log->epoch,end,log->last_rx,
            journal.reply,!*service_ready);
        if (!e) journal.reply_encoded = 1;
        if (!e) e = call_check(b,log,end);
        if (!e) {
            journal.reply_attempted = 1;
            e = send_until(b,journal.reply,end,log);
        }
        if (!e) {
            journal.reply_consumed = 1; log->reverse_replies++;
            if (message == X9_VCP_MEM_ALLOC && get32(log->last_rx+32) == X9_VCP_MEM_SHM) {
                *service_ready = true; *prepared_service = true;
            }
        }
    }
    route_packet_end(b,r,&journal,e);
    return e;
}
/* Already initialized service only. A control ACK found BEFORE a new request
 * belongs to an older operation, even if its alias/token matches. Dispatch
 * legitimate queued async/reverse work, but never carry an old ACK across the
 * next send. This is an RX baseline, NOT firmware/DMA producer exclusion. */
static int dispatch_pending(struct x9_vcp_bus *b, uint64_t end,
    struct x9_vcp_bus_call_log *log)
{
    bool service_ready = true, prepared_service = false;
    return dispatch_packet(b,end,log,&service_ready,&prepared_service,NULL);
}
static int control_rx_baseline(struct x9_vcp_bus *b, uint64_t end,
    struct x9_vcp_bus_call_log *log)
{
    int e = 0;
    for (unsigned n = 0; n < 64 && !e; n++) {
        e = call_check(b,log,end);
        if (!e) e = receive_for(b,log->last_rx,log);
        if (e == EAGAIN) {
            e = poll_for(b,log);
            if (e == ENOSPC) e = 0;
            if (!e) e = receive_for(b,log->last_rx,log);
            if (e == EAGAIN) {
                log->queue_empty_observed = 1;
                return 0;
            }
        }
        if (!e) {
            log->received++;
            e = call_check(b,log,end);
        }
        if (!e) e = dispatch_pending(b,end,log);
    }
    if (!e) { log->batch_limit_reached = 1; e = EOVERFLOW; }
    return e;
}
static int control_call(struct x9_vcp_bus *b, const struct x9_vcp_init *init,
    const struct x9_vcp_query *query, const struct x9_vcp_bus_reverse_ops *ops,
    uint64_t end, const uint8_t request[X9_VCP_WIRE_BYTES],
    struct x9_vcp_init_result *ir, struct x9_vcp_query_result *qr,
    const struct x9_vcp_init_result *initialized, const struct x9_vcp_start *start,
    struct x9_vcp_start_result *sr,
    const struct x9_vcp_reset *reset, struct x9_vcp_reset_result *rr,
    struct x9_vcp_bus_call_log *log)
{
    uint32_t expected = reset ? X9_VCP_RESET_DONE : start ? X9_VCP_START_DONE :
        query ? X9_VCP_QUERY_CAP_DONE : X9_VCP_INIT_DONE;
    unsigned loops = 0;
    bool service_ready = false, prepared_service = false;
    int e = call_begin(b,init,ops,log,&service_ready);
    if (e) { log->last_errno = (uint32_t)e; return e; }
    e = call_check(b,log,end);
    if (!e && (start || reset)) {
        /* A live service/instance must already exist. Do not make START the
         * first AP command and bootstrap SHM behind the preparation callback. */
        if (!service_ready || !b->caller->status.exposed) e = ENXIO;
        else e = control_rx_baseline(b,end,log);
        if (!e && start) {
            log->prepare_attempted = 1;
            e = ops->prepare_start(ops->cookie,log->epoch,end,init,initialized,start);
            if (!e) e = call_check(b,log,end);
            if (!e) log->prepared = 1;
        }
    }
    if (!e && !start && !reset && service_ready)
        e = control_rx_baseline(b,end,log);
    if (!e) e = call_expose(b,log);
    if (!e) {
        log->request_attempted = 1; /* conservative: even an uncertain send */
        e = send_until(b,request,end,log);
    }
    while (!e) {
        e = call_check(b,log,end);
        if (e) break;
        if (++loops > 300000) { e = ETIMEDOUT; break; }
        /* Drain owned bytes first: ENOSPC must not starve reverse dispatch.
         * Other readers are excluded even while a callback is executing. */
        e = receive_for(b,log->last_rx,log);
        if (e == EAGAIN) {
            e = poll_for(b,log);
            if (e == ENOSPC) e = 0;
            if (!e) delay(10);
            continue;
        }
        if (e) break;
        log->received++;
        e = call_check(b,log,end); /* do not dispatch after concurrent stop */
        if (e) break;
        if (get32(log->last_rx+8) == expected) {
            struct vcp_bus_instance *r = NULL;
            struct x9_vcp_bus_route_journal journal;
            e = route_packet_begin(b,log,&r,&journal);
            if (e) break;
            /* Whole-call serialization leaves no legitimate foreign control
             * ACK outstanding. Never enter foreign state for a stale ACK. */
            if (journal.foreign) e = EPROTO;
            else if (reset) {
                e = x9_vcp_parse_reset_reply(init,log->last_rx,X9_VCP_WIRE_BYTES,rr);
                log->ack_matched = !e || (e == EIO && rr->firmware_status != 0);
            } else if (start) {
                e = x9_vcp_parse_start_reply(init,log->last_rx,X9_VCP_WIRE_BYTES,sr);
                log->ack_matched = !e || (e == EIO && sr->firmware_status != 0);
                if (!e && ops->observe_start_reply) {
                    e = call_check(b,log,end);
                    if (!e) e = ops->observe_start_reply(ops->cookie,log->epoch,end,
                        init,initialized,start,sr,log);
                }
            } else if (query) {
                e = x9_vcp_parse_query_reply(query,log->last_rx,X9_VCP_WIRE_BYTES,qr);
                log->ack_matched = !e || (e == EIO && qr->firmware_status != 0);
                if (!e && ops->observe_query_reply)
                    e = ops->observe_query_reply(ops->cookie,log->epoch,end,query,qr);
            } else {
                e = x9_vcp_parse_init_reply(init,log->last_rx,X9_VCP_WIRE_BYTES,ir);
                log->ack_matched = !e || (e == EIO && ir->firmware_status != 0);
            }
            route_packet_end(b,r,&journal,e);
            break;
        }
        e = dispatch_packet(b,end,log,&service_ready,&prepared_service,NULL);
    }
    return call_end(b,log,end,e,prepared_service);
}
int x9_vcp_bus_init(struct x9_vcp_bus *b, const struct x9_vcp_init *init,
    const struct x9_vcp_bus_reverse_ops *ops, uint32_t timeout_ms,
    struct x9_vcp_init_result *result, struct x9_vcp_bus_call_log *log)
{
    struct x9_vcp_init q;
    struct x9_vcp_bus_reverse_ops handlers = {0};
    uint8_t request[X9_VCP_WIRE_BYTES];
    uint64_t end = 0;
    int e;
    if (!init || !result || !log) return EINVAL;
    q = *init; if (ops) handlers = *ops;
    memset(result,0,sizeof(*result)); memset(log,0,sizeof(*log));
    e = deadline(timeout_ms,&end);
    if (!e) e = x9_vcp_encode_init(&q,request);
    if (!e) e = control_call(b,&q,NULL,&handlers,end,request,result,NULL,NULL,NULL,NULL,NULL,NULL,log);
    /* A matched alias is not usable after timeout/cancellation/guard loss. */
    if (e) result->firmware_vsi_address = 0;
    log->last_errno = (uint32_t)e;
    return e;
}
int x9_vcp_bus_query(struct x9_vcp_bus *b, const struct x9_vcp_init *init,
    const struct x9_vcp_query *query, const struct x9_vcp_bus_reverse_ops *ops,
    uint32_t timeout_ms, struct x9_vcp_query_result *result, struct x9_vcp_bus_call_log *log)
{
    struct x9_vcp_init i;
    struct x9_vcp_query q;
    struct x9_vcp_bus_reverse_ops handlers = {0};
    uint8_t request[X9_VCP_WIRE_BYTES];
    uint64_t end = 0;
    int e;
    if (!init || !query || !result || !log) return EINVAL;
    i = *init; q = *query; if (ops) handlers = *ops;
    memset(result,0,sizeof(*result)); memset(log,0,sizeof(*log));
    e = deadline(timeout_ms,&end);
    if (!e && (q.service_id != i.service_id || q.context_id != i.context_id ||
        q.instance_token != i.instance_token || i.instance_token == UINT64_MAX ||
        q.data_token == UINT64_MAX)) e = EINVAL;
    if (!e) e = x9_vcp_encode_query(&q,request);
    if (!e) e = control_call(b,&i,&q,&handlers,end,request,NULL,result,NULL,NULL,NULL,NULL,NULL,log);
    if (e) result->firmware_data_address = 0;
    log->last_errno = (uint32_t)e;
    return e;
}
int x9_vcp_bus_decode_start(struct x9_vcp_bus *b, const struct x9_vcp_init *init,
    const struct x9_vcp_init_result *initialized, const struct x9_vcp_start *start,
    const struct x9_vcp_bus_reverse_ops *ops, uint32_t timeout_ms,
    struct x9_vcp_start_result *result, struct x9_vcp_bus_call_log *log)
{
    struct x9_vcp_init i;
    struct x9_vcp_init_result ready;
    struct x9_vcp_start q;
    struct x9_vcp_bus_reverse_ops handlers = {0};
    uint8_t request[X9_VCP_WIRE_BYTES];
    uint64_t end = 0;
    int e;
    if (!init || !initialized || !start || !result || !log) return EINVAL;
    i = *init; ready = *initialized; q = *start; if (ops) handlers = *ops;
    memset(result,0,sizeof(*result)); memset(log,0,sizeof(*log));
    e = deadline(timeout_ms,&end);
    if (!e && (!handlers.prepare_start || !i.instance_token ||
        i.instance_token == UINT64_MAX || i.service_id != X9_VCP_VDEC_SERVICE ||
        ready.firmware_status || !ready.firmware_vsi_address ||
        q.firmware_vsi_address != ready.firmware_vsi_address ||
        q.service_id != i.service_id || q.context_id != i.context_id)) e = EINVAL;
    if (!e) e = x9_vcp_encode_start(&q,request);
    if (!e) e = control_call(b,&i,NULL,&handlers,end,request,NULL,NULL,&ready,&q,result,NULL,NULL,log);
    log->last_errno = (uint32_t)e;
    return e;
}
int x9_vcp_bus_decode_reset(struct x9_vcp_bus *b, const struct x9_vcp_init *init,
    const struct x9_vcp_init_result *initialized, const struct x9_vcp_reset *reset,
    const struct x9_vcp_bus_reverse_ops *ops, uint32_t ms,
    struct x9_vcp_reset_result *result, struct x9_vcp_bus_call_log *log)
{
    struct x9_vcp_init i;
    struct x9_vcp_init_result ready;
    struct x9_vcp_reset q;
    struct x9_vcp_bus_reverse_ops handlers;
    uint8_t request[X9_VCP_WIRE_BYTES];
    uint64_t end = 0;
    int e;
    if (!init || !initialized || !reset || !ops || !result || !log) return EINVAL;
    i = *init; ready = *initialized; q = *reset; handlers = *ops;
    memset(result,0,sizeof(*result)); memset(log,0,sizeof(*log));
    e = deadline(ms,&end);
    if (!e && (!i.instance_token || i.instance_token == UINT64_MAX ||
        i.service_id != X9_VCP_VDEC_SERVICE || ready.firmware_status ||
        !ready.firmware_vsi_address || q.firmware_vsi_alias != ready.firmware_vsi_address ||
        q.service_id != i.service_id || q.context_id != i.context_id)) e = EINVAL;
    if (!e) e = x9_vcp_encode_reset(&q,request);
    if (!e) e = control_call(b,&i,NULL,&handlers,end,request,NULL,NULL,&ready,NULL,NULL,&q,result,log);
    log->last_errno = (uint32_t)e;
    return e;
}
int x9_vcp_bus_register_output(struct x9_vcp_bus *b, const struct x9_vcp_init *init,
    const struct x9_vcp_init_result *initialized,
    const struct x9_vcp_bus_reverse_ops *ops, uint32_t ms,
    struct x9_vcp_output_notification *out, struct x9_vcp_bus_call_log *log)
{
    struct x9_vcp_init i;
    struct x9_vcp_init_result ready;
    struct x9_vcp_bus_reverse_ops handlers;
    struct x9_vcp_output_request q = {0};
    struct x9_vcp_output_record record = {0};
    uint8_t request[X9_VCP_WIRE_BYTES];
    uint64_t end = 0;
    unsigned loops = 0;
    bool service_ready = false, prepared_service = false;
    int e;
    if (!init || !initialized || !ops || !out || !log) return EINVAL;
    i = *init; ready = *initialized; handlers = *ops;
    memset(out,0,sizeof(*out)); memset(log,0,sizeof(*log));
    e = deadline(ms,&end);
    if (!e && (!handlers.prepare_output || !handlers.notification ||
        !i.instance_token || i.instance_token == UINT64_MAX ||
        i.service_id != X9_VCP_VDEC_SERVICE || ready.firmware_status ||
        !ready.firmware_vsi_address)) e = EINVAL;
    if (e) { log->last_errno = (uint32_t)e; return e; }
    e = call_begin(b,&i,&handlers,log,&service_ready);
    if (e) { log->last_errno = (uint32_t)e; return e; }
    e = call_check(b,log,end);
    if (!e && (!service_ready || !b->caller->status.exposed)) e = ENXIO;
    if (!e) e = control_rx_baseline(b,end,log);
    if (!e) {
        log->prepare_attempted = 1;
        e = handlers.prepare_output(handlers.cookie,log->epoch,end,&i,&ready,&q,&record);
        if (!e) e = call_check(b,log,end);
        if (!e && (record.generation != log->epoch ||
            record.firmware_vsi_address != ready.firmware_vsi_address)) e = ESTALE;
        if (!e) e = x9_vcp_encode_output(X9_VCP_OUTPUT_ABI_MT6993_CAPTURE1,
            &i,&q,&record,request);
        if (!e) log->prepared = 1;
    }
    if (!e) {
        log->request_attempted = 1;
        e = send_until(b,request,end,log);
    }
    while (!e) {
        e = call_check(b,log,end);
        if (e) break;
        if (++loops > 300000) { e = ETIMEDOUT; break; }
        e = receive_for(b,log->last_rx,log);
        if (e == EAGAIN) {
            e = poll_for(b,log);
            if (e == ENOSPC) e = 0;
            if (!e) delay(10);
            continue;
        }
        if (e) break;
        log->received++;
        e = call_check(b,log,end);
        if (e) break;
        e = dispatch_packet(b,end,log,&service_ready,&prepared_service,out);
        if (out->message_id == X9_VCP_FRAME_BUFFER_DONE) break;
    }
    return call_end(b,log,end,e,false);
}
int x9_vcp_bus_service(struct x9_vcp_bus *b, const struct x9_vcp_init *init,
    const struct x9_vcp_init_result *initialized,
    const struct x9_vcp_bus_reverse_ops *ops, uint32_t max_packets, uint32_t ms,
    struct x9_vcp_bus_call_log *log)
{
    struct x9_vcp_init i;
    struct x9_vcp_init_result ready;
    struct x9_vcp_bus_reverse_ops handlers;
    uint8_t identity_request[X9_VCP_WIRE_BYTES];
    uint64_t end = 0;
    bool service_ready = false;
    int e;
    if (!init || !initialized || !ops || !log) return EINVAL;
    i = *init; ready = *initialized; handlers = *ops;
    memset(log,0,sizeof(*log));
    e = deadline(ms,&end);
    if (!e && (!max_packets || max_packets > 64 || !handlers.notification ||
        i.service_id != X9_VCP_VDEC_SERVICE || !i.instance_token ||
        i.instance_token == UINT64_MAX || ready.firmware_status ||
        !ready.firmware_vsi_address)) e = EINVAL;
    /* Validate the retained identity using the normal encoder, WITHOUT
     * sending INIT again or manufacturing a new outstanding control ACK. */
    if (!e) e = x9_vcp_encode_init(&i,identity_request);
    if (e) { log->last_errno = (uint32_t)e; return e; }
    e = call_begin(b,&i,&handlers,log,&service_ready);
    if (e) { log->last_errno = (uint32_t)e; return e; }
    e = call_check(b,log,end);
    if (!e && (!service_ready || !b->caller->status.exposed)) e = ENXIO;
    while (!e && log->received < max_packets) {
        e = call_check(b,log,end);
        if (e) break;
        /* Backpressure must not starve an already-owned reverse packet.
         * Poll only when software RX is empty; there is no idle spin loop. */
        e = receive_for(b,log->last_rx,log);
        if (e == EAGAIN) {
            e = poll_for(b,log);
            if (e == ENOSPC) e = 0;
            if (!e) e = receive_for(b,log->last_rx,log);
            if (e == EAGAIN) {
                log->queue_empty_observed = 1; e = 0; break;
            }
        }
        if (e) break;
        log->received++;
        e = call_check(b,log,end);
        if (e) break;
        e = dispatch_pending(b,end,log);
    }
    if (!e && log->received == max_packets) log->batch_limit_reached = 1;
    if (!e && handlers.recheck) {
        e = call_check(b,log,end);
        if (!e) e = handlers.recheck(handlers.cookie,log->epoch,end);
        if (!e) e = call_check(b,log,end);
    }
    return call_end(b,log,end,e,false);
}
