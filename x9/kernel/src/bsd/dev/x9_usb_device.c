/* Own-code MT6993 MTU3 USB2 device controller, OPPO 6.12 register/QMU model.
 * Device mode only: CDC ECM -> native BSD ue0, optional private bulk stream.
 * No DWC3 emulation, Apple VID, DMA addresses from userspace, or host VBUS.
 * EP0 vendor log requests are retained. All MMIO and DMA are worker-owned.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kern_event.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/sockio.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <net/if.h>
#include <net/if_private.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_ether.h>
#include <net/ether_if_module.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <netinet/in.h>
#include "x9_usb_device.h"
#include "x9_usb_log_protocol.h"

extern int sweet_x9_arg(const char *, int);
extern uint32_t sweet_log_copy(uint64_t *, char *, uint32_t);
extern uint64_t sweet_log_position(void);
extern void thread_terminate_self(void);
#define BIT(n) (1u << (n))
#define R x9_mtu3_read
#define W x9_mtu3_write
#define EP0 0x100
#define SETUP BIT(17)
#define RX BIT(16)
#define TX BIT(18)
#define DATAEND BIT(19)
#define STALL BIT(25)
#define SENTSTALL BIT(22)
#define EP0_W1C (RX | SETUP | SENTSTALL)
#define ACTIVE BIT(15)
#define HWO BIT(0)
#define QUEUE 16u
#define NET_MAX 1514u
#define DMA_BUFFER 4096u
#define DESC_STRIDE 256u

static LCK_GRP_DECLARE(usb_group, "x9_usb_device");
static LCK_MTX_DECLARE(usb_lock, &usb_group);
static struct x9_usb_status st = { .version = X9_USB_ABI };
static ifnet_t netif;
static unsigned started, packet_filter, suspended, failed;
static uint64_t lease, next_lease;
struct chunk { uint32_t n; uint8_t bytes[X9_USB_CHUNK]; };
struct queue { struct chunk v[QUEUE]; unsigned head, count; };
static struct queue net_tx, raw_tx, raw_rx;
static int channel;
static int bus_channel;
enum { COMMAND_DESCRIPTION = X9_USB_BUS_CYCLE + 1 };
static struct x9_usb_description description = {
    .version = X9_USB_DESCRIPTION_ABI, .vendor_id = 0x1209, .product_id = 1,
    .device_id = 0x200, .device_class = 0xef, .device_subclass = 2,
    .device_protocol = 1, .bcd_usb = 0x200,
    .manufacturer = "HelioDarwin", .product = "X9 USB Ethernet", .serial = "X9USBLOG1"
};
static uint32_t description_revision = 1;
_Static_assert(sizeof(struct x9_usb_description) == 224, "description ABI");
_Static_assert(sizeof(struct x9_usb_description_status) == 240, "description status ABI");
static struct {
    unsigned pending, running, done, abandoned, operation;
    uint32_t delay_ms;
    int result;
    uint64_t reconnect_at;
    struct x9_usb_description description;
} bus_command;
static const uint8_t phone_mac[6] = { 0x02, 0x58, 0x39, 0x55, 0x53, 0x02 };

struct gpd { uint32_t flags, next, buffer, length; };
_Static_assert(sizeof(struct gpd) == 16, "OPPO QMU GPD size");
struct endpoint {
    unsigned number, in, interrupt, enabled, busy, slot, requested, halted;
    uint8_t *va;
    uint64_t pa;
    uint64_t owner;
};
/* One pinned 16-KiB allocation per endpoint. Never freed while DMA may own it.
 * Ring entries occupy separate cache lines; data is in a separate 4-KiB span. */
static struct endpoint eps[5] = {
    { .number = 1, .in = 0 }, { .number = 1, .in = 1 },
    { .number = 2, .in = 1, .interrupt = 1 },
    { .number = 3, .in = 0 }, { .number = 3, .in = 1 }
};
static unsigned gen2, maxpacket, notification;
static uint64_t raw_owner;
static uint8_t control[X9LOG_BATCH_FRAME];
static unsigned sent, length, wanted;
static enum { CTRL_IDLE, CTRL_IN, CTRL_IN_END } phase;

