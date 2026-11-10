/*
 * MT6993 / MT6653 Bluetooth bootstrap, OPPO linux_v2 + MT6993 DTS reference.
 * UART3 16030000 -> UARTHUB AP/CMM -> GPIO241/242, BT_RST GPIO249.
 * No Wi-Fi PCIe DMA, shared PMIC toggles, firmware asserts, or reset loops.
 * x9_bt=1: passive status; x9_bt_probe=1: adopt an already clocked hub and
 * issue the two ROM queries at 115200. A gated hub is a prerequisite failure,
 * NOT permission to read its registers or invent a successful controller.
 * x9_bt_raw=1 additionally permits admin-only H4 traffic after ROM handshake.
 * Patch/connfem/calibration/low-power ownership and native AppleBT ABI follow
 * in later stages. firmware_ready deliberately remains false in this stage.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <arm64/sweet_early.h>
#include "x9_bluetooth.h"
#include "../../../generated/x9_bt_firmware.inc"

extern int sweet_x9_arg(const char *, int);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
static LCK_GRP_DECLARE(bt_group, "x9_bluetooth");
static LCK_MTX_DECLARE(bt_lock, &bt_group);
static struct x9_bt_status bs = { .version = X9_BT_ABI_VERSION };
static volatile uint32_t *gpio, *peri, *top, *uart, *cmm, *hub;
static int started, raw_enabled;
struct bt_frame { uint32_t length; uint8_t data[X9_BT_FRAME_MAX]; };
#define BT_QUEUE 8u
static struct bt_frame rxq[BT_QUEUE], txq[BT_QUEUE];
static unsigned rx_head, rx_count, tx_head, tx_count;
static struct bt_frame assembling;
static uint32_t discard;
static uint64_t last_byte_us;
static uint64_t now_us(void)
{
    uint64_t t, ns; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000;
}
static uint32_t rr(volatile uint32_t *base, unsigned off) { return base[off / 4]; }
static void wr(volatile uint32_t *base, unsigned off, uint32_t value)
{ base[off / 4] = value; __asm__ volatile("dsb sy" ::: "memory"); }
static void bits(volatile uint32_t *base, unsigned off, uint32_t mask, uint32_t value)
{ wr(base, off, (rr(base, off) & ~mask) | (value & mask)); }
static unsigned pin(unsigned n)
{ return (rr(gpio, 0x200 + (n / 32) * 0x10) >> (n % 32)) & 1; }
static void state(enum x9_bt_state s, int error)
{ lck_mtx_lock(&bt_lock); bs.state = s; bs.error = error; lck_mtx_unlock(&bt_lock); }

/* Return the exact H4 frame length, 0 for a partial header, -1 for bad type.
 * Oversize packets are consumed to their declared boundary, not resynchronized
 * on arbitrary payload bytes. ISO length excludes its two RFU/status bits. */
static int frame_length(const uint8_t *p, uint32_t n)
{
    if (!n) return 0;
    switch (p[0]) {
    case 1: return n < 4 ? 0 : 4 + p[3];
    case 2: return n < 5 ? 0 : 5 + p[3] + ((unsigned)p[4] << 8);
    case 3: return n < 4 ? 0 : 4 + p[3];
    case 4: return n < 3 ? 0 : 3 + p[2];
    case 5: return n < 5 ? 0 : 5 + p[3] + ((unsigned)(p[4] & 0x3f) << 8);
    default: return -1;
    }
}

/* Worker owns all MMIO. The mutex protects only bounded memory copies. */
static void rx_byte(uint8_t ch)
{
    lck_mtx_lock(&bt_lock);
    bs.rx_bytes++;
    if (discard) { discard--; lck_mtx_unlock(&bt_lock); return; }
    assembling.data[assembling.length++] = ch;
    int total = frame_length(assembling.data, assembling.length);
    if (total < 0) { bs.bad_frames++; assembling.length = 0; }
    else if (total > (int)X9_BT_FRAME_MAX) {
        bs.bad_frames++; discard = (uint32_t)total - assembling.length;
        assembling.length = 0;
    } else if (total && assembling.length == (uint32_t)total) {
        bs.rx_frames++;
        if (rx_count < BT_QUEUE) {
            rxq[(rx_head + rx_count) % BT_QUEUE] = assembling;
            rx_count++;
        } else { bs.rx_dropped++; }
        assembling.length = 0;
    }
    lck_mtx_unlock(&bt_lock);
}

static int poll_rx(void)
{
    for (unsigned n = 0; n < 512; n++) {
        uint32_t lsr = rr(uart, 0x14);
        if (lsr & 0x1e) {
            lck_mtx_lock(&bt_lock); bs.uart_errors++; lck_mtx_unlock(&bt_lock);
            return EIO;
        }
        if (!(lsr & 1)) break;
        rx_byte((uint8_t)rr(uart, 0));
        last_byte_us = now_us();
    }
    if (now_us() - last_byte_us >= 500000 && (assembling.length || discard)) {
        lck_mtx_lock(&bt_lock);
        bs.bad_frames++; bs.timeouts++; assembling.length = discard = 0;
        lck_mtx_unlock(&bt_lock);
    }
    return 0;
}

static int transmit(const uint8_t *p, uint32_t len)
{
    for (unsigned n = 0; n < len; n++) {
        unsigned wait;
        for (wait = 0; wait < 20000 && !(rr(uart, 0x14) & 0x20); wait++) {
            int error = poll_rx(); if (error) return error;
            delay(5);
        }
        if (wait == 20000) {
            lck_mtx_lock(&bt_lock); bs.timeouts++; lck_mtx_unlock(&bt_lock);
            return ETIMEDOUT;
        }
        wr(uart, 0, p[n]);
        lck_mtx_lock(&bt_lock); bs.tx_bytes++; lck_mtx_unlock(&bt_lock);
    }
    lck_mtx_lock(&bt_lock); bs.tx_frames++; lck_mtx_unlock(&bt_lock);
    return 0;
}

void x9_bluetooth_status(struct x9_bt_status *out)
{ lck_mtx_lock(&bt_lock); *out = bs; lck_mtx_unlock(&bt_lock); }

int x9_bluetooth_section(uint32_t n, struct x9_bt_section *out)
{
    if (n >= X9_BT_FW_SECTIONS) return ENOENT;
    *out = x9_bt_fw_sections[n]; return 0;
}

int x9_bluetooth_send(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    if (!p || !len || len > X9_BT_FRAME_MAX || p[0] == 4 || frame_length(p, len) != (int)len)
        return EINVAL;
    lck_mtx_lock(&bt_lock);
    int error = !raw_enabled ? EPERM : bs.state != X9_BT_ROM_READY ? ENXIO :
        tx_count == BT_QUEUE ? EAGAIN : 0;
    if (!error) {
        struct bt_frame *f = &txq[(tx_head + tx_count) % BT_QUEUE];
        memcpy(f->data, p, len); f->length = len; tx_count++;
    }
    lck_mtx_unlock(&bt_lock); return error;
}

static int pop_rx(void *data, uint32_t *len, bool bootstrap)
{
    lck_mtx_lock(&bt_lock);
    int error = !bootstrap && bs.state != X9_BT_ROM_READY ? ENXIO : !rx_count ? EAGAIN : 0;
    if (!error) {
        struct bt_frame *f = &rxq[rx_head];
        if (*len < f->length) { *len = f->length; error = EMSGSIZE; }
        else { memcpy(data, f->data, f->length); *len = f->length;
               rx_head = (rx_head + 1) % BT_QUEUE; rx_count--; }
    }
    lck_mtx_unlock(&bt_lock); return error;
}
int x9_bluetooth_receive(void *data, uint32_t *len)
{ return pop_rx(data, len, false); }

/* mt6985 UART's divisor/feature registers differ from legacy 16550 DLAB.
 * Set both AP and external CMM in bypass mode; never alter MD/ADSP UARTs. */