static uint32_t csr0(const struct endpoint *e) { return (e->in ? 0x100u : 0x200u) + e->number * 16; }
static uint32_t qcsr(const struct endpoint *e) { return (e->in ? 0x500u : 0x600u) + e->number * 16; }
static uint32_t qhi(const struct endpoint *e) { return (e->in ? 0x480u : 0x4c0u) + e->number * 4; }
static uint32_t ebit(const struct endpoint *e) { return BIT(e->number + (e->in ? 0 : 16)); }
static struct gpd *descriptor(struct endpoint *e, unsigned slot) { return (struct gpd *)(e->va + slot * DESC_STRIDE); }
static void cache(void *p, unsigned n, int invalidate)
{
    uint64_t ctr; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t line = (uintptr_t)4 << ((ctr >> 16) & 15);
    uintptr_t a = (uintptr_t)p & ~(line - 1), end = (uintptr_t)p + n;
    for (; a < end; a += line) {
        if (invalidate) __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
        else __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
static void count_drop(void) { lck_mtx_lock(&usb_lock); st.dropped++; lck_mtx_unlock(&usb_lock); }
static void clear_queues(void)
{
    lck_mtx_lock(&usb_lock);
    st.dropped += net_tx.count + raw_tx.count + raw_rx.count;
    net_tx.head = net_tx.count = raw_tx.head = raw_tx.count = raw_rx.head = raw_rx.count = 0;
    lck_mtx_unlock(&usb_lock);
}
static void post_link(unsigned up)
{
    lck_mtx_lock(&usb_lock);
    if (st.link == up) { lck_mtx_unlock(&usb_lock); return; }
    st.link = up; lck_mtx_unlock(&usb_lock);
    if (!netif) return;
    ifnet_set_flags(netif, up ? IFF_RUNNING : 0, IFF_RUNNING);
    struct net_event_data data; bzero(&data, sizeof(data));
    data.if_family = IFNET_FAMILY_ETHERNET; data.if_unit = 0;
    snprintf(data.if_name, sizeof(data.if_name), "ue");
    struct kev_msg msg; bzero(&msg, sizeof(msg));
    msg.vendor_code = KEV_VENDOR_APPLE; msg.kev_class = KEV_NETWORK_CLASS;
    msg.kev_subclass = KEV_DL_SUBCLASS; msg.event_code = up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF;
    msg.dv[0].data_length = sizeof(data); msg.dv[0].data_ptr = &data;
    (void)kev_post_msg(&msg);
    printf("X9 USB: ue0 link %s (real configuration/alt/filter)\n", up ? "up" : "down");
}
static int stop_endpoint(struct endpoint *e)
{
    if (R(qcsr(e)) & ACTIVE) {
        W(qcsr(e), BIT(2));
        for (unsigned i = 0; i < 10000 && (R(qcsr(e)) & ACTIVE); i++) delay(1);
        if (R(qcsr(e)) & ACTIVE) return ETIMEDOUT;
    }
    if (e->in) W(csr0(e), (R(csr0(e)) & ~BIT(22)) | BIT(20));
    e->busy = 0;
    return 0;
}
static int disable_endpoint(struct endpoint *e)
{
    if (!e->enabled) return 0;
    int error = stop_endpoint(e);
    if (error) return error; /* retain allocation and all descriptor contents */
    uint32_t bit = BIT(e->number + (e->in ? 16 : 0));
    W(0x804, bit); W(0x804, 0);
    W(csr0(e), 0); W(csr0(e) + 4, 0); W(csr0(e) + 8, 0);
    W(0x70c, ebit(e)); e->enabled = e->halted = 0;
    return 0;
}
static void fault(int error, uint32_t detail)
{
    lck_mtx_lock(&usb_lock); failed = 1; lck_mtx_unlock(&usb_lock); post_link(0);
    W(0x2404, R(0x2404) & ~BIT(6));
    /* A stop timeout quarantines buffers for the remainder of this boot. No
     * automatic reset/retry can convert uncertain ownership into safe reuse. */
    for (unsigned i = 0; i < 5; i++) if (eps[i].enabled) (void)stop_endpoint(&eps[i]);
    clear_queues();
    lck_mtx_lock(&usb_lock); st.state = X9_USB_FAULT; st.error = error;
    st.qmu_error = detail; st.dma_errors++; st.generation++; lck_mtx_unlock(&usb_lock);
    printf("X9 USB: isolated error=%d QMU=%#x; DMA retained, no retry\n", error, detail);
}
static int configure_endpoint(struct endpoint *e, unsigned fifo)
{
    unsigned size = e->interrupt ? 16 : maxpacket;
    W(csr0(e), size | BIT(29));
    W(csr0(e) + 4, e->interrupt ? BIT(4) : 0); /* TYPE_INT / TYPE_BULK, slot=0 */
    W(csr0(e) + 8, (fifo >> 4) | (9u << 16) |
        (e->interrupt ? ((st.speed == 3 ? 8u : 16u) << 24) : 0));
    W(0x400, R(0x400) & ~ebit(e)); /* checksum disabled, like coherent Linux */
    if (e->in) W(0x404, R(0x404) | BIT(e->number));
    else W(0x40c, (R(0x40c) & ~BIT(e->number)) | BIT(e->number + 16));
    W(0x708, ebit(e));
    e->enabled = 1; e->busy = e->halted = 0; e->slot = 0;
    return 0;
}
static int submit(struct endpoint *e, const void *bytes, unsigned n)
{
    if (!e->enabled || e->busy || e->halted || failed || n > DMA_BUFFER) return EINVAL;
    int error = stop_endpoint(e); if (error) return error;
    unsigned next = e->slot ^ 1;
    struct gpd *g = descriptor(e, e->slot), *terminator = descriptor(e, next);
    bzero(g, sizeof(*g)); bzero(terminator, sizeof(*terminator));
    uint64_t buffer = e->pa + DMA_BUFFER, ng = e->pa + next * DESC_STRIDE;
    uint32_t ext = ((uint32_t)(buffer >> 32) << (gen2 ? 24 : 16)) |
                   ((uint32_t)(ng >> 32) << (gen2 ? 28 : 20));
    g->next = (uint32_t)ng; g->buffer = (uint32_t)buffer;
    if (e->in) {
        if (n && bytes != e->va + DMA_BUFFER) memcpy(e->va + DMA_BUFFER, bytes, n);
        g->flags = ext; g->length = n;
        if (!e->interrupt && n && !(n % maxpacket)) {
            if (gen2) g->flags |= BIT(6); else g->length |= BIT(29);
        }
    } else {
        g->flags = n << (gen2 ? 12 : 16); g->length = ext;
    }
    cache(e->va + DMA_BUFFER, DMA_BUFFER, 0);
    cache(terminator, DESC_STRIDE, 0);
    cache(g, DESC_STRIDE, 0);
    g->flags |= HWO | BIT(7); cache(g, DESC_STRIDE, 0); /* publish HWO last */
    e->requested = n; e->busy = 1;
    if (e->number == 3) {
        lck_mtx_lock(&usb_lock); e->owner = lease; lck_mtx_unlock(&usb_lock);
    }
    uint64_t start = e->pa + e->slot * DESC_STRIDE;
    W(qcsr(e) + 4, (uint32_t)start);
    W(qhi(e), (R(qhi(e)) & ~15u) | (uint32_t)(start >> 32));
    W(qcsr(e), BIT(0));
    return 0;
}
static void net_receive(const uint8_t *p, unsigned n)
{
    if (!netif || n < 14 || n > NET_MAX) { count_drop(); return; }
    mbuf_t m = NULL;
    if (mbuf_allocpacket(MBUF_DONTWAIT, n + 2, NULL, &m)) { count_drop(); return; }
    unsigned done = 0;
    for (mbuf_t b = m; b && done < n; b = mbuf_next(b)) {
        size_t room = mbuf_maxlen(b) - mbuf_leadingspace(b);
        if (b == m) {
            if (room < 16) break;
            (void)mbuf_setdata(b, (uint8_t *)mbuf_data(b) + 2, 0); room -= 2;
        }
        unsigned take = n - done; if (take > room) take = (unsigned)room;
        memcpy(mbuf_data(b), p + done, take); mbuf_setlen(b, take); done += take;
    }
    if (done != n) { mbuf_freem(m); count_drop(); return; }
    mbuf_pkthdr_setlen(m, n); mbuf_pkthdr_setrcvif(m, netif);
    mbuf_pkthdr_setheader(m, mbuf_data(m)); mbuf_adj(m, 14);
    struct ifnet_stat_increment_param inc; bzero(&inc, sizeof(inc));
    inc.packets_in = 1; inc.bytes_in = n;
    errno_t error = ifnet_input(netif, m, &inc); /* consumes m, including errors */
    lck_mtx_lock(&usb_lock); if (!error) st.net_rx_frames++; else st.dropped++; lck_mtx_unlock(&usb_lock);
}
static int complete(struct endpoint *e)
{
    if (!e->busy) return 0;
    struct gpd *g = descriptor(e, e->slot); cache(g, DESC_STRIDE, 1);
    if (g->flags & HWO) return 0;
    uint64_t current = R(qcsr(e) + 8) | ((uint64_t)((R(qhi(e)) >> 8) & 15) << 32);
    if (current == e->pa + e->slot * DESC_STRIDE) return 0; /* CPR publication may lag HWO */
    if (current != e->pa + (e->slot ^ 1) * DESC_STRIDE) return EIO;
    unsigned n = g->length & (gen2 ? 0xfffffu : 0xffffu);
    if (n > e->requested || (e->in && n != e->requested)) return EIO;
    int error = stop_endpoint(e); if (error) return error;
    e->slot ^= 1;
    lck_mtx_lock(&usb_lock); st.dma_completions++;
    if (e->in && !e->interrupt) {
        if (e->number == 1) st.net_tx_frames++; else st.raw_tx_bytes += n;
    }
    lck_mtx_unlock(&usb_lock);
    if (e->in && e->number == 1 && netif) ifnet_stat_increment_out(netif, 1, n, 0);
    if (!e->in) {
        cache(e->va + DMA_BUFFER, DMA_BUFFER, 1);
        if (e->number == 1) net_receive(e->va + DMA_BUFFER, n);
        else if (n) {
            lck_mtx_lock(&usb_lock);
            if (lease && lease == e->owner && raw_rx.count < QUEUE) {
                struct chunk *v = &raw_rx.v[(raw_rx.head + raw_rx.count) % QUEUE];
                v->n = n; memcpy(v->bytes, e->va + DMA_BUFFER, n); raw_rx.count++;
                st.raw_rx_bytes += n;
            } else st.dropped++;
            lck_mtx_unlock(&usb_lock);
        }
    }
    return 0;
}
static int kick_tx(struct endpoint *e, struct queue *q)
{
    if (e->busy || e->halted) return 0;
    lck_mtx_lock(&usb_lock);
    if (!q->count) { lck_mtx_unlock(&usb_lock); return 0; }
    struct chunk *v = &q->v[q->head];
    memcpy(e->va + DMA_BUFFER, v->bytes, v->n); unsigned n = v->n;
    q->head = (q->head + 1) % QUEUE; q->count--;
    lck_mtx_unlock(&usb_lock);
    /* submit copies this same, worker-owned buffer; no external queue lock */
    return submit(e, e->va + DMA_BUFFER, n);
}
static void data_poll(void)
{
    lck_mtx_lock(&usb_lock); uint64_t owner = lease; lck_mtx_unlock(&usb_lock);
    if (owner != raw_owner) {
        /* Closing a client cannot hand its in-flight RX or old FIFO contents
         * to a successor. Only this worker may drain/reset DMA ownership. */
        int error = disable_endpoint(&eps[3]);
        if (!error) error = disable_endpoint(&eps[4]);
        if (error) { fault(error, 3); return; }
        if (st.configuration && st.raw_enabled) {
            configure_endpoint(&eps[3], 512); configure_endpoint(&eps[4], 1024);
        }
        raw_owner = owner;
    }
    uint32_t txerr = R(0x780), rxerr = R(0x790), zlperr = R(0x7a0);
    uint32_t errors = (txerr & 0xe000eu) | (rxerr & 0xa000au) | (zlperr & 0xa0000u);
    if (errors) { fault(EIO, errors); return; }
    W(0x700, R(0x700)); W(0x710, R(0x710));
    for (unsigned i = 0; i < 5; i++) {
        int error = complete(&eps[i]); if (error) { fault(error, i + 1); return; }
    }
    int error = 0;
    if (st.configuration && st.data_alt && !suspended) {
        if (!eps[0].busy && !eps[0].halted) error = submit(&eps[0], NULL, 2048);
        if (!error) error = kick_tx(&eps[1], &net_tx);
    }
    if (!error && st.configuration && st.raw_enabled && !suspended) {
        lck_mtx_lock(&usb_lock); unsigned ready = lease && raw_rx.count < QUEUE; lck_mtx_unlock(&usb_lock);
        if (ready && !eps[3].busy && !eps[3].halted) error = submit(&eps[3], NULL, 512);
        if (!error) error = kick_tx(&eps[4], &raw_tx);
    }
    if (!error && notification && eps[2].enabled && !eps[2].busy && !eps[2].halted && !suspended) {
        uint8_t n[8] = { 0xa1, 0, (uint8_t)st.link, 0, 0, 0, 0, 0 };
        error = submit(&eps[2], n, sizeof(n)); notification = 0;
    }
    if (error) fault(error, 0);
}
static int reset_data(void)
{
    post_link(0); clear_queues();
    for (unsigned i = 0; i < 5; i++) { int e = disable_endpoint(&eps[i]); if (e) return e; }
    lck_mtx_lock(&usb_lock); st.configuration = st.data_alt = 0;
    st.generation++; st.state = X9_USB_READY; lck_mtx_unlock(&usb_lock);
    lck_mtx_lock(&usb_lock); packet_filter = 0; lck_mtx_unlock(&usb_lock);
    notification = suspended = 0;
    return 0;
}
static void bus_finish(int error)
{
    lck_mtx_lock(&usb_lock);
    bus_command.result = error; bus_command.done = 1;
    st.reserved &= ~X9_USB_BUS_CYCLING;
    unsigned operation = bus_command.operation, generation = st.generation;
    unsigned off = !!(st.reserved & X9_USB_BUS_FORCED_OFF);
    if (bus_command.abandoned) bus_command.pending = 0;
    thread_wakeup(&bus_channel);
    lck_mtx_unlock(&usb_lock);
    printf("X9 USB: IOKit worker op=%u result=%d forced-off=%u generation=%u; not mux/pairing\n",
        operation, error, off, generation);
}
static void bus_poll(void)
{
    lck_mtx_lock(&usb_lock);
    if (!bus_command.pending || bus_command.done) { lck_mtx_unlock(&usb_lock); return; }
    unsigned running = bus_command.running, operation = bus_command.operation;
    uint32_t delay_ms = bus_command.delay_ms;
    uint64_t reconnect_at = bus_command.reconnect_at;
    unsigned forced_off = st.reserved & X9_USB_BUS_FORCED_OFF;
    bus_command.running = 1;
    lck_mtx_unlock(&usb_lock);
    if (failed) { bus_finish(ENXIO); return; }
    if (!running && ((operation == X9_USB_BUS_ON && !forced_off) ||
        (operation == X9_USB_BUS_OFF && forced_off))) { bus_finish(0); return; }
    if (operation == COMMAND_DESCRIPTION) {
        /* A synchronous bus-off has already stopped all DMA. Revalidate the
         * wire state before changing anything host-visible. No reconnect. */
        if (R(0x2404) & BIT(6)) { fault(EIO, 0); bus_finish(EIO); return; }
        lck_mtx_lock(&usb_lock);
        description = bus_command.description; description_revision++;
        st.generation++;
        lck_mtx_unlock(&usb_lock);
        phase = CTRL_IDLE; sent = length = wanted = 0;
        bus_finish(0);
        return;
    }
    if (running && mach_absolute_time() < reconnect_at) return;
    int error = 0;
    if (!running && operation != X9_USB_BUS_ON) {
        /* The register writer remains the sole MMIO worker. First disconnect,
         * then STOP every QMU before discarding queued data or reconnecting. */
        W(0x2404, R(0x2404) & ~BIT(6));
        if (R(0x2404) & BIT(6)) error = EIO;
        if (!error) { lck_mtx_lock(&usb_lock); st.reserved |= X9_USB_BUS_FORCED_OFF; lck_mtx_unlock(&usb_lock); }
        if (!error) error = reset_data();
        phase = CTRL_IDLE; sent = length = wanted = 0;
        W(0x800, R(0x800) & ~(0x7fu << 24));
        /* Discard pre-disconnect EP0/common events. Do not execute an old
         * SET_CONFIGURATION while forced off and re-arm its QMU endpoints. */
        W(0x80, R(0x80) & 0x10001u);
        W(0x241c, R(0x241c));
        W(EP0, 64 | EP0_W1C);
        if (error) { fault(error, 0); bus_finish(error); return; }
        if (operation == X9_USB_BUS_OFF) { bus_finish(0); return; }
        clock_interval_to_deadline(delay_ms, NSEC_PER_MSEC, &reconnect_at);
        lck_mtx_lock(&usb_lock);
        bus_command.reconnect_at = reconnect_at; st.reserved |= X9_USB_BUS_CYCLING;
        lck_mtx_unlock(&usb_lock);
        return;
    }
    if (!running && operation == X9_USB_BUS_ON) {
        error = reset_data();
        if (error) { fault(error, 0); bus_finish(error); return; }
    }
    /* Events may have latched while disconnected; clear them before a fresh
     * connection, for both explicit bus-on and the second half of a cycle. */
    phase = CTRL_IDLE; sent = length = wanted = 0;
    W(0x800, R(0x800) & ~(0x7fu << 24));
    W(0x80, R(0x80) & 0x10001u); W(0x241c, R(0x241c));
    W(EP0, 64 | EP0_W1C);
    W(0x2404, R(0x2404) | BIT(6));
    if (!(R(0x2404) & BIT(6))) error = EIO;
    if (!error) { lck_mtx_lock(&usb_lock); st.reserved &= ~X9_USB_BUS_FORCED_OFF; lck_mtx_unlock(&usb_lock); }
    else fault(error, 0);
    bus_finish(error);
}
static int worker_command(unsigned operation, uint32_t delay_ms,
                          const struct x9_usb_description *replacement)
{
    uint64_t deadline; clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
    lck_mtx_lock(&usb_lock);
    if (st.state < X9_USB_READY) { lck_mtx_unlock(&usb_lock); return EAGAIN; }
    if (failed || st.state == X9_USB_FAULT) { lck_mtx_unlock(&usb_lock); return ENXIO; }
    if (bus_command.pending) { lck_mtx_unlock(&usb_lock); return EBUSY; }
    /* A cycle must not override a separately requested ForceOffBusEnable.
     * The operator must first issue ForceOffBusDisable explicitly. */
    if (operation == X9_USB_BUS_CYCLE && (st.reserved & X9_USB_BUS_FORCED_OFF)) {
        lck_mtx_unlock(&usb_lock); return EBUSY;
    }
    if (operation == COMMAND_DESCRIPTION && (!(st.reserved & X9_USB_BUS_FORCED_OFF) ||
        (st.reserved & X9_USB_BUS_CYCLING) || st.configuration)) {
        lck_mtx_unlock(&usb_lock); return EBUSY;
    }
    if (replacement && description_revision == UINT32_MAX) {
        lck_mtx_unlock(&usb_lock); return EOVERFLOW;
    }
    bzero(&bus_command, sizeof(bus_command));
    bus_command.pending = 1; bus_command.operation = operation; bus_command.delay_ms = delay_ms;
    if (replacement) bus_command.description = *replacement;
    while (!bus_command.done) {
        if (mach_absolute_time() >= deadline) {
            /* A started command retains ownership after a waiter times out.
             * Another caller cannot overwrite it or induce a second cycle. */
            bus_command.abandoned = 1;
            lck_mtx_unlock(&usb_lock); return ETIMEDOUT;
        }
        (void)assert_wait_deadline(&bus_channel, THREAD_UNINT, deadline);
        lck_mtx_unlock(&usb_lock); thread_block(THREAD_CONTINUE_NULL); lck_mtx_lock(&usb_lock);
    }
    int result = bus_command.result; bus_command.pending = 0;
    lck_mtx_unlock(&usb_lock); return result;
}
int x9_usb_bus_command(unsigned operation, uint32_t delay_ms)
{
    if (operation > X9_USB_BUS_CYCLE || delay_ms > 1000 ||
        (operation != X9_USB_BUS_CYCLE && delay_ms)) return EINVAL;
    return worker_command(operation, delay_ms, NULL);
}
static int valid_string(const char *s)
{
    if (!s[0]) return 0;
    for (unsigned i = 0; i < X9_USB_STRING_CAPACITY; i++) {
        if (!s[i]) return 1;
        if ((uint8_t)s[i] < 0x20 || (uint8_t)s[i] > 0x7e) return 0;
    }
    return 0;
}
int x9_usb_set_description(const struct x9_usb_description *d)
{
    if (!d || d->version != X9_USB_DESCRIPTION_ABI || !d->vendor_id ||
        d->vendor_id > 0xffff || !d->product_id || d->product_id > 0xffff ||
        d->device_id > 0xffff || d->device_class != 0xef || d->device_subclass != 2 ||
        d->device_protocol != 1 || d->bcd_usb != 0x200 ||
        !valid_string(d->manufacturer) || !valid_string(d->product) || !valid_string(d->serial)) return EINVAL;
    return worker_command(COMMAND_DESCRIPTION, 0, d);
}
void x9_usb_get_description(struct x9_usb_description_status *out)
{
    lck_mtx_lock(&usb_lock);
    out->version = X9_USB_DESCRIPTION_ABI; out->revision = description_revision;
    out->mode = st.mode; out->forced_off = !!(st.reserved & X9_USB_BUS_FORCED_OFF);
    out->description = description;
    lck_mtx_unlock(&usb_lock);
}
static int set_configuration(unsigned value)
{
    int error = reset_data(); if (error || !value) return error;
    /* MTU3 with a USB3 IP has separate EP0/TX/RX FIFOs even at USB2 speed.
     * Validate capacity at init, then assign 512 bytes per endpoint. */
    configure_endpoint(&eps[2], 512);
    if (st.raw_enabled) { configure_endpoint(&eps[3], 512); configure_endpoint(&eps[4], 1024); }
    lck_mtx_lock(&usb_lock); st.configuration = 1; st.state = X9_USB_CONFIGURED; lck_mtx_unlock(&usb_lock);
    printf("X9 USB: config=1 speed=%u QMU36 gen2=%u raw=%u\n", st.speed, gen2, st.raw_enabled);
    return 0;
}
static void update_link(void)
{
    unsigned up = st.configuration && st.data_alt && packet_filter && !suspended && !failed;
    unsigned before = st.link; post_link(up); if (before != up) notification = 1;
}
static uint32_t epcsr(void) { return R(EP0) & ~EP0_W1C; }
static void stall(unsigned ack) { W(EP0, epcsr() | STALL | ack); phase = CTRL_IDLE; }
static void transmit(void)
{
    unsigned n = length - sent; if (n > 64) n = 64;
    x9_mtu3_fifo_write(control + sent, n); sent += n;
    phase = (n < 64 || (sent == length && length == wanted)) ? CTRL_IN_END : CTRL_IN;
    W(EP0, epcsr() | TX);
}
static uint64_t getpos(void *x) { (void)x; return sweet_log_position(); }
static uint32_t copylog(void *x, uint64_t *p, char *dst, uint32_t n) { (void)x; return sweet_log_copy(p, dst, n); }
static const struct x9log_reader reader = { NULL, getpos, copylog };
static unsigned string_descriptor(unsigned index)
{
    const char *s = index == 1 ? description.manufacturer : index == 2 ? description.product :
                    index == 3 ? description.serial : index == 4 ? "025839555301" : NULL;
    if (!index) { control[0] = 4; control[1] = 3; control[2] = 9; control[3] = 4; return 4; }
    if (!s) return 0;
    unsigned n = 2; while (*s && n < 126) { control[n++] = (uint8_t)*s++; control[n++] = 0; }
    control[0] = (uint8_t)n; control[1] = 3; return n;
}
static unsigned configuration_descriptor(unsigned other)
{
    static const uint8_t cfg[] = {
        9,2,88,0,2,1,0,0x80,250,
        8,11,0,2,2,6,0,0, /* IAD isolates ECM from the optional private function */
        9,4,0,0,1,2,6,0,0,
        5,0x24,0,0x10,1, 5,0x24,6,0,1,
        13,0x24,0xf,4,0,0,0,0,0xea,5,0,0,0,
        7,5,0x82,3,16,0,8,
        9,4,1,0,0,0xa,0,0,0,
        9,4,1,1,2,0xa,0,0,0,
        7,5,0x81,2,0,2,0, 7,5,1,2,0,2,0
    };
    static const uint8_t raw[] = { 9,4,2,0,2,0xff,0x58,1,0, 7,5,0x83,2,0,2,0, 7,5,3,2,0,2,0 };
    unsigned n = sizeof(cfg); memcpy(control, cfg, n);
    if (st.raw_enabled) { memcpy(control + n, raw, sizeof(raw)); n += sizeof(raw); control[4] = 3; }
    control[2] = (uint8_t)n; control[3] = (uint8_t)(n >> 8); if (other) control[1] = 7;
    unsigned mps = (st.speed == 3) ^ !!other ? 512 : 64;
    for (unsigned o = 9; o < n; o += control[o]) {
        if (control[o + 1] != 5) continue;
        if (control[o + 3] == 2) { control[o + 4] = (uint8_t)mps; control[o + 5] = (uint8_t)(mps >> 8); }
        else control[o + 6] = mps == 512 ? 8 : 16;
    }
    return n;
}
static struct endpoint *find_endpoint(unsigned address)
{
    for (unsigned i = 0; i < 5; i++) if ((eps[i].number | (eps[i].in ? 128u : 0)) == address && eps[i].enabled) return &eps[i];
    return NULL;
}
static int halt_endpoint(struct endpoint *e, unsigned halt)
{
    int error = stop_endpoint(e); if (error) return error;
    uint32_t reset = BIT(e->number + (e->in ? 16 : 0));
    if (!halt) { W(0x804, reset); W(0x804, 0); }
    uint32_t value = R(csr0(e)) & ~(BIT(22) | BIT(21) | (e->in ? 0 : BIT(16)));
    if (halt) value |= BIT(21); else value |= BIT(22);
    W(csr0(e), value); e->halted = halt;
    return 0;
}
static void setup(void)
{
    uint8_t s[8]; x9_mtu3_fifo_read(s, 8);
    unsigned type=s[0], req=s[1], value=s[2]|(unsigned)s[3]<<8,
             index=s[4]|(unsigned)s[5]<<8, n=s[6]|(unsigned)s[7]<<8, count=0;
    phase = CTRL_IDLE; sent = 0; wanted = n;
    int error = 0;
    if (type == 0x80 && req == 6) {
        static const uint8_t dev[] = {18,1,0,2,0xef,2,1,64,9,0x12,1,0,0,2,1,2,3,1};
        static const uint8_t qual[] = {10,6,0,2,0xef,2,1,64,1,0};
        if (value == 0x100 && !index) {
            memcpy(control,dev,sizeof(dev)); count=sizeof(dev);
            control[8]=(uint8_t)description.vendor_id; control[9]=(uint8_t)(description.vendor_id>>8);
            control[10]=(uint8_t)description.product_id; control[11]=(uint8_t)(description.product_id>>8);
            control[12]=(uint8_t)description.device_id; control[13]=(uint8_t)(description.device_id>>8);
        }
        else if ((value == 0x200 || value == 0x700) && !index) count=configuration_descriptor(value == 0x700);
        else if ((value >> 8) == 3 && (!(value & 255) ? !index : index == 0x409)) count=string_descriptor(value & 255);
        else if (value == 0x600 && !index) { memcpy(control,qual,sizeof(qual)); count=sizeof(qual); }
        if (!count || !n) { stall(n ? 0 : SETUP); return; }
    } else if (type == 0 && req == 5 && !n && value < 128 && !index && !st.configuration)
        W(0x800,(R(0x800)&~(0x7fu<<24))|value<<24);
    else if (type == 0 && req == 9 && !n && value <= 1 && !index) error=set_configuration(value);
    else if (type == 0x80 && req == 8 && n == 1 && !value && !index) { control[0]=(uint8_t)st.configuration; count=1; }
    else if (type == 0x80 && req == 0 && n == 2 && !value && !index) { control[0]=control[1]=0; count=2; }
    else if (type == 0x81 && req == 0 && n == 2 && !value && st.configuration && index < (st.raw_enabled ? 3u : 2u)) { control[0]=control[1]=0; count=2; }
    else if (type == 0x82 && req == 0 && n == 2 && !value) {
        struct endpoint *e=find_endpoint(index);
        if (!e && index != 0 && index != 0x80) { stall(0); return; }
        control[0]=e ? (uint8_t)e->halted : 0; control[1]=0; count=2;
    } else if (type == 2 && (req == 1 || req == 3) && !n && !value) {
        struct endpoint *e=find_endpoint(index); if (!e) { stall(SETUP); return; }
        error=halt_endpoint(e,req==3);
    } else if (type == 0x81 && req == 10 && n == 1 && !value && st.configuration && index < (st.raw_enabled ? 3u : 2u)) {
        control[0]=index==1 ? (uint8_t)st.data_alt : 0; count=1;
    } else if (type == 1 && req == 11 && !n && st.configuration && index < (st.raw_enabled ? 3u : 2u) && value <= (index == 1 ? 1u : 0u)) {
        if (index == 1) {
            error=disable_endpoint(&eps[0]); if (!error) error=disable_endpoint(&eps[1]);
            if (!error && value) { configure_endpoint(&eps[0],0); configure_endpoint(&eps[1],0); }
            lck_mtx_lock(&usb_lock); st.data_alt=value; net_tx.head=net_tx.count=0; lck_mtx_unlock(&usb_lock);
            update_link();
        }
    } else if (type == 0x21 && req == 0x43 && !index && !n && st.configuration && !(value & ~31u)) {
        lck_mtx_lock(&usb_lock); packet_filter=value; lck_mtx_unlock(&usb_lock); update_link();
    } else if (type==0xc0 && st.configuration && ((req==X9LOG_BATCH_REQ && n==X9LOG_BATCH_FRAME) || (req==X9LOG_REQ && n==64))) {
        int k=x9log_read_capacity(&reader,(uint32_t)value|((uint32_t)index<<16),control,n,2);
        if (k<0) { stall(0); return; } count=(unsigned)k;
    } else { stall(n ? 0 : SETUP); return; }
    if (error) { fault(error,0); stall(n ? 0 : SETUP); return; }
    if (!n) { W(EP0,epcsr()|SETUP|DATAEND); return; }
    length=count<n ? count:n; W(EP0,epcsr()|SETUP|BIT(20)); phase=CTRL_IN; transmit();
}
static void poll(void)
{
    uint32_t common=R(0x241c); if (common) W(0x241c,common);
    if (common & (BIT(2)|BIT(5))) {
        phase=CTRL_IDLE;
        int error=reset_data(); if (error) { fault(error,0); return; }
        lck_mtx_lock(&usb_lock); st.resets++; st.speed=R(0x800)&7; lck_mtx_unlock(&usb_lock);
        maxpacket=st.speed==3 ? 512:64;
        W(0x800,R(0x800)&~(0x7fu<<24)); W(EP0,64);
    }
    if (common & BIT(0)) { suspended=1; update_link(); }
    if (common & (BIT(1)|BIT(4))) { suspended=0; update_link(); }
    /* Speed can settle after RESET; read the controller's result, not cable labels. */
    unsigned speed=R(0x800)&7;
    if (speed==1 || speed==3) { lck_mtx_lock(&usb_lock); st.speed=speed; lck_mtx_unlock(&usb_lock); maxpacket=speed==3 ? 512:64; }
    uint32_t isr=R(0x80);
    if (isr & 0x10001u) {
        W(0x80,isr & 0x10001u); uint32_t csr=R(EP0);
        if (isr & BIT(16)) { phase=CTRL_IDLE; sent=0; }
        if (csr & SENTSTALL) { W(EP0,(csr & ~(EP0_W1C|STALL))|SENTSTALL); phase=CTRL_IDLE; csr=R(EP0); }
        if (csr & SETUP) { if (R(0x108)!=8) stall(SETUP); else setup(); }
        else if ((phase==CTRL_IN || phase==CTRL_IN_END) && !(csr & TX)) {
            if (phase==CTRL_IN_END) { W(EP0,epcsr()|DATAEND); phase=CTRL_IDLE; } else transmit();
        }
    }
    if (!failed) data_poll();
}
static errno_t net_output(ifnet_t ifp, mbuf_t m)
{
    size_t n=mbuf_pkthdr_len(m); int error=0;
    lck_mtx_lock(&usb_lock);
    if (n<14 || n>NET_MAX) error=EMSGSIZE;
    else if (!st.link || failed) error=ENETDOWN;
    else if (net_tx.count==QUEUE) error=ENOBUFS;
    else {
        struct chunk *v=&net_tx.v[(net_tx.head+net_tx.count)%QUEUE];
        error=mbuf_copydata(m,0,n,v->bytes); if (!error) { v->n=(uint32_t)n; net_tx.count++; }
    }
    if (error) st.dropped++;
    lck_mtx_unlock(&usb_lock); mbuf_freem(m);
    if (error) ifnet_stat_increment_out(ifp,0,0,1);
    return error;
}
static errno_t net_ioctl(ifnet_t ifp, unsigned long cmd, void *data)
{
    switch (cmd) {
    case SIOCSIFADDR: case SIOCAIFADDR: return ifnet_set_flags(ifp,IFF_UP,IFF_UP);
    case SIOCSIFFLAGS: case SIOCADDMULTI: case SIOCDELMULTI: return 0;
    case SIOCSIFMTU: return ((struct ifreq *)data)->ifr_mtu==ETHERMTU ? 0:EINVAL;
    case SIOCGIFMEDIA: {
        struct ifmediareq *m=data; m->ifm_current=m->ifm_active=IFM_ETHER|IFM_AUTO;
        m->ifm_status=IFM_AVALID | (st.link ? IFM_ACTIVE:0); m->ifm_count=0; return 0;
    }
    default: return EOPNOTSUPP;
    }
}
static void net_detach(ifnet_t ifp) { (void)ifp; }
static int create_network(void)
{
    struct ifnet_init_eparams p; bzero(&p,sizeof(p));
    p.ver=IFNET_INIT_CURRENT_VERSION; p.len=sizeof(p); p.flags=IFNET_INIT_LEGACY|IFNET_INIT_NX_NOAUTO;
    p.uniqueid="x9-mtu3-ecm"; p.uniqueid_len=sizeof("x9-mtu3-ecm")-1;
    p.name="ue"; p.unit=0; p.family=IFNET_FAMILY_ETHERNET; p.type=IFT_ETHER;
    p.output=net_output; p.demux=ether_demux; p.add_proto=ether_add_proto; p.del_proto=ether_del_proto;
    p.check_multi=ether_check_multi; p.framer_extended=ether_frameout_extended; p.ioctl=net_ioctl; p.detach=net_detach;
    p.broadcast_addr=etherbroadcastaddr; p.broadcast_len=6;
    int error=ifnet_allocate_extended(&p,&netif); if (error) return error;
    ifnet_set_addrlen(netif,6); ifnet_set_hdrlen(netif,14); ifnet_set_mtu(netif,ETHERMTU);
    ifnet_set_baudrate(netif,480000000); ifnet_set_flags(netif,IFF_BROADCAST|IFF_SIMPLEX|IFF_MULTICAST,0xffff);
    error=ifnet_attach(netif,NULL); if (error) { ifnet_release(netif); netif=NULL; return error; }
    ifnet_set_lladdr(netif,phone_mac,6);
    error=proto_plumb(PF_INET,netif); if (error) return error;
    if (sweet_x9_arg("x9_usb_ip",1)==1) {
        socket_t so=NULL; error=sock_socket(PF_INET,SOCK_DGRAM,0,NULL,NULL,&so);
        if (!error) {
            struct ifaliasreq a; bzero(&a,sizeof(a)); snprintf(a.ifra_name,sizeof(a.ifra_name),"ue0");
            struct sockaddr_in *v=(struct sockaddr_in *)&a.ifra_addr;
            v->sin_len=sizeof(*v); v->sin_family=AF_INET; v->sin_addr.s_addr=htonl(0xc0a80702);
            v=(struct sockaddr_in *)&a.ifra_mask; v->sin_len=sizeof(*v); v->sin_family=AF_INET; v->sin_addr.s_addr=htonl(0xffffff00);
            v=(struct sockaddr_in *)&a.ifra_broadaddr; v->sin_len=sizeof(*v); v->sin_family=AF_INET; v->sin_addr.s_addr=htonl(0xc0a807ff);
            error=sock_ioctl(so,SIOCAIFADDR,&a); sock_close(so);
        }
    }
    if (!error) ifnet_set_flags(netif,IFF_UP,IFF_UP);
    printf("X9 USB: native BSD ue0 attached, static IPv4 err=%d; no Apple mux/lockdown emulation\n",error);
    return error;
}
static void worker(void *arg, wait_result_t why)
{
    (void)arg; (void)why;
    int error=x9_mtu3_init(0);
    if (error) { lck_mtx_lock(&usb_lock); st.state=X9_USB_FAULT; st.error=error; lck_mtx_unlock(&usb_lock);
        printf("X9 USB: inherited PHY/clock bring-up failed %d\n",error); thread_terminate_self(); return; }
    unsigned version=x9_mtu3_ip_read(0xa4)>>16; gen2=version>=0x1003;
    lck_mtx_lock(&usb_lock); st.trunk_version=version; lck_mtx_unlock(&usb_lock);
    uint32_t info=R(0xc10);
    unsigned txneed=st.raw_enabled ? 3u:2u, rxneed=st.raw_enabled ? 3u:1u;
    if ((info&31)<txneed || ((info>>8)&31)<rxneed || R(0xc08)<txneed*512 ||
        R(0xc0c)<(st.raw_enabled ? 1024u:512u) || !(x9_mtu3_ip_read(0x28)&0xff)) error=ENODEV;
    /* DMA_ADDR_36BIT is a hardware capability, NOT a writable enable. OPPO's
     * mtu3_set_dma_mask reads it. XNU has no usable <4-GiB pages here; reject
     * a 32-bit controller rather than truncate addresses or force this bit. */
    if (!(R(0xc84)&BIT(31))) error=ENOTSUP;
    printf("X9 USB: CAP_EPINFO=%#x TXFIFO=%u RXFIFO=%u MISC=%#x\n",info,R(0xc08),R(0xc0c),R(0xc84));
    /* The existing HS PHY path is verified; SS PHY/retimer/Type-C ownership
     * is intentionally not borrowed from Qualcomm or guessed. */
    if (!error) for (unsigned i=0;i<5;i++) {
        void *va=NULL; uint64_t pa=0; error=x9_usb_dma_alloc(&va,&pa);
        if (error) break; eps[i].va=va; eps[i].pa=pa;
    }
    if (!error) error=create_network();
    if (error) { fault(error,info); thread_terminate_self(); return; }
    /* No IOMMU attachment is declared for ssusb in the MT6993 DTS. This is
     * not proof of live EMI/MPU accessibility; only an actual DMA can prove it. */
    W(0x428,gen2 ? 0xffffffffu:0); W(0x400,0);
    W(0x780,0xffffffff); W(0x790,0xffffffff); W(0x7a0,0xffffffff);
    W(0x700,0xffffffff); W(0x710,0xffffffff);
    W(0x2418,BIT(0)|BIT(1)|BIT(2)|BIT(4)|BIT(5)); maxpacket=64;
    lck_mtx_lock(&usb_lock); st.state=X9_USB_READY; lck_mtx_unlock(&usb_lock);
    W(0x2404,R(0x2404)|BIT(6));
    printf("X9 USB: MTU3 USB2 ECM%s ready, GIC INTID533 polled, QMU36 version=%#x; EP0 log retained\n",st.raw_enabled?"+private bulk":"",version);
    for (;;) {
        bus_poll(); /* completes even if the hardware was isolated */
        lck_mtx_lock(&usb_lock); unsigned off = st.reserved & X9_USB_BUS_FORCED_OFF; lck_mtx_unlock(&usb_lock);
        if (!failed && !off) poll();
        uint64_t deadline; clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);
        (void)assert_wait_deadline(&channel,THREAD_UNINT,deadline); thread_block(THREAD_CONTINUE_NULL);
    }
}
void x9_usb_start(void)
{
    lck_mtx_lock(&usb_lock);
    if (started) { lck_mtx_unlock(&usb_lock); return; }
    started=1; st.mode=(uint32_t)sweet_x9_arg("x9_usb",0); st.raw_enabled=st.mode==2;
    st.state=X9_USB_INITIALIZING; st.generation=1; lck_mtx_unlock(&usb_lock);
    thread_t t; int error=kernel_thread_start(worker,NULL,&t);
    if (!error) thread_deallocate(t);
    else { lck_mtx_lock(&usb_lock); st.state=X9_USB_FAULT; st.error=ENOMEM; lck_mtx_unlock(&usb_lock); }
}
void x9_usb_get_status(struct x9_usb_status *out)
{
    lck_mtx_lock(&usb_lock); *out=st; out->tx_pending=raw_tx.count; out->rx_pending=raw_rx.count; lck_mtx_unlock(&usb_lock);
}
int x9_usb_claim(uint64_t *token)
{
    lck_mtx_lock(&usb_lock); int error=0;
    if (!st.raw_enabled) error=ENOTSUP;
    else if (lease) error=EBUSY;
    else { lease=++next_lease; if (!lease) lease=++next_lease; *token=lease; }
    lck_mtx_unlock(&usb_lock); return error;
}
void x9_usb_release(uint64_t token)
{
    lck_mtx_lock(&usb_lock);
    if (lease==token && token) { lease=0; raw_tx.head=raw_tx.count=raw_rx.head=raw_rx.count=0; }
    lck_mtx_unlock(&usb_lock);
}
int x9_usb_send(uint64_t token, uint32_t generation, const void *bytes, uint32_t n)
{
    if (!n || n>X9_USB_CHUNK) return EINVAL;
    lck_mtx_lock(&usb_lock); int error=0;
    if (!token || token!=lease) error=EPERM;
    else if (generation!=st.generation) error=ESTALE;
    else if (st.state!=X9_USB_CONFIGURED || failed) error=ENXIO;
    else if (raw_tx.count==QUEUE) error=EAGAIN;
    else { struct chunk *v=&raw_tx.v[(raw_tx.head+raw_tx.count)%QUEUE]; v->n=n; memcpy(v->bytes,bytes,n); raw_tx.count++; }
    lck_mtx_unlock(&usb_lock); return error;
}
int x9_usb_receive(uint64_t token, uint32_t generation, void *bytes, uint32_t *n)
{
    lck_mtx_lock(&usb_lock); int error=0;
    if (!token || token!=lease) error=EPERM;
    else if (generation!=st.generation) error=ESTALE;
    else if (st.state!=X9_USB_CONFIGURED || failed) error=ENXIO;
    else if (!raw_rx.count) error=EAGAIN;
    else {
        struct chunk *v=&raw_rx.v[raw_rx.head];
        if (*n<v->n) error=EMSGSIZE;
        else { *n=v->n; memcpy(bytes,v->bytes,v->n); raw_rx.head=(raw_rx.head+1)%QUEUE; raw_rx.count--; }
    }
    if (error) *n=0;
    lck_mtx_unlock(&usb_lock); return error;
}