static const unsigned save_off[] = { 0x04, 0x0c, 0x10, 0x24, 0x28, 0x2c,
    0x34, 0x44, 0x4c, 0x54, 0x58, 0x90, 0x94, 0x98, 0x9c };
static uint32_t ap_save[sizeof(save_off)/sizeof(save_off[0])];
static uint32_t cmm_save[sizeof(save_off)/sizeof(save_off[0])];
static uint32_t ap_fifo_save, cmm_fifo_save;
static uint32_t mode_tx_save, mode_rst_save, rst_dir_save, rst_do_save, hub_save;
static void uart_115200(volatile uint32_t *u, unsigned clock)
{
    static const uint8_t fl[] = {0,1,5,0x15,0x55,0x57,0x57,0x77,0x7f,0xff,0xff};
    static const uint8_t fm[] = {0,0,0,0,0,0,1,1,1,1,3};
    unsigned div = (clock + 256 * 115200 - 1) / (256 * 115200);
    unsigned sample = clock / (115200 * div) - 1;
    unsigned fraction = ((clock * 100ULL / 115200 / div) % 100 + 5) / 10;
    wr(u, 4, 0); wr(u, 0x4c, 0); /* no IRQ/DMA; polling owns the FIFO */
    wr(u, 0x0c, 3); wr(u, 0x34, 0); wr(u, 0x24, 3);
    wr(u, 0x28, sample); wr(u, 0x2c, (sample >> 1) - 1);
    wr(u, 0x54, fl[fraction]); wr(u, 0x58, fm[fraction]);
    wr(u, 0x9c, 1); wr(u, 0x90, div); wr(u, 0x94, 0);
    wr(u, 0x98, 0x10); wr(u, 0x9c, 0);
    wr(u, 0x44, 0); wr(u, 0x10, 3); wr(u, 8, 7);
}

static void restore(void)
{
    /* Restore divisor and enhancement registers through the feature window. */
    wr(uart, 4, 0); wr(cmm, 4, 0);
    for (unsigned i = 0; i < sizeof(save_off)/sizeof(save_off[0]); i++) {
        if (save_off[i] == 0x9c || save_off[i] == 4) continue;
        if (save_off[i] >= 0x90) { wr(uart, 0x9c, 1); wr(cmm, 0x9c, 1); }
        wr(uart, save_off[i], ap_save[i]); wr(cmm, save_off[i], cmm_save[i]);
    }
    wr(uart, 0x9c, ap_save[14]); wr(cmm, 0x9c, cmm_save[14]);
    wr(uart, 8, ap_fifo_save); wr(cmm, 8, cmm_fifo_save);
    wr(uart, 4, ap_save[0]); wr(cmm, 4, cmm_save[0]);
    bits(hub, 0x4c8, 4, hub_save);
    bits(gpio, 0x170, 1u << 25, rst_do_save);
    bits(gpio, 0x70, 1u << 25, rst_dir_save);
    bits(gpio, 0x6e0, 0x1fu << 8, mode_rst_save);
    bits(gpio, 0x6c0, (0x1fu << 8) | (0x1fu << 16), mode_tx_save);
    /* No PMIC, POR, WF_RST, clock mux, host request or shared reset writes. */
}

static int query(uint8_t id, struct bt_frame *reply)
{
    const uint8_t cmd[] = {1, 0x6f, 0xfc, 5, 1, 4, 1, 0, id};
    int error = transmit(cmd, sizeof(cmd)); if (error) return error;
    for (unsigned ms = 0; ms < 500; ms++) {
        error = poll_rx(); if (error) return error;
        reply->length = sizeof(reply->data);
        if (!pop_rx(reply->data, &reply->length, true) &&
            reply->length == 13 && reply->data[0] == 4 && reply->data[1] == 0xe4 &&
            reply->data[2] == 10 && reply->data[3] == 2 && reply->data[4] == 4 &&
            reply->data[5] == 6 && reply->data[6] == 0 && reply->data[7] == 0 && reply->data[8] == id)
            return 0;
        delay_for_interval(1, 1000 * NSEC_PER_USEC);
    }
    lck_mtx_lock(&bt_lock); bs.timeouts++; lck_mtx_unlock(&bt_lock);
    return ETIMEDOUT;
}

static void worker(void *unused, wait_result_t result)
{
    (void)unused; (void)result;
    state(X9_BT_WAIT_POWER, 0);
    /* Shared Wi-Fi power worker starts at ~20 s. Only wait/read AO GPIO. */
    unsigned ms;
    for (ms = 0; ms < 45000 && (!pin(251) || !pin(65)); ms++)
        delay_for_interval(1, 1000 * NSEC_PER_USEC);
    uint32_t cg0 = rr(peri, 0x10), cg2 = rr(peri, 0x18), rst = rr(peri, 0);
    uint32_t mux = rr(top, 0x100);
    printf("X9 BT: prerequisites power=%u faultb=%u cg0=%08x cg2=%08x rst=%08x mux=%08x\n",
           pin(251), pin(65), cg0, cg2, rst, mux);
    if (ms == 45000 || (cg0 & 8) || (cg2 & (7u << 18)) || (rst & (1u << 22)) ||
        (mux & ((1u << 23) | (1u << 31))) || !(rr(peri, 0x20) & 8)) {
        state(X9_BT_FAILED, ENXIO);
        printf("X9 BT: gated/unpowered UART3 or UARTHUB; no off-domain MMIO, no power/reset writes\n");
        return;
    }
    /* Derive live clock rates from muxes rather than silently assuming 208M. */
    const unsigned rates[] = {26000000, 52000000, 104000000, 208000000};
    unsigned ap_clock = rates[(mux >> 24) & 3];
    unsigned hub_sel = (mux >> 16) & 3;
    if (hub_sel > 2) { state(X9_BT_FAILED, EINVAL); return; }
    unsigned hub_clock = hub_sel == 0 ? 26000000 : hub_sel == 1 ? 104000000 : 208000000;
    uart = (volatile uint32_t *)ml_io_map(0x16030000, 0x1000);
    cmm = (volatile uint32_t *)ml_io_map(0x16060000, 0x1000);
    hub = (volatile uint32_t *)ml_io_map(0x160a0000, 0x1000);
    if (!uart || !cmm || !hub) { state(X9_BT_FAILED, ENOMEM); return; }
    /* A ROM bootstrap changes the hub's routing globally. Refuse live MD/ADSP
     * requests; no request clear, shared hub reset, or enabling their FIFOs. */
    if ((rr(hub, 0x440) | rr(hub, 0x480)) & 0x1f0f) {
        state(X9_BT_FAILED, EBUSY); printf("X9 BT: shared MD/ADSP hub busy; bootstrap refused\n"); return;
    }
    /* Require our AP/CMM endpoints to be idle as well. Do not adopt Linux
     * APDMA state or consume somebody else's queued FIFO data. */
    if (rr(uart, 4) || rr(cmm, 4) || rr(uart, 0x4c) || rr(cmm, 0x4c) ||
        !(rr(uart, 0x14) & 0x40) || !(rr(cmm, 0x14) & 0x40) ||
        ((rr(uart, 0x14) | rr(cmm, 0x14)) & 1)) {
        state(X9_BT_FAILED, EBUSY); printf("X9 BT: AP/CMM UART IRQ/DMA/FIFO not idle; refused\n"); return;
    }
    for (unsigned i = 0; i < sizeof(save_off)/sizeof(save_off[0]); i++) {
        ap_save[i] = rr(uart, save_off[i]); cmm_save[i] = rr(cmm, save_off[i]);
    }
    ap_fifo_save = rr(uart, 0x5c); cmm_fifo_save = rr(cmm, 0x5c);
    wr(uart, 0x9c, 1); wr(cmm, 0x9c, 1);
    for (unsigned i = 11; i < 14; i++) {
        ap_save[i] = rr(uart, save_off[i]); cmm_save[i] = rr(cmm, save_off[i]);
    }
    wr(uart, 0x9c, ap_save[14]); wr(cmm, 0x9c, cmm_save[14]);
    mode_tx_save = rr(gpio, 0x6c0); mode_rst_save = rr(gpio, 0x6e0);
    rst_dir_save = rr(gpio, 0x70); rst_do_save = rr(gpio, 0x170); hub_save = rr(hub, 0x4c8);
    bits(hub, 0x4c8, 4, 4); /* INTFHUB CON2 +0xc8 bypass, b2 */
    bits(gpio, 0x6c0, (0x1fu << 8) | (0x1fu << 16), (1u << 8) | (1u << 16));
    bits(gpio, 0x6e0, 0x1fu << 8, 0);
    bits(gpio, 0x170, 1u << 25, 1u << 25); bits(gpio, 0x70, 1u << 25, 1u << 25);
    delay_for_interval(20, 1000 * NSEC_PER_USEC);
    uart_115200(uart, ap_clock); uart_115200(cmm, hub_clock);
    state(X9_BT_ROM_QUERY, 0);
    struct bt_frame reply;
    int error = query(2, &reply);
    if (!error) error = query(9, &reply);
    uint32_t chip = error ? 0 : reply.data[9] | ((unsigned)reply.data[10] << 8);
    if (!error && chip != 0x6653) error = ENODEV;
    if (error) {
        restore(); state(X9_BT_FAILED, error);
        printf("X9 BT: ROM handshake failed errno=%d; UART/pins/bypass restored (no retries/assert)\n", error);
        return;
    }
    lck_mtx_lock(&bt_lock); bs.chip_id = chip; bs.baud = 115200; bs.state = X9_BT_ROM_READY;
    lck_mtx_unlock(&bt_lock);
    printf("X9 BT: ROM link verified chip=%04x baud=115200; firmware/calibration NOT ready; raw=%d\n", chip, raw_enabled);
    for (;;) {
        struct bt_frame tx;
        bool pending = false;
        lck_mtx_lock(&bt_lock);
        if (tx_count) { tx = txq[tx_head]; tx_head = (tx_head + 1) % BT_QUEUE; tx_count--; pending = true; }
        lck_mtx_unlock(&bt_lock);
        error = pending ? transmit(tx.data, tx.length) : poll_rx();
        if (error) { restore(); state(X9_BT_FAILED, error); printf("X9 BT: transport failed errno=%d; restored\n", error); return; }
        delay_for_interval(1, 1000 * NSEC_PER_USEC);
    }
}

void x9_bluetooth_start(void)
{
    if (!sweet_early_is_x9() || sweet_x9_arg("x9_bt", 0) != 1 || started) return;
    started = 1; raw_enabled = sweet_x9_arg("x9_bt_raw", 0) == 1;
    lck_mtx_lock(&bt_lock);
    bs.state = X9_BT_PASSIVE; bs.firmware_bytes = X9_BT_FW_BYTES; bs.firmware_sections = X9_BT_FW_SECTIONS;
    lck_mtx_unlock(&bt_lock);
    gpio = (volatile uint32_t *)ml_io_map(0x1002d000, 0x1000);
    peri = (volatile uint32_t *)ml_io_map(0x16640000, 0x1000);
    top = (volatile uint32_t *)ml_io_map(0x1c210000, 0x1000);
    if (!gpio || !peri || !top) { state(X9_BT_FAILED, ENOMEM); return; }
    printf("X9 BT: MT6653 UART3/UARTHUB bootstrap, firmware metadata %u bytes/%u sections; probe=%d\n",
           X9_BT_FW_BYTES, X9_BT_FW_SECTIONS, sweet_x9_arg("x9_bt_probe", 0));
    printf("X9 BT: passive BT_RST=%u PMIC_EN=%u cg0=%08x cg2=%08x; firmware-ready=0\n",
           pin(249), pin(251), rr(peri, 0x10), rr(peri, 0x18));
    if (sweet_x9_arg("x9_bt_probe", 0) != 1) return;
    thread_t thread;
    if (kernel_thread_start(worker, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else state(X9_BT_FAILED, ENOMEM);
}
#endif
