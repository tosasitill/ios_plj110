/* MT6993 DPMAIF v3, independent of Qualcomm IPA/QMAP.
 * Register/descriptor contract: live DT, ccci_dpmaif_{drv_v3,drv_com,com}.
 * One bridge owner performs all MMIO, DMA publication and completion. Network
 * threads only copy into a bounded mailbox. No fixed/reserved DMA addresses.
 * x9_dpmaif=0: off; 1: clock/status probe + private allocation (NO HW writes);
 * 2: experimental SRAM/queue initialization, polled with both IRQs masked.
 * Mode 2 requires an explicitly selected active candidate. A fatal error retains
 * all DMA memory until reboot; no reset, modem assertion or recovery loop.
 */
namespace {
// Android live dump: BAT rings 16384 entries with ~2000 credited, PIT 0x8000.
// Each BID owns a stable buffer, allocated in bounded chunks on first use.
// Never change a live BID's address/modulus when MD requests more buffers.
constexpr unsigned X9D_DRB = 2048, X9D_BAT = 16384, X9D_PIT = 0x8000;
constexpr unsigned X9D_BUF_CHUNK = 256, X9D_POOL_CHUNKS = X9D_BAT / X9D_BUF_CHUNK;
constexpr unsigned X9D_CREDIT = 2000;
constexpr unsigned X9D_TX_SIZE = 3072, X9D_PKT_SIZE = 1664, X9D_FRG_SIZE = 1920;
constexpr unsigned X9D_MAIL = 128, X9D_BUDGET = 128, X9D_TX_BUDGET = 32;
constexpr uint64_t X9D_DMA_LIMIT = 1ULL << 40;
constexpr uint32_t X9D_BAT_HINT_PENDING = 1u << 15;
constexpr unsigned X9D_BAT_HINT_MAX = 0xffff;
static_assert(X9D_BAT % X9D_BUF_CHUNK == 0 && X9D_CREDIT < X9D_BAT, "BAT geometry");
constexpr uint32_t X9D_BUSY_BIT = 0x80000000u;
constexpr uint32_t X9D_UL_FLOW = (31u << 5) | (31u << 10) | (31u << 15);
constexpr uint32_t X9D_UL_ERROR = 31u << 20;
// v3 LRO IRQ STATUS positions, NOT mask-register positions (8/9 -> 13/14).
constexpr uint32_t X9D_DL_DONE = (1u << 13) | (1u << 14);
constexpr uint32_t X9D_DL_EMPTY = 3u << 4;
constexpr uint32_t X9D_DL_ERROR = (7u << 1) | (1u << 6) | (7u << 8);
// Legacy (non-LRO) PIT count status. There is no legacy PIT in native path2;
// drv3_isr0 filters this masked bit out before its length-error handler.
// Do NOT confuse it with the real LRO PIT count errors at status bits 8/9.
constexpr uint32_t X9D_DL_LEGACY_PIT = 1u << 3;
constexpr uint32_t X9D_DL_LEGACY_DONE = 1u; // masked QDONE of the non-LRO path
// drv3_intr_hw_init: L2 enable masks (mask-register positions). UL leaves
// EMPTY(5..9) and MD_NOTREADY(10..14) MASKED; DL LRO = MTU_ERR6 | LRO0/1 8/9.
constexpr uint32_t X9D_UL_L2_EN = (31u << 20) | (31u << 15) | 15u;
constexpr uint32_t X9D_DL_L2_EN = (1u << 6) | (1u << 8) | (1u << 9);
// mapping_rx_reg_from_mask_to_sts: mask bit8/9 -> status 13/14, 10..12 -> 8..10.
static inline uint32_t x9d_dl_mask_to_sts(uint32_t m) {
    return (m & 255u) | ((m >> 8 & 1u) << 13) | ((m >> 9 & 1u) << 14) | ((m >> 10 & 1u) << 8) |
        ((m >> 11 & 1u) << 9) | ((m >> 12 & 1u) << 10);
}
struct x9d_dma {
    IOBufferMemoryDescriptor *md;
    uint8_t *va;
    uint64_t pa;
    unsigned size;
};
// Per-bearer IP accounting; queue admission is NOT successful transmission.
// Only retirement of a payload DRB increments TX success. No packet contents.
struct x9d_stats {
    uint64_t tx_packets, rx_packets, tx_errors, rx_errors, tx_overflows, rx_overflows;
    uint64_t tx_bytes, rx_bytes, tx_dropped, rx_dropped;
};
// Admission epoch changes on unbind. Accounting lifetime does not: payloads
// already published must retire into their original call's ledger.
struct x9d_route { uint64_t epoch,lifetime; unsigned mtu4,mtu6; uint8_t channel, families; bool bound,retiring; x9d_stats stats; };
struct x9d_completion { uint64_t lifetime; uint16_t size; uint8_t mux; };
struct x9d_tx { uint64_t epoch,lifetime; uint16_t size; uint8_t mux; uint8_t ip[X9D_TX_SIZE]; };
// Channel tombstones are never reused. RX accounting stays independent of
// TX retirement: an already-associated RX partial can still finish/drop.
struct x9d_rx_account { uint64_t lifetime,packets,bytes,errors,overflows,dropped; };
struct x9d_rx {
    unsigned rd, bytes; uint8_t seq, channel; bool message, normal, drop;
    uint64_t seq_wait, epoch,lifetime; unsigned mux;
    uint8_t ip[X9D_TX_SIZE];
};
struct x9d_state {
    IOLock *lock;
    uintptr_t ao, pd, md_misc, infra, sram1;
    x9d_dma drb, txpool, pit[2], bat[2], pool[2][X9D_POOL_CHUNKS];
    x9d_dma drbq[3]; // TXQ1..3: started like dpmaif_txqs_start (DPMAIF_TXQ_NUM 4), never fed
    bool posted[2][X9D_BAT];
    bool used_channel[20]; // bound/retiring routes only; released when retirement finishes
    unsigned bat_wr[2], tx_wr, tx_rd, head, count;
    unsigned bat_hint, bat_target, bat_credit[2], bat_backing[2], bat_owned[2];
    unsigned bat_hint_reports, bat_alloc_failures[2], bat_alloc_reports;
    uint64_t bat_hint_events, bat_invalid_hints, bat_retry_ms[2];
    uint64_t rx_wire_bytes, bat_rate_ms, bat_rate_bytes;
    bool bat_high_rate;
    x9d_tx mail[X9D_MAIL]; x9d_route route[CBP_INTERFACES]; x9d_rx rx[2];
    x9d_completion completion[X9D_DRB / 2];
    x9d_rx_account rx_account[20];
    unsigned mode; bool prepared, ready, failed, published, hw_attempted, clocks_live;
    unsigned irq_reports;
    const char *phase;
    uint32_t last_ul, last_dl; bool legacy_pit_reported;
    uint64_t tx_packets, tx_done, rx_packets, rx_dropped, rejected, flow_events,next_lifetime;
    // TX progress watch (vendor drv3_dump_register subset on a stall).
    uint64_t tx_progress_ms, tx_seen_done, stale_completions, bind_ms; unsigned stall_dumps, rx_dumps, tx_diag, tx_diag6; bool stall_reported, at_seen;
    int (*receiver)(uint32_t, const uint8_t *, uint32_t, uint32_t, uint64_t);
    // Diagnostic ledgers only. Network producers use atomic increments;
    // report/RX probes run on the existing bridge owner. No packet contents.
    uint64_t tx_admit[9], pit_consumed[2], rx_probe_next[2], rx_disposition[6];
};
static x9d_state x9d;
enum { X9DP_TX_ACCEPT, X9DP_TX_ARGUMENT, X9DP_TX_IP, X9DP_TX_OFFLINE,
    X9DP_TX_ROUTE, X9DP_TX_EPOCH, X9DP_TX_FAMILY, X9DP_TX_MTU, X9DP_TX_FULL };
static void x9d_tx_count(unsigned why) {
    __atomic_fetch_add(&x9d.tx_admit[why], uint64_t(1), __ATOMIC_RELAXED);
}
static uint64_t x9d_ms() { uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000; }
static uint32_t x9d_rd(uintptr_t base, unsigned off) {
    uint32_t v = *reinterpret_cast<volatile uint32_t *>(base + off);
    __asm__ volatile("dmb oshld" ::: "memory"); return v;
}
static void x9d_wr(uintptr_t base, unsigned off, uint32_t v) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(base + off) = v;
    __asm__ volatile("dsb oshst" ::: "memory");
}
static void x9d_sync(const x9d_dma &m, unsigned off, unsigned size) {
    uintptr_t begin = reinterpret_cast<uintptr_t>(m.va + off) & ~uintptr_t(63);
    uintptr_t end = reinterpret_cast<uintptr_t>(m.va + off + size);
    // Pools/descriptors have disjoint cache lines, never invalidate unrelated data.
    for (uintptr_t p = begin; p < end; p += 64) __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static bool x9d_alloc(x9d_dma &m, unsigned bytes) {
    m.size = (bytes + 16383u) & ~16383u;
    // MT6993 ccci_dpmaif_com.c configures DMA_BIT_MASK(40), with ordinary
    // dma_map_single/page RX/TX buffers. DT alloc-ranges only constrains the
    // optional reserved-memory allocator, NOT all device DMA. XNU manages RAM
    // starting at 4GiB, so a low32 allocation can NEVER succeed (174.txt).
    // >page alignment also forces real host-contiguous pages for small rings.
    m.md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
        m.size, (X9D_DMA_LIMIT - 1) & ~32767ULL);
    if (!m.md) { printf("X9 DPMAIF: DMA alloc of %u bytes returned no descriptor\n", m.size); return false; }
    IOByteCount segment = 0;
    m.va = static_cast<uint8_t *>(m.md->getBytesNoCopy());
    m.pa = m.md->getPhysicalSegment(0, &segment, kIOMemoryMapperNone);
    if (!m.va || segment < m.size || m.pa < 0x40000000ULL ||
        m.pa >= X9D_DMA_LIMIT || m.size > X9D_DMA_LIMIT - m.pa || (m.pa & 32767)) {
        printf("X9 DPMAIF: DMA alloc of %u bytes out of range: pa 0x%llx segment 0x%llx va %d\n",
            m.size, (unsigned long long)m.pa, (unsigned long long)segment, m.va != nullptr);
        return false;
    }
    memset(m.va, 0, m.size); x9d_sync(m, 0, m.size); return true;
}
static void x9d_snapshot(const char *label) {
    if (!x9d.clocks_live) return; // never touch gated windows, including on failure
    uint32_t gate = x9d_rd(x9d.infra, 0xd20) & 12;
    if (gate) { printf("X9 DPMAIF: %s gated 0x%x, DPMAIF snapshot skipped\n", label, gate); return; }
    const uintptr_t misc = x9d.pd + 0x400, dls = x9d.pd + 0xc00, uls = x9d.pd + 0xd00;
    printf("X9 DPMAIF: %s phase %s gate 0x%x P2 0x%x CODA 0x%x arb 0x%x masks 0x%x/0x%x IRQ 0x%x/0x%x DMAerr 0x%x bus 0x%x\n",
        label, x9d.phase ? x9d.phase : "prepare", gate,
        x9d_rd(x9d.md_misc, 0x400), x9d_rd(misc, 0x6c), x9d_rd(x9d.ao, 0x1c),
        x9d_rd(x9d.ao, 0x80), x9d_rd(x9d.ao, 0x90), x9d_rd(misc, 0), x9d_rd(misc, 0x50),
        x9d_rd(misc, 0x40), x9d_rd(misc, 0x30));
    printf("X9 DPMAIF: %s TXQ0 config 0x%x/0x%x/0x%x indices 0x%x BAT indices 0x%x/0x%x FRG indices 0x%x/0x%x\n",
        label, x9d_rd(uls, 0x10), x9d_rd(uls, 0x14), x9d_rd(uls, 0x18), x9d_rd(uls, 0xe0),
        x9d_rd(dls, 0xd8), x9d_rd(dls, 0xdc), x9d_rd(dls, 0x78), x9d_rd(dls, 0x7c));
    // Keep undocumented latched fields RAW; do not claim guessed base/size semantics.
    for (unsigned q = 0; q < 2; q++) {
        unsigned o = q ? 0xb0 : 0x90;
        printf("X9 DPMAIF: %s PIT%u SRAM raw 0x%x/0x%x/0x%x/0x%x/0x%x/0x%x/0x%x\n",
            label, q, x9d_rd(dls, o), x9d_rd(dls, o + 4), x9d_rd(dls, o + 8),
            x9d_rd(dls, o + 12), x9d_rd(dls, o + 16), x9d_rd(dls, o + 20), x9d_rd(dls, o + 24));
    }
}
static void x9d_phase(const char *phase) {
    x9d.phase = phase; printf("X9 DPMAIF: init phase %s\n", phase);
}
// Raw register windows named by vendor drv3_dump_register (pd_ul ADD_DESC..
// DBG_STA9/ADD_DESC_CH, UL SRAM channel status 0xe0 = rd<<16|wr, AO UL,
// PD MISC L2TISAR0..CODA, DL pdn/SRAM, LRO). Register values only.
static void x9d_dump_window(const char *label, const char *name, uintptr_t base, unsigned off, unsigned bytes) {
    for (unsigned at = off; at < off + bytes; at += 32) {
        uint32_t v[8]; for (unsigned i = 0; i < 8; i++) v[i] = x9d_rd(base, at + 4 * i);
        printf("X9 DPMAIF: %s %s+0x%03x %08x %08x %08x %08x %08x %08x %08x %08x\n", label, name, at,
            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    }
}
static void x9d_dump(const char *label, bool dl) {
    if (!x9d.clocks_live || (x9d_rd(x9d.infra, 0xd20) & 12)) return;
    printf("X9 DPMAIF: %s dump: sw tx_wr %u tx_rd %u submitted %llu completed %llu rx %llu\n", label,
        x9d.tx_wr, x9d.tx_rd, x9d.tx_packets, x9d.tx_done, x9d.rx_packets);
    x9d_dump_window(label, "pd_ul", x9d.pd, 0x00, 0xc0);
    x9d_dump_window(label, "ul_sram", x9d.pd + 0xd00, 0x00, 0x100);
    x9d_dump_window(label, "ao_ul", x9d.ao, 0x00, 0x100);
    x9d_dump_window(label, "misc", x9d.pd + 0x400, 0x00, 0x100);
    if (!dl) return;
    x9d_dump_window(label, "pd_dl", x9d.pd + 0x100, 0x00, 0x100);
    x9d_dump_window(label, "dl_sram", x9d.pd + 0xc00, 0x00, 0x100);
    x9d_dump_window(label, "lro", x9d.pd + 0x900, 0x00, 0x60);
    x9d_dump_window(label, "ao_dl", x9d.ao + 0x400, 0x00, 0x100);
    x9d_dump_window(label, "hpc", x9d.pd + 0x600, 0x00, 0xa0); // vendor pd_mmw_hpc_base
    x9d_dump_window(label, "ao_misc_sram", x9d.pd + 0xe00, 0x00, 0x90);
}
static void x9d_fail(const char *reason) {
    if (x9d.failed) return;
    IOLockLock(x9d.lock); x9d.failed = true; x9d.ready = false;
    for (x9d_route &r : x9d.route) { if (r.bound) r.retiring = true; r.bound = false; r.families = 0; r.epoch++; }
    x9d.count = 0; IOLockUnlock(x9d.lock);
    printf("X9 DPMAIF: quarantined (%s), phase %s; DMA retained, no reset/retry\n",
        reason, x9d.phase ? x9d.phase : "prepare");
    x9d_snapshot("failure");
}
static bool x9d_expect(uintptr_t base, unsigned off, uint32_t mask, uint32_t expected) {
    uint64_t deadline = x9d_ms() + 20;
    unsigned spins = 0;
    uint32_t value;
    while (((value = x9d_rd(base, off)) & mask) != expected) {
        if (++spins >= 200000 || x9d_ms() >= deadline) {
            printf("X9 DPMAIF: readback offset 0x%x value 0x%x mask 0x%x expected 0x%x\n", off, value, mask, expected);
            x9d_fail("MMIO readback/deadline"); return false;
        }
    }
    return true;
}
static bool x9d_wait(uintptr_t base, unsigned off, uint32_t mask) { return x9d_expect(base, off, mask, 0); }
static bool x9d_command(uintptr_t base, unsigned off, uint32_t value) {
    if (!x9d_wait(base, off, X9D_BUSY_BIT)) return false;
    x9d_wr(base, off, value); return x9d_wait(base, off, X9D_BUSY_BIT);
}
static bool x9d_ip(const uint8_t *p, unsigned n) {
    if (!p || n < 20 || n > X9D_TX_SIZE) return false;
    if (p[0] >> 4 == 4) return (p[0] & 15) >= 5 && (p[0] & 15) * 4u <= n && ((unsigned)p[2] << 8 | p[3]) == n;
    return p[0] >> 4 == 6 && n >= 40 && ((unsigned)p[4] << 8 | p[5]) + 40 == n;
}
static unsigned x9d_sum(const uint8_t *p, unsigned n) {
    unsigned sum = 0;
    for (; n >= 2; p += 2, n -= 2) sum += (unsigned)p[0] << 8 | p[1];
    if (n) sum += (unsigned)p[0] << 8;
    return sum;
}
static const char *x9d_l4_sanity(const uint8_t *ip, unsigned n) {
    // Read-only, bounded diagnostics at the actual DMA submission boundary.
    // No checksum repair/offload, and no claim that MD or the network got it.
    if (!x9d_ip(ip, n)) return "bad-ip";
    bool v4 = (ip[0] >> 4) == 4;
    unsigned hdr = v4 ? (ip[0] & 15) * 4u : 40u, proto = ip[v4 ? 9 : 6];
    if (v4 && (((unsigned)ip[6] << 8 | ip[7]) & 0x3fff)) return "fragment-unchecked";
    // IPv6 extension headers need their own walk; do not call them bad.
    if (proto != 6 && proto != 17 && !(v4 && proto == 1) && !(!v4 && proto == 58)) return "unchecked";
    const uint8_t *p = ip + hdr; unsigned len = n - hdr;
    if (proto == 17) {
        if (len < 8) return "bad-length";
        unsigned udp = (unsigned)p[4] << 8 | p[5];
        if (udp < 8 || udp > len) return "bad-length";
        if (!p[6] && !p[7]) return v4 ? "v4-udp-disabled" : "BAD-zero-v6";
        len = udp;
    } else if (proto == 6) {
        if (len < 20 || (p[12] >> 4) < 5 || (p[12] >> 4) * 4u > len) return "bad-length";
    } else if (len < 4) return "bad-length";
    unsigned sum = x9d_sum(p, len);
    if (!(v4 && proto == 1)) sum += x9d_sum(ip + (v4 ? 12 : 8), v4 ? 8 : 32) + proto + len;
    while (sum >> 16) sum = (sum & 65535u) + (sum >> 16);
    return sum == 65535u ? "ok" : "BAD";
}
static unsigned x9d_buffer_offset(unsigned kind, unsigned bid) {
    return (bid % X9D_BUF_CHUNK) * (kind ? X9D_FRG_SIZE : X9D_PKT_SIZE);
}
static x9d_dma &x9d_buffer(unsigned kind, unsigned bid) {
    return x9d.pool[kind][bid / X9D_BUF_CHUNK];
}
static bool x9d_buffer_ensure(unsigned kind, unsigned bid) {
    x9d_dma &m = x9d_buffer(kind, bid);
    if (m.va) return true;
    uint64_t now = x9d_ms();
    if (now < x9d.bat_retry_ms[kind]) return false;
    x9d_dma fresh = {};
    if (!x9d_alloc(fresh, X9D_BUF_CHUNK * (kind ? X9D_FRG_SIZE : X9D_PKT_SIZE))) {
        // This fresh chunk has NEVER appeared in a BAT descriptor. Published
        // chunks are retained, even if an unrelated allocation later fails.
        if (fresh.md) fresh.md->release();
        if (x9d.bat_alloc_failures[kind] < 5) x9d.bat_alloc_failures[kind]++;
        x9d.bat_retry_ms[kind] = now + (1000u << (x9d.bat_alloc_failures[kind] - 1));
        if (x9d.bat_alloc_reports++ < 8)
            printf("X9 DPMAIF: BAT%u allocation deferred at BID %u; target %u credit %u backing %u, no fictitious credit\n",
                kind, bid, x9d.bat_target, x9d.bat_credit[kind], x9d.bat_backing[kind]);
        return false;
    }
    m = fresh; x9d.bat_backing[kind] += X9D_BUF_CHUNK;
    x9d.bat_alloc_failures[kind] = 0; x9d.bat_retry_ms[kind] = 0;
    return true;
}
static void x9d_bat_entry(unsigned kind, unsigned bid) {
    uint64_t pa = x9d_buffer(kind, bid).pa + x9d_buffer_offset(kind, bid);
    uint32_t *d = reinterpret_cast<uint32_t *>(x9d.bat[kind].va + bid * 8);
    d[0] = (uint32_t)pa; d[1] = (uint32_t)(pa >> 32) & 255;
}
static bool x9d_hw_init() {
    // Parent mux selection is preserved, not reprogrammed. No vendor reset.
    x9d_phase("clock/P2");
    x9d_wr(x9d.infra, 0xd28, 12); // ifrao main bit2, 26MHz bit3
    if (!x9d_wait(x9d.infra, 0xd20, 12)) return false;
    x9d.clocks_live = true;
    x9d_snapshot("before");
    x9d_wr(x9d.infra, 0xc24, 1u << 30);
    if (!x9d_expect(x9d.infra, 0xc20, 1u << 30, 1u << 30)) return false;
    uint32_t mode = x9d_rd(x9d.md_misc, 0x400);
    x9d_wr(x9d.md_misc, 0x400, (mode & ~1u) | 4);
    if (!x9d_expect(x9d.md_misc, 0x400, 5, 4)) return false;
    x9d_phase("SRAM-clear");
    const uint32_t sram[][2] = {
        {0xffff0100, 0x8033f000}, {0xffff0200, 0x801df000},
        {0xffff0400, 0x805ff000}, {0xffff1000, 0x8033f000},
        {0xffff4000, 0x80443000}, {0xffff8000, 0x80177000}
    };
    for (const auto &row : sram) {
        x9d_wr(x9d.pd + 0x400, 0xc4, row[0]);
        if (!x9d_command(x9d.pd + 0x400, 0xc0, row[1])) return false;
        printf("X9 DPMAIF: SRAM group 0x%x cleared\n", row[0] & 65535);
    }
    const uintptr_t misc = x9d.pd + 0x400, dl = x9d.pd + 0x100;
    const uintptr_t dls = x9d.pd + 0xc00, uls = x9d.pd + 0xd00, lro = x9d.pd + 0x900;
    x9d_wr(misc, 0x68, 0x7f);
    x9d_phase("IRQ-mask/config");
    // MT6993 bus remap belongs to MD. Do not write the older AP remap.
    x9d_wr(x9d.ao, 0x8c, (1u << 9) | (1u << 10) | (1u << 15) | (1u << 16) | (1u << 24));
    // Polled bring-up: do not register or unmask SPI 846/847 (GIC 878/879).
    // Vendor L2 masks (drv3_intr_hw_init): TIMCR0 = enable, TIMSR0 = ~enable.
    // The CPU still takes no interrupt (SPI 846/847 stay unregistered), but the
    // hardware sees the same mask state as with the vendor driver.
    x9d_wr(misc, 0, ~0u); // clear dummy UL status first, as vendor step 1
    x9d_wr(x9d.ao, 0x84, X9D_UL_L2_EN); x9d_wr(x9d.ao, 0x88, ~X9D_UL_L2_EN);
    x9d_wr(misc, 0x50, ~0u);
    x9d_wr(x9d.ao, 0x94, X9D_DL_L2_EN); x9d_wr(x9d.ao, 0x98, ~X9D_DL_L2_EN);
    if (!x9d_expect(x9d.ao, 0x80, X9D_UL_L2_EN | X9D_UL_FLOW, X9D_UL_FLOW & ~X9D_UL_L2_EN) ||
        !x9d_expect(x9d.ao, 0x90, X9D_DL_L2_EN | X9D_DL_LEGACY_PIT, X9D_DL_LEGACY_PIT)) return false;
    // Clear inherited dummy status before our queues exist; later fatal status
    // is preserved. IP-busy IRQ remains masked for the polled-only candidate.
    x9d_wr(misc, 0, ~0u); x9d_wr(misc, 0x50, ~0u);
    // drv3_intr_hw_init tail: clear AP IP busy, then UNMASK it (mask 0).
    x9d_wr(misc, 0x60, ~0u); x9d_wr(x9d.ao, 0x9c, 0);
    x9d_wr(misc, 0x6c, 0xff);
    x9d_wr(dls, 0x00, 64 | (4u << 8) | (1u << 16)); // 64B remain / 1 packet per BAT
    x9d_wr(dls, 0x04, 3080);
    x9d_wr(dls, 0x08, (124u << 24) | (13u << 8)); // PIT threshold / 1664B
    uint32_t cfg = x9d_rd(dls, 0x0c); // RDY_CHK_THRES (not FRG_THRES)
    cfg = (cfg & ~((3u << 22) | (63u << 16) | (1u << 31))) | (1u << 23) | (1u << 13) | (62u << 16) |
        (1u << 31); // ccci_dpmaif_bat_start: ccci_drv3_dl_set_ao_chksum_en(true); Android 0x80be2000
    x9d_wr(dls, 0x0c, cfg);
    x9d_wr(dls, 0x10, (1u << 28) | (15u << 8) | 62); // fragment BAT, 1920B
    // Initialize both PIT rings, preserving native HPC path mode2 (RX0 only).
    // Capability 2RXQ does not itself prove traffic is steered to both queues.
    // Explicitly disable all 16 aggregation bits.
    // Multiple normal buffers in one packet are an unexpected-LRO fault below,
    // never silently concatenated or reported as a successful received packet.
    // HPC_CNTL: Android live dump (android-atci-20261111) reads 0x10da, i.e.
    // add_mode=2 (bits 3:2) on top of the vendor-source 0x10d2. Match it.
    x9d_wr(dls, 0x38, 2 | (2u << 2) | (13u << 4) | (16u << 8));
    x9d_wr(dls, 0x20, 65535 | (50u << 16)); x9d_wr(dls, 0x24, 100);
    for (unsigned i = 0; i < 4; i++) {
        x9d_wr(dls, 0x28 + 4 * i, 100 | (100u << 16));
        x9d_wr(x9d.sram1, 0x40 + 4 * i, 100 | (100u << 16));
    }
    x9d_wr(x9d.ao + 0x400, 0x28, 0); x9d_wr(x9d.ao + 0x400, 0x34, 10);
    x9d_wr(x9d.sram1, 0x04, x9d_rd(x9d.sram1, 0x04) & ~65535u);
    if (!x9d_expect(x9d.sram1, 0x04, 65535, 0) ||
        !x9d_expect(dls, 0x04, ~0u, 3080)) return false;
    x9d_phase("PIT-init");
    // drv3_dl_set_ao_lro_pit_chknum reads the AO configuration view, then
    // writes DL SRAM. Reading back the SRAM-clear template instead retained
    // 0x100000ff in 212 (1ff0ffff vs Android's 0ff0ff00). Both fresh rings
    // use CHK_NUM=ff; take the non-CHK bits from AO, NOT the SRAM template.
    // This runs only before INIT_DONE/MD HS1, never on a live RX ring.
    const uint32_t seq_mask = (255u << 8) | (255u << 20);
    const uint32_t seq_ao = x9d_rd(x9d.ao + 0x400, 0x40);
    const uint32_t seq_before = x9d_rd(dls, 0x40);
    const uint32_t seq_config = (seq_ao & ~seq_mask) | seq_mask;
    x9d_wr(dls, 0x40, seq_config);
    if (!x9d_expect(dls, 0x40, ~0u, seq_config)) return false;
    printf("X9 DPMAIF: fresh PIT sequence AO 0x%x SRAM-before 0x%x configured 0x%x (not RX proof)\n",
        seq_ao, seq_before, seq_config);
    for (unsigned q = 0; q < 2; q++) {
        x9d_wr(lro, 0x14, (uint32_t)x9d.pit[q].pa);
        // drv3_dl_set_lro_pit_base_addr uses CON1[31:24], including on 6993.
        x9d_wr(lro, 0x18, X9D_PIT | (uint32_t)(x9d.pit[q].pa >> 32) << 24);
        x9d_wr(lro, 0x1c, 0); // reset INIT_CON2; do not inherit queue state
        x9d_wr(lro, 0x20, 1u << 20);
        x9d_wr(lro, 0x24, 0); // Not the high address register for the LRO PIT path.
        if (!x9d_expect(lro, 0x14, ~0u, (uint32_t)x9d.pit[q].pa) ||
            !x9d_expect(lro, 0x18, 0xff03ffff, X9D_PIT | (uint32_t)(x9d.pit[q].pa >> 32) << 24) ||
            !x9d_expect(lro, 0x20, 1u << 20, 1u << 20)) return false;
        x9d.published = true; // first queue INIT can expose our DMA; never free it
        if (!x9d_command(lro, 0, X9D_BUSY_BIT | 1 | (q << 16))) return false;
        printf("X9 DPMAIF: PIT%u INIT command complete (DMA reachability not yet proven)\n", q);
    }
    x9d_phase("BAT-init/credit");
    // ccci_dpmaif_bat_start order: dpmaif_bat_hw_init programs each BAT with
    // BAT_EN (CON1 bit16) CLEAR and BAT_INIT ALLSET (normal, then frag), then
    // add frag count, add normal count, and only then
    // ccci_drv_dl_all_frg_queue_en / all_queue_en: CON1 |= EN and BAT_INIT
    // with only the enable bit (bit0 = 0) | INIT_EN (| FRG_INIT for frag).
    for (unsigned kind = 0; kind < 2; kind++) {
        x9d_wr(dl, 0x08, (uint32_t)x9d.bat[kind].pa);
        x9d_wr(dl, 0x50, (uint32_t)(x9d.bat[kind].pa >> 32)); // 6993 BAT INIT_CON3
        x9d_wr(dl, 0x0c, X9D_BAT | (1u << 22)); // BAT_EN clear during ALLSET
        x9d_wr(dl, 0x10, 0); // INIT_CON2: initial indices
        if (!x9d_expect(dl, 0x08, ~0u, (uint32_t)x9d.bat[kind].pa) ||
            !x9d_expect(dl, 0x50, ~0u, (uint32_t)(x9d.bat[kind].pa >> 32)) ||
            !x9d_expect(dl, 0x0c, 0x0041ffff, X9D_BAT | (1u << 22))) return false;
        if (!x9d_command(dl, 0, X9D_BUSY_BIT | 1 | (kind << 16))) return false;
    }
    for (unsigned kind = 2; kind-- > 0;) // frag first, then normal
        if (!x9d_command(dl, 4, X9D_BUSY_BIT | X9D_CREDIT | (kind << 16))) return false;
    for (unsigned kind = 2; kind-- > 0;) {
        x9d_wr(dl, 0x0c, x9d_rd(dl, 0x0c) | (1u << 16));
        if (!x9d_command(dl, 0, X9D_BUSY_BIT | (kind << 16))) return false; // only-enable update
    }
    printf("X9 DPMAIF: BAT enable after credit (vendor order), CON1 0x%x\n", x9d_rd(dl, 0x0c));
    x9d_phase("TXQ0/SRAM-sync");
    // Feed TXQ0; all four queues are initialized below. DRB size is WORDS.
    x9d_wr(x9d.ao, 0x1c, 0);
    x9d_wr(uls, 0x10, (uint32_t)x9d.drb.pa); x9d_wr(uls, 0x14, X9D_DRB * 2);
    x9d_wr(uls, 0x18, (uint32_t)(x9d.drb.pa >> 32)); // 6993 UL_DRBSIZE_ADDRH_n_1
    // Android ccci_dump: "dpmaif-txq: 0-3(status=0xf)[2048]" - all four TX
    // queues started (drv3_txq_hw_init: DRB base/size, rdy + arb per queue).
    for (unsigned q = 1; q < 4; q++) {
        const x9d_dma &m = x9d.drbq[q - 1];
        x9d_wr(uls, 0x10 + 0x10 * q, (uint32_t)m.pa);
        x9d_wr(uls, 0x14 + 0x10 * q, (x9d_rd(uls, 0x14 + 0x10 * q) & ~65535u) | (X9D_DRB * 2));
        x9d_wr(uls, 0x18 + 0x10 * q, (uint32_t)(m.pa >> 32));
    }
    x9d_wr(x9d.ao, 0x1c, 15u | (15u << 8));
    // Mark buffers published BEFORE any INIT_DONE makes them DMA reachable.
    x9d.published = true;
    x9d_wr(misc, 0x90, x9d_rd(misc, 0x90) | 1);
    if (!x9d_wait(misc, 0x90, 1)) return false;
    // Verify documented initial indices BEFORE enabling DMA. Raw latched
    // base/size fields are captured, not validated against an invented layout.
    x9d_phase("initial-readback");
    x9d_snapshot("latched");
    if (!x9d_expect(uls, 0x10, ~0u, (uint32_t)x9d.drb.pa) ||
        !x9d_expect(uls, 0x14, 65535, X9D_DRB * 2) ||
        !x9d_expect(uls, 0x18, ~0u, (uint32_t)(x9d.drb.pa >> 32)) ||
        !x9d_expect(x9d.ao, 0x1c, 0x1f1f, 0xf0f) ||
        !x9d_expect(uls, 0xe0, ~0u, 0) ||
        !x9d_expect(dls, 0xa4, 0x3ffff, 0) || !x9d_expect(dls, 0xc4, 0x3ffff, 0) ||
        !x9d_expect(dls, 0xd8, 65535, 0) || !x9d_expect(dls, 0x78, 65535, 0) ||
        !x9d_expect(dls, 0xdc, 65535, X9D_CREDIT) ||
        !x9d_expect(x9d.sram1, 0x04, 65535, 0)) return false;
    x9d_phase("INIT_DONE");
    x9d_dump("pre-init-done", true);
    x9d_wr(x9d.ao, 0, 1); x9d_wr(x9d.ao + 0x400, 0, 1);
    // dpmaif_start: hw_init_done, then ccci_drv_clear_ip_busy (W1C misc+0x60).
    { uint32_t busy = x9d_rd(misc, 0x60); if (busy) x9d_wr(misc, 0x60, busy); }
    x9d_snapshot("enabled");
    IOLockLock(x9d.lock); x9d.ready = true; IOLockUnlock(x9d.lock);
    x9d.phase = "poll";
    printf("X9 DPMAIF: initialized, TXQ0 dual-PIT polled/native-path2, aggregation off; bearer route NOT proven\n");
    return true;
}
static void x9d_prepare() {
    if (x9d.prepared) return; x9d.prepared = true;
    PE_parse_boot_argn("x9_dpmaif", &x9d.mode, sizeof(x9d.mode));
    if (x9d.mode > 2) x9d.mode = 0;
    x9d.lock = IOLockAlloc();
    if (!x9d.lock) { x9d.failed = true; return; }
    if (!x9d.mode) { printf("X9 DPMAIF: disabled\n"); return; }
    x9d.bat_target = X9D_CREDIT;
    // Map only actively used DT windows. reg2 is MD misc, not old DL config.
    x9d.ao = ml_io_map(0x14117000, 0x1000); x9d.pd = ml_io_map(0x1410d000, 0x1000);
    x9d.md_misc = ml_io_map(0x1410c000, 0x1000); x9d.sram1 = ml_io_map(0x14100000, 0x1000);
    x9d.infra = ml_io_map(0x14126000, 0x1000);
    if (!x9d.ao || !x9d.pd || !x9d.md_misc || !x9d.sram1 || !x9d.infra) { x9d_fail("MMIO mapping"); return; }
    const uint32_t gate = x9d_rd(x9d.infra, 0xd20);
    printf("X9 DPMAIF: v3 MT6993 cap 0x7a dual RX/LRO, mode %u, clocks gated 0x%x\n", x9d.mode, gate & 12);
    if (!(gate & 12)) printf("X9 DPMAIF: probe CODA 0x%x UL state 0x%x (read-only)\n",
        x9d_rd(x9d.pd + 0x400, 0x6c), x9d_rd(x9d.pd + 0xd00, 0xe0));
    // No dereference of gated DPMAIF windows in probe mode.
    if (!x9d_alloc(x9d.drb, X9D_DRB * 8) || !x9d_alloc(x9d.txpool, X9D_DRB / 2 * X9D_TX_SIZE)) {
        x9d_fail("private TX DMA allocation/range"); return;
    }
    for (unsigned q = 0; q < 3; q++)
        if (!x9d_alloc(x9d.drbq[q], X9D_DRB * 8)) { x9d_fail("private TXQ1..3 DRB allocation/range"); return; }
    for (unsigned kind = 0; kind < 2; kind++) {
        if (!x9d_alloc(x9d.pit[kind], X9D_PIT * 16) || !x9d_alloc(x9d.bat[kind], X9D_BAT * 8)) {
            x9d_fail("private RX DMA allocation/range"); return;
        }
        for (unsigned bid = 0; bid < X9D_CREDIT; bid++) {
            if (!x9d_buffer_ensure(kind, bid)) { x9d_fail("initial BAT buffer allocation"); return; }
            x9d_bat_entry(kind, bid); x9d.posted[kind][bid] = true;
        }
        x9d.bat_wr[kind] = X9D_CREDIT;
        x9d.bat_owned[kind] = X9D_CREDIT; x9d.bat_credit[kind] = X9D_CREDIT;
        x9d_sync(x9d.bat[kind], 0, X9D_BAT * 8);
        // Each slot must differ from ITS expected first-lap sequence, including
        // slots 255/511/etc. A fixed 0xff poison would match those descriptors.
        uint32_t *pit = reinterpret_cast<uint32_t *>(x9d.pit[kind].va);
        for (unsigned i = 0; i < X9D_PIT; i++) pit[i * 4 + 3] = (i ^ 128u) & 255u;
        x9d_sync(x9d.pit[kind], 0, X9D_PIT * 16);
    }
    printf("X9 DPMAIF: cell158 private 40-bit DMA allocated, DRB high 0x%x PIT high 0x%x/0x%x BAT high 0x%x/0x%x, TX 2048/BAT 16384 (2000 credited, stable BID chunks)/PIT 0x8000x2; %s\n",
        (unsigned)(x9d.drb.pa >> 32), (unsigned)(x9d.pit[0].pa >> 32), (unsigned)(x9d.pit[1].pa >> 32),
        (unsigned)(x9d.bat[0].pa >> 32), (unsigned)(x9d.bat[1].pa >> 32),
        x9d.mode == 1 ? "probe only, no descriptors published" : "experimental initialization requested");
    // Physical HS2 can precede the serialized AT initializer's CFUN completion.
    // Active initialization is deferred until the MD owner is actually online.
}
static bool x9d_ready() { return x9d.ready && !x9d.failed; } // owner only
// bind/unbind/account/retirement APIs run only on the serialized bridge owner.
// The lock synchronizes network mailbox producers; it does not authorize
// other threads to publish, retire completion records or mutate lifetimes.
// Called only AFTER native MIPC has proved call ID -> interface/ccmni binding.
// Neither mux nor QMI client id is a physical ccmni channel.
static unsigned x9d_cap(const x9d_route &r,unsigned family) { return family==4?r.mtu4:family==6?r.mtu6:0; }
static bool x9d_caps(unsigned families,unsigned mtu4,unsigned mtu6) {
    return !(families&~3u) && (!(families&1)||(mtu4>=68&&mtu4<=X9D_TX_SIZE)) &&
        (!(families&2)||(mtu6>=1280&&mtu6<=X9D_TX_SIZE));
}
// Admission changes do NOT retire the physical call or its DMA ledger.
// Invalidate unpublished traffic under the producer lock; already published
// payloads keep their original lifetime and are accounted at completion.
static bool x9d_update(unsigned mux,uint64_t lifetime,unsigned families,unsigned mtu4,unsigned mtu6) {
    if(!x9d.lock||!cbp_mux_ok(mux)||!x9d_caps(families,mtu4,mtu6))return false;
    IOLockLock(x9d.lock);x9d_route &r=x9d.route[mux-1];
    if(!r.bound||r.lifetime!=lifetime){IOLockUnlock(x9d.lock);return false;}
    if(r.families!=families||r.mtu4!=mtu4||r.mtu6!=mtu6){
        r.families=(uint8_t)families;r.mtu4=mtu4;r.mtu6=mtu6;r.epoch++;
        unsigned kept=0,old=x9d.count;
        for(unsigned i=0;i<old;i++){
            unsigned from=(x9d.head+i)%X9D_MAIL;
            if(x9d.mail[from].mux==mux){if(x9d.mail[from].lifetime==r.lifetime)r.stats.tx_dropped++;continue;}
            unsigned to=(x9d.head+kept++)%X9D_MAIL;
            if(to!=from)memcpy(&x9d.mail[to],&x9d.mail[from],sizeof(x9d_tx));
        }
        for(unsigned i=kept;i<old;i++)bzero(&x9d.mail[(x9d.head+i)%X9D_MAIL],sizeof(x9d_tx));
        x9d.count=kept;
    }
    IOLockUnlock(x9d.lock);return true;
}
static bool x9d_bind(unsigned mux, unsigned channel, unsigned families, unsigned mtu4,unsigned mtu6,uint64_t &lifetime) {
    if (!x9d_ready() || !cbp_mux_ok(mux) || channel >= 20 || !families || !x9d_caps(families,mtu4,mtu6)) return false;
    IOLockLock(x9d.lock);
    x9d_route &r = x9d.route[mux - 1];
    if (x9d.used_channel[channel] || r.bound || r.retiring || x9d.next_lifetime == ~0ULL) { IOLockUnlock(x9d.lock); return false; }
    for (unsigned i = 0; i < CBP_INTERFACES; i++) if (i != mux - 1 && x9d.route[i].bound && x9d.route[i].channel == channel) {
        IOLockUnlock(x9d.lock); return false;
    }
    r.epoch++; r.lifetime = ++x9d.next_lifetime; r.channel = (uint8_t)channel; r.families = (uint8_t)families; r.mtu4 = mtu4; r.mtu6 = mtu6; r.bound = true;
    bzero(&r.stats, sizeof(r.stats));
    x9d.rx_account[channel].lifetime = r.lifetime; // unused channel only; no RX ledger reset
    x9d.used_channel[channel] = true; x9d.bind_ms = x9d_ms(); x9d.rx_dumps = 0; x9d.tx_diag = x9d.tx_diag6 = 0;
    lifetime = r.lifetime; IOLockUnlock(x9d.lock); return true;
}
static void x9d_unbind(unsigned mux) {
    if (!x9d.lock || !cbp_mux_ok(mux)) return;
    IOLockLock(x9d.lock); x9d_route &r = x9d.route[mux - 1];
    if (r.bound) { r.retiring = true; r.bound = false; r.families = 0; r.epoch++; }
    // An unbound mux cannot admit new packets under this same lock. Retire
    // its unpublished mailbox entries now, even when a full DRB ring keeps
    // tx_poll from dequeuing. Preserve every other mux's FIFO order. Never
    // touch payloads or completions that have already been given to DMA.
    unsigned kept = 0, old_count = x9d.count;
    for (unsigned i = 0; i < old_count; i++) {
        unsigned from = (x9d.head + i) % X9D_MAIL;
        if (x9d.mail[from].mux == mux) {
            if (x9d.mail[from].lifetime == r.lifetime) r.stats.tx_dropped++;
            continue;
        }
        unsigned to = (x9d.head + kept++) % X9D_MAIL;
        if (to != from) memcpy(&x9d.mail[to], &x9d.mail[from], sizeof(x9d_tx));
    }
    for (unsigned i = kept; i < old_count; i++)
        bzero(&x9d.mail[(x9d.head + i) % X9D_MAIL], sizeof(x9d_tx));
    x9d.count = kept;
    IOLockUnlock(x9d.lock);
}
// Bridge-owner only: after unbind closes admission, wait for every queued
// or hardware-published payload before final AP TX accounting handoff.
// Completion records are retired only at the payload's even DRB boundary,
// not when its MSG descriptor has been read. This does not prove RX drain,
// MD channel release, or authorize used_channel reuse by itself.
static bool x9d_tx_quiescent(unsigned mux) {
    if (!x9d.lock || !cbp_mux_ok(mux) || !x9d_ready()) return false;
    IOLockLock(x9d.lock);
    bool idle = !x9d.route[mux - 1].bound;
    for (unsigned i = 0; idle && i < x9d.count; i++) {
        const x9d_tx &t = x9d.mail[(x9d.head + i) % X9D_MAIL];
        if (t.mux == mux) idle = false;
    }
    // Only this owner mutates completion[] or publishes DRBs. Network
    // producers hold lock and cannot enqueue after the unbind above.
    unsigned inflight = 0;
    for (unsigned i = 0; idle && i < X9D_DRB / 2; i++)
        if (x9d.completion[i].mux == mux && x9d.completion[i].size) inflight++;
    IOLockUnlock(x9d.lock);
    // A stalled UL engine (no read-index progress for 5 s) must not hold the
    // mux forever (193.txt: 64 DRBs never completed, every later Start BUSY).
    // Late completions of the old lifetime are then dropped, not fatal.
    if (idle && inflight) {
        if (x9d_ms() - x9d.tx_progress_ms < 5000) return false;
        printf("X9 DPMAIF: mux %u retiring with %u TX descriptors stalled in hardware\n", mux, inflight);
    }
    return idle;
}
static void x9d_stats_locked(const x9d_route &r,x9d_stats &stats) {
    stats = r.stats;
    const x9d_rx_account &rx = x9d.rx_account[r.channel];
    if (rx.lifetime == r.lifetime) {
        stats.rx_packets = rx.packets; stats.rx_bytes = rx.bytes;
        stats.rx_errors = rx.errors; stats.rx_overflows = rx.overflows; stats.rx_dropped = rx.dropped;
    }
}
static bool x9d_account(unsigned mux,uint64_t lifetime,x9d_stats &stats) {
    if (!x9d.lock || !cbp_mux_ok(mux) || !lifetime) return false;
    IOLockLock(x9d.lock); const x9d_route &r = x9d.route[mux - 1];
    bool valid = r.lifetime == lifetime;
    if (valid) x9d_stats_locked(r,stats);
    IOLockUnlock(x9d.lock); return valid;
}
// Owner-only final TX accounting handoff. A retained ledger prevents a new
// bind on this mux until every original published PAYLOAD has retired. RX
// delivery bytes stop at admission closure; RX drop totals remain a snapshot
// of the independent retained RX ledger. This is NOT a final RX/MD flush.
static bool x9d_retire_finish(unsigned mux,uint64_t lifetime,x9d_stats &stats) {
    if (!lifetime || !x9d_tx_quiescent(mux)) return false;
    IOLockLock(x9d.lock); x9d_route &r = x9d.route[mux - 1];
    bool valid = r.retiring && r.lifetime == lifetime;
    if (valid) {
        x9d_stats_locked(r,stats); r.retiring = false;
        bool other = false;
        for (unsigned i = 0; i < CBP_INTERFACES; i++) if (i != mux - 1 && (x9d.route[i].bound || x9d.route[i].retiring) && x9d.route[i].channel == r.channel) other = true;
        if (!other) x9d.used_channel[r.channel] = false; // ccmni channels are reused by the MD/vendor driver
    }
    IOLockUnlock(x9d.lock); return valid;
}
static bool x9d_retiring() {
    if (!x9d.lock) return false;
    IOLockLock(x9d.lock); bool pending = false;
    for (const x9d_route &r : x9d.route) if (r.retiring) pending = true;
    IOLockUnlock(x9d.lock); return pending;
}
static int x9d_send(unsigned mux, const uint8_t *ip, unsigned n,uint64_t epoch) {
    if (!x9d.lock || !cbp_mux_ok(mux)) { x9d_tx_count(X9DP_TX_ARGUMENT); return ENETDOWN; }
    if (!x9d_ip(ip, n)) { x9d_tx_count(X9DP_TX_IP); return EINVAL; }
    IOLockLock(x9d.lock); x9d_route &r = x9d.route[mux - 1];
    if (!x9d.ready || x9d.failed || !r.bound || !epoch || r.epoch!=epoch || !(r.families & cbp_family_bit(ip[0] >> 4)) || n > x9d_cap(r,ip[0]>>4)) {
        // Classify the first failing predicate without changing the original errno.
        x9d_tx_count(!x9d.ready || x9d.failed ? X9DP_TX_OFFLINE : !r.bound ? X9DP_TX_ROUTE :
            !epoch || r.epoch != epoch ? X9DP_TX_EPOCH :
            !(r.families & cbp_family_bit(ip[0] >> 4)) ? X9DP_TX_FAMILY : X9DP_TX_MTU);
        // Post-unbind unversioned producers do not belong to the old TX ledger.
        x9d.rejected++; if (r.bound) r.stats.tx_dropped++; IOLockUnlock(x9d.lock); return n > x9d_cap(r,ip[0]>>4) ? EMSGSIZE : ENETDOWN;
    }
    // Backpressure is retryable; don't count a retained packet as dropped.
    if (x9d.count == X9D_MAIL) { x9d_tx_count(X9DP_TX_FULL); IOLockUnlock(x9d.lock); return EBUSY; }
    x9d_tx &t = x9d.mail[(x9d.head + x9d.count) % X9D_MAIL];
    t.epoch = r.epoch; t.lifetime = r.lifetime; t.size = (uint16_t)n; t.mux = (uint8_t)mux; memcpy(t.ip, ip, n); x9d.count++;
    x9d_tx_count(X9DP_TX_ACCEPT);
    IOLockUnlock(x9d.lock); return 0; // accepted into bounded mailbox, NOT completion
}
static bool x9d_tx_poll() {
    uint32_t words = x9d_rd(x9d.pd + 0xd00, 0xe0) >> 16;
    if ((words & 1) || words >= X9D_DRB * 2) { x9d_fail("TX hardware read index"); return false; }
    unsigned rd = words / 2, done = (rd + X9D_DRB - x9d.tx_rd) % X9D_DRB;
    unsigned used = (x9d.tx_wr + X9D_DRB - x9d.tx_rd) % X9D_DRB;
    if (done > used) { x9d_fail("TX completion exceeds published descriptors"); return false; }
    // MD may retire the message and payload separately. Payload remains pinned
    // until the next even descriptor boundary; pool reuse uses ring free space.
    for (unsigned i = 0, at = x9d.tx_rd; i < done; i++, at = (at + 1) % X9D_DRB) {
        if (!(at & 1)) continue; // MSG alone is not a completed IP packet.
        x9d_completion &c = x9d.completion[at / 2];
        if (!cbp_mux_ok(c.mux) || !c.size) { x9d_fail("TX completion ownership"); return false; }
        IOLockLock(x9d.lock); x9d_route &r = x9d.route[c.mux - 1];
        bool valid = c.lifetime && r.lifetime == c.lifetime;
        if (valid) { r.stats.tx_packets++; r.stats.tx_bytes += c.size; }
        IOLockUnlock(x9d.lock);
        if (!valid) x9d.stale_completions++; // retired after a stall; no ledger left to credit
        bzero(&c, sizeof(c)); x9d.tx_done++;
    }
    x9d.tx_rd = rd;
    for (unsigned budget = 0; budget < X9D_TX_BUDGET; budget++) {
        unsigned free = X9D_DRB - 1 - (x9d.tx_wr + X9D_DRB - x9d.tx_rd) % X9D_DRB;
        if (free < 2) break;
        static x9d_tx t; uint8_t channel;
        IOLockLock(x9d.lock);
        if (!x9d.count) { IOLockUnlock(x9d.lock); break; }
        t = x9d.mail[x9d.head]; x9d.head = (x9d.head + 1) % X9D_MAIL; x9d.count--;
        x9d_route &r = x9d.route[t.mux - 1];
        bool valid = r.bound && r.epoch == t.epoch && r.lifetime == t.lifetime && (r.families & cbp_family_bit(t.ip[0] >> 4)) && t.size<=x9d_cap(r,t.ip[0]>>4);
        if(!valid&&r.lifetime==t.lifetime)r.stats.tx_dropped++; channel = r.channel;
        IOLockUnlock(x9d.lock); if (!valid) { bzero(&t, sizeof(t)); continue; }
        unsigned index = x9d.tx_wr, pool = index / 2 * X9D_TX_SIZE;
        memcpy(x9d.txpool.va + pool, t.ip, t.size); x9d_sync(x9d.txpool, pool, t.size);
        if (x9d.tx_diag < 12 && t.size >= 20 && (t.ip[0] >> 4) == 4) {
            // UL sanity after each bind: header checksum, addresses, L4 port.
            x9d.tx_diag++; const uint8_t *h = t.ip; unsigned ihl = (h[0] & 15) * 4, sum = 0;
            for (unsigned i = 0; i < ihl && i + 1 < t.size; i += 2) sum += (unsigned)h[i] << 8 | h[i + 1];
            while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
            unsigned port = (h[9] == 6 || h[9] == 17) && t.size >= ihl + 4 ? ((unsigned)h[ihl + 2] << 8 | h[ihl + 3]) : 0;
            printf("X9 DPMAIF: UL[%u] ch %u v4 proto %u len %u ttl %u hdrsum %s l4sum %s %u.%u.%u.%u -> %u.%u.%u.%u port %u\n",
                x9d.tx_diag, channel, h[9], t.size, h[8], sum == 0xffff ? "ok" : "BAD",
                x9d_l4_sanity(h, t.size),
                h[12], h[13], h[14], h[15], h[16], h[17], h[18], h[19], port);
        }
        if (x9d.tx_diag6 < 3 && t.size >= 40 && (t.ip[0] >> 4) == 6) {
            x9d.tx_diag6++;
            printf("X9 DPMAIF: UL6[%u] ch %u next-header %u len %u hop-limit %u l4sum %s\n",
                x9d.tx_diag6, channel, t.ip[6], t.size, t.ip[7], x9d_l4_sanity(t.ip, t.size));
        }
        uint64_t pa = x9d.txpool.pa + pool;
        uint32_t *d = reinterpret_cast<uint32_t *>(x9d.drb.va + index * 8);
        d[0] = 1 | 4 | ((uint32_t)t.size << 16); d[1] = (uint32_t)channel << 16; // MSG dtype=1/cbit=1
        d[2] = ((uint32_t)(pa >> 32) << 8) | ((uint32_t)t.size << 16); d[3] = (uint32_t)pa; // PD dtype=0/cbit=0
        x9d_sync(x9d.drb, index * 8, 16);
        x9d.completion[index / 2] = {t.lifetime, t.size, t.mux};
        // No checksum offloads advertised by PDP; stack supplies IP/L4 checksums.
        x9d.tx_wr = (index + 2) % X9D_DRB; x9d.tx_packets++; bzero(&t, sizeof(t));
        if (!x9d_command(x9d.pd, 0xb0, X9D_BUSY_BIT | 4)) return false; // two 8B DRBs -> four WORDS
    }
    return true;
}
static bool x9d_rx_item(unsigned q, const uint32_t *d) {
    x9d_rx &r = x9d.rx[q]; uint32_t a = d[0];
    if (a & 1) { // MSG PIT, channel bits16..23, drop bit31
        if (r.message) { x9d_fail("interleaved RX message"); return false; }
        if (!(a & 2)) { x9d_fail("message PIT without continuation"); return false; }
        r.message = true; r.normal = false; r.bytes = 0; r.channel = (uint8_t)(a >> 16);
        r.mux = 0; r.epoch = 0; r.lifetime = 0;
        IOLockLock(x9d.lock);
        for (unsigned i = 0; i < CBP_INTERFACES; i++) if (x9d.route[i].bound && x9d.route[i].channel == r.channel) {
            r.mux = i + 1; r.epoch = x9d.route[i].epoch; r.lifetime = x9d.route[i].lifetime;
        }
        IOLockUnlock(x9d.lock);
        r.drop = (a & (1u << 31)) || (a & (1u << 4)); return true;
    }
    if (!r.message) { x9d_fail("RX payload without message"); return false; }
    unsigned kind = (a >> 2) & 1, bid = ((a >> 3) & 8191) | (((d[3] >> 8) & 7) << 13);
    unsigned len = a >> 16, hoff = ((d[3] >> 19) & 31) * 4;
    if (bid >= X9D_BAT || !x9d.posted[kind][bid] || (!kind && r.normal) || (kind && !r.normal)) {
        x9d_fail("RX BID ownership or unexpected aggregation"); return false;
    }
    const x9d_dma &buffer = x9d_buffer(kind, bid);
    if (!buffer.va || !x9d.bat_owned[kind]) { x9d_fail("RX missing BAT backing"); return false; }
    unsigned stride = kind ? X9D_FRG_SIZE : X9D_PKT_SIZE, off = x9d_buffer_offset(kind, bid);
    uint64_t addr = d[1] | ((uint64_t)d[2] << 32), base = buffer.pa + off;
    // Buffer address is observed in PIT but vendor treats BAT as authoritative.
    // Reject out-of-buffer addresses; do not follow arbitrary physical pointers.
    if (!len || len > stride || hoff > stride - len || addr < base || addr >= base + stride) {
        x9d_fail("RX DMA address/length"); return false;
    }
    if (kind && hoff) { x9d_fail("fragment header offset"); return false; }
    unsigned bytes = len + (kind ? 0 : hoff);
    if (bytes > sizeof(r.ip) - r.bytes) { x9d_fail("RX packet exceeds bounded MTU"); return false; }
    x9d_sync(buffer, off, stride);
    memcpy(r.ip + r.bytes, buffer.va + off, bytes); r.bytes += bytes; r.normal = true;
    x9d.posted[kind][bid] = false; // CPU copy complete; global BAT replenisher owns reuse
    x9d.bat_owned[kind]--;
    if (!(a & 2)) {
        x9d.rx_wire_bytes += r.bytes; // completed copies, including drops; NOT successful delivery
        unsigned mux = 0; int (*receiver)(uint32_t, const uint8_t *, uint32_t, uint32_t, uint64_t);
        IOLockLock(x9d.lock);
        for (unsigned i = 0; i < CBP_INTERFACES; i++) if (x9d.route[i].bound && x9d.route[i].channel == r.channel &&
            i + 1 == r.mux && x9d.route[i].epoch == r.epoch && x9d.route[i].lifetime == r.lifetime && r.bytes <= x9d_cap(x9d.route[i],r.ip[0]>>4) &&
            (x9d.route[i].families & cbp_family_bit(r.ip[0] >> 4))) mux = i + 1;
        receiver = x9d.receiver; IOLockUnlock(x9d.lock);
        bool valid = !r.drop && mux && receiver && x9d_ip(r.ip, r.bytes);
        // No route lock across the network KPI. Bind/unbind and this RX path
        // run on the same board owner thread; the helper separately validates
        // its family gate and the immutable epoch carried by this packet.
        int input_error = valid ? receiver(mux, r.ip, r.bytes, r.ip[0] >> 4, r.epoch) : 0;
        bool accepted = valid && input_error == 0;
        unsigned disposition = r.drop ? 1u : !mux ? 2u : !receiver ? 3u : !valid ? 4u : !accepted ? 5u : 0u;
        uint64_t seen = ++x9d.rx_disposition[disposition];
        if (seen <= 4 || !(seen & (seen - 1)))
            printf("X9 CELL RX: q %u disposition %u seen %llu mux %u channel %u epoch %llu lifetime %llu bytes %u input-error %d accepted %u\n",
                q, disposition, seen, r.mux, r.channel, r.epoch, r.lifetime, r.bytes, input_error, accepted);
        IOLockLock(x9d.lock);
        if (cbp_mux_ok(r.mux) && r.channel < 20 && r.lifetime && x9d.rx_account[r.channel].lifetime == r.lifetime) {
            x9d_rx_account &s = x9d.rx_account[r.channel];
            if (accepted) { s.packets++; s.bytes += r.bytes; } else s.dropped++;
        }
        IOLockUnlock(x9d.lock);
        if (accepted) x9d.rx_packets++;
        else x9d.rx_dropped++;
        bzero(r.ip, r.bytes); r.bytes = 0; r.message = r.normal = false;
    }
    return true;
}
static void x9d_rx_probe(unsigned q, uint32_t raw, const char *status, bool force = false) {
    uint64_t now = x9d_ms(); if (!force && now < x9d.rx_probe_next[q]) return;
    x9d.rx_probe_next[q] = now + 3000; const x9d_rx &r = x9d.rx[q];
    printf("X9 CELL PIT: q %u status %s raw-wr 0x%x wr %u rd %u consumed %llu seq %u wait-ms %llu partial %u/%u bytes %u channel %u mux %u epoch %llu lifetime %llu\n",
        q, status, raw, raw & 0x3ffff, r.rd, x9d.pit_consumed[q], r.seq,
        r.seq_wait ? now - r.seq_wait : 0, r.message, r.normal, r.bytes, r.channel, r.mux, r.epoch, r.lifetime);
}
static bool x9d_rx_poll(unsigned q) {
    x9d_rx &r = x9d.rx[q]; uint32_t raw = x9d_rd(x9d.pd + 0xc00, q ? 0xc4 : 0xa4);
    unsigned wr = raw & 0x3ffff;
    if (wr >= X9D_PIT) { x9d_rx_probe(q, raw, "bad-producer", true); x9d_fail("RX hardware write index"); return false; }
    unsigned consumed = 0, released = 0;
    while (r.rd != wr && consumed < X9D_BUDGET) {
        x9d_sync(x9d.pit[q], r.rd * 16, 16);
        uint32_t item[4]; memcpy(item, x9d.pit[q].va + r.rd * 16, sizeof(item));
        if ((item[3] & 255) != r.seq) {
            // Producer index can become visible before DMA data. Bounded defer,
            // not a busy loop and not acceptance of a stale/poison descriptor.
            if (!r.seq_wait) { r.seq_wait = x9d_ms(); x9d_rx_probe(q, raw, "sequence-wait", true); }
            else if (x9d_ms() - r.seq_wait >= 20) { x9d_rx_probe(q, raw, "sequence-timeout", true); x9d_fail("RX sequence deadline"); }
            break;
        }
        r.seq_wait = 0;
        if (!x9d_rx_item(q, item)) { x9d_rx_probe(q, raw, "item-rejected", true); return false; }
        r.rd = (r.rd + 1) % X9D_PIT; r.seq++; consumed++;
        x9d.pit_consumed[q]++;
        if (!(r.rd & 3)) {
            // Eight-bit sequence wraps 128 times in this 32768-entry PIT ring:
            // after a whole lap the old sequence could equal the new one.
            // Poison each fully consumed cache line BEFORE returning credits.
            // Never dirty a line containing a neighboring in-flight DMA PIT.
            // Up to three consumed entries stay reserved across poll calls.
            unsigned first = (r.rd + X9D_PIT - 4) % X9D_PIT;
            uint32_t *line = reinterpret_cast<uint32_t *>(x9d.pit[q].va + first * 16);
            for (unsigned i = 0; i < 4; i++) line[i * 4 + 3] ^= 128;
            x9d_sync(x9d.pit[q], first * 16, 64); released += 4;
        }
    }
    if (x9d.failed) return false;
    // Return only fully consumed/poisoned cache-line groups; serialized shared
    // command register. The 1..3 remaining entries have not been credited yet.
    bool ok = !released || x9d_command(x9d.pd + 0x900, 0x10, X9D_BUSY_BIT | (q << 20) | released);
    x9d_rx_probe(q, raw, !ok ? "credit-failed" : r.seq_wait ? "sequence-wait" :
        r.rd != wr ? "budget-pending" : consumed ? "consumed" : "empty", !ok);
    return ok;
}
static void x9d_bat_policy() {
    // MT6993 dpmaif_updata_max_bat_skb_cnt runs even when no PIT is ready.
    // CODA is an ordinary R/W mailbox, not W1C. Acknowledgement means the
    // notification was consumed, never that any allocation/credit succeeded.
    const uintptr_t misc = x9d.pd + 0x400;
    uint32_t raw = x9d_rd(misc, 0x6c);
    if (raw & X9D_BAT_HINT_PENDING) {
        unsigned hint = raw >> 16;
        if (hint >= X9D_CREDIT) x9d.bat_hint = hint;
        else x9d.bat_invalid_hints++; // vendor ignores invalid small values
        x9d.bat_hint_events++;
        x9d_wr(misc, 0x6c, raw & ~X9D_BAT_HINT_PENDING);
        // Do not demand an exact readback: MD may publish another update.
        if (x9d.bat_hint_reports++ < 12)
            printf("X9 DPMAIF: BAT hint 0x%x count %u %s; consumed notification, capacity/delivery not acknowledged\n",
                raw, hint, hint < X9D_CREDIT ? "ignored-invalid" : hint == X9D_BAT_HINT_MAX ? "max-request" : "adaptive");
    }
    uint64_t now = x9d_ms();
    if (!x9d.bat_rate_ms) { x9d.bat_rate_ms = now; x9d.bat_rate_bytes = x9d.rx_wire_bytes; }
    uint64_t elapsed = now - x9d.bat_rate_ms;
    if (elapsed >= 1000) {
        // Same 150 Mbit/s low/high boundary as the phone driver's allocation
        // policy; finite hints are not mandatory preload counts or hard caps.
        uint64_t bytes = x9d.rx_wire_bytes - x9d.bat_rate_bytes;
        x9d.bat_high_rate = bytes > elapsed * 18750;
        x9d.bat_rate_ms = now; x9d.bat_rate_bytes = x9d.rx_wire_bytes;
    }
    x9d.bat_target = x9d.bat_hint == X9D_BAT_HINT_MAX || x9d.bat_high_rate ? X9D_BAT - 1 : X9D_CREDIT;
}
static bool x9d_replenish(unsigned kind) {
    const uintptr_t dls = x9d.pd + 0xc00;
    unsigned rd = x9d_rd(dls, kind ? 0x78 : 0xd8) & 65535;
    if (rd >= X9D_BAT) { x9d_fail("BAT hardware read index"); return false; }
    unsigned n = 0, w = x9d.bat_wr[kind], stride = kind ? X9D_FRG_SIZE : X9D_PKT_SIZE;
    unsigned credit = (w + X9D_BAT - rd) % X9D_BAT;
    x9d.bat_credit[kind] = credit;
    if (credit >= x9d.bat_target) return true; // never revoke a published BID on a lower target
    unsigned demand = x9d.bat_target - credit;
    // Keep one entry empty; require BOTH hardware consumption and CPU release.
    // A BID held by either RX queue stops the contiguous refill, never overwrites.
    while ((w + 1) % X9D_BAT != rd && n < demand && n < X9D_BUDGET &&
           !x9d.posted[kind][w] && !x9d.posted[kind][(w + 1) % X9D_BAT]) {
        if (!x9d_buffer_ensure(kind, w)) break; // retain progress, retry later; no invented credits
        x9d_sync(x9d_buffer(kind, w), x9d_buffer_offset(kind, w), stride); x9d_bat_entry(kind, w);
        x9d_sync(x9d.bat[kind], w * 8, 8); x9d.posted[kind][w] = true;
        x9d.bat_owned[kind]++;
        w = (w + 1) % X9D_BAT; n++;
    }
    if (!n) return true;
    x9d.bat_wr[kind] = w;
    if (!x9d_command(x9d.pd + 0x100, 4, X9D_BUSY_BIT | (kind << 16) | n)) return false;
    x9d.bat_credit[kind] += n;
    return true;
}
static void x9d_poll() {
    if (x9d.mode == 2 && !x9d.hw_attempted && !x9d.failed && x9_md_at_online()) {
        x9d.hw_attempted = true; (void)x9d_hw_init();
    }
    if (!x9d_ready()) return;
    // DPMAIF now starts at BOOT_WAITING_FOR_HS1, before the AT/MIPC transport
    // is online. Only a transport that went DOWN after being up is fatal.
    if (!x9_md_at_online()) { if (x9d.at_seen) x9d_fail("MD/AT offline"); return; }
    if (!x9d.at_seen) { x9d.at_seen = true; x9d_dump("at-online", true); }
    if (x9d_rd(x9d.infra, 0xd20) & 12) { x9d_fail("clock gated while active"); return; }
    uint32_t ul = x9d_rd(x9d.pd + 0x400, 0), dl = x9d_rd(x9d.pd + 0x400, 0x50);
    uint32_t checked_dl = dl;
    const uint32_t legacy = dl & (X9D_DL_LEGACY_PIT | X9D_DL_LEGACY_DONE) & x9d_rd(x9d.ao, 0x90);
    if (legacy && (x9d_rd(x9d.pd + 0xc00, 0x38) & 3u) == 2u) {
        // 175: masked count bit3; Android live proc_ccci_dump2: masked QDONE
        // bit0 with working LRO RX. drv3_isr0 filters both before handling.
        // Neither is an unknown LRO error. Keep ALL LRO/DMA errors fatal;
        // do not W1C either masked legacy bit or mask a newly unmasked source.
        checked_dl &= ~legacy;
        if (!x9d.legacy_pit_reported) {
            x9d.legacy_pit_reported = true;
            printf("X9 DPMAIF: masked legacy status 0x%x in LRO path2; LRO/DMA errors remain fatal\n", legacy);
        }
    }
    // EMPTY / MD-not-ready / power-not-ready are flow control, not DMA length
    // faults (native drv3_isr masks PWR_NOTRDY until subsequent TX_DONE).
    // Do not ACK unknown/error bits or reset the modem to hide the evidence.
    if ((ul & (X9D_UL_ERROR | ~(15u | X9D_UL_FLOW | X9D_UL_ERROR))) ||
        (checked_dl & (X9D_DL_ERROR | ~(X9D_DL_DONE | X9D_DL_EMPTY | X9D_DL_ERROR)))) {
        x9d_fail("hardware interrupt error/unknown status"); return;
    }
    if ((ul & X9D_UL_FLOW) || (dl & X9D_DL_EMPTY)) x9d.flow_events++;
    if ((ul != x9d.last_ul || dl != x9d.last_dl) && (ul || dl) && x9d.irq_reports < 8) {
        x9d.irq_reports++;
        printf("X9 DPMAIF: polled IRQ 0x%x/0x%x, flow 0x%x/0x%x\n", ul, dl, ul & X9D_UL_FLOW, dl & X9D_DL_EMPTY);
    }
    x9d.last_ul = ul; x9d.last_dl = dl;
    x9d_bat_policy(); // notification must not depend on a nonzero PIT producer
    if (!x9d_tx_poll() || !x9d_rx_poll(0) || !x9d_rx_poll(1) || !x9d_replenish(0) || !x9d_replenish(1)) return;
    uint64_t now = x9d_ms();
    if (x9d.tx_done != x9d.tx_seen_done || x9d.tx_done == x9d.tx_packets) {
        x9d.tx_seen_done = x9d.tx_done; x9d.tx_progress_ms = now; x9d.stall_reported = false;
    } else if (now - x9d.tx_progress_ms >= 1000 && !x9d.stall_reported && x9d.stall_dumps < 3) {
        x9d.stall_dumps++; x9d.stall_reported = true; // one dump per stall
        printf("X9 DPMAIF: TX stalled: hardware read index %u, %llu of %llu payloads completed\n", x9d.tx_rd, x9d.tx_done, x9d.tx_packets);
        x9d_dump("tx-stall", true);
    }
    if (x9d.bind_ms && now - x9d.bind_ms >= 8000 && !x9d.rx_packets && x9d.tx_done && x9d.rx_dumps < 1) {
        x9d.rx_dumps++; printf("X9 DPMAIF: no downlink 8 s after bind with %llu uplink completed\n", x9d.tx_done);
        x9d_dump("rx-none", true);
    }
    // drv3_isr: acknowledge only UNMASKED status (UL EMPTY/MD_NOTREADY and
    // masked DL sources are never written back), and clear IP busy each pass.
    uint32_t ul_ack = ul & ~x9d_rd(x9d.ao, 0x80), dl_ack = dl & ~x9d_dl_mask_to_sts(x9d_rd(x9d.ao, 0x90));
    if (ul_ack) x9d_wr(x9d.pd + 0x400, 0, ul_ack);
    if (dl_ack) x9d_wr(x9d.pd + 0x400, 0x50, dl_ack);
    { uint32_t busy = x9d_rd(x9d.pd + 0x400, 0x60); if (busy) x9d_wr(x9d.pd + 0x400, 0x60, busy); }
}
static void x9d_report(unsigned second) {
    if (!x9d.lock || !x9d.mode) return;
    IOLockLock(x9d.lock); uint64_t rejected = x9d.rejected; unsigned queued = x9d.count; IOLockUnlock(x9d.lock);
    printf("X9 DPMAIF: BAT policy hint %u events %llu invalid %llu target %u credit %u/%u owned %u/%u backing %u/%u max %u per kind\n",
        x9d.bat_hint, x9d.bat_hint_events, x9d.bat_invalid_hints, x9d.bat_target,
        x9d.bat_credit[0], x9d.bat_credit[1], x9d.bat_owned[0], x9d.bat_owned[1],
        x9d.bat_backing[0], x9d.bat_backing[1], X9D_BAT - 1);
    printf("X9 DPMAIF: %u s mode %u ready %u quarantined %u published %u TX submitted %llu completed %llu RX delivered %llu dropped %llu queued %u rejected %llu flow %llu phase %s\n",
        second, x9d.mode, x9d.ready, x9d.failed, x9d.published, x9d.tx_packets, x9d.tx_done, x9d.rx_packets, x9d.rx_dropped, queued, rejected, x9d.flow_events,
        x9d.phase ? x9d.phase : "prepare");
    uint64_t counts[9]; for (unsigned i = 0; i < 9; i++) counts[i] = __atomic_load_n(&x9d.tx_admit[i], __ATOMIC_RELAXED);
    printf("X9 CELL TX: mailbox accept %llu argument %llu bad-ip %llu offline %llu unbound %llu epoch %llu family %llu mtu %llu full %llu; completion-not-air\n",
        counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7], counts[8]);
    printf("X9 CELL RX: totals accepted %llu descriptor-drop %llu route %llu no-receiver %llu bad-ip %llu input-error %llu copied-bytes %llu PIT-consumed %llu/%llu\n",
        x9d.rx_disposition[0], x9d.rx_disposition[1], x9d.rx_disposition[2], x9d.rx_disposition[3],
        x9d.rx_disposition[4], x9d.rx_disposition[5], x9d.rx_wire_bytes, x9d.pit_consumed[0], x9d.pit_consumed[1]);
    for (unsigned i = 0; i < CBP_INTERFACES; i++) {
        IOLockLock(x9d.lock); x9d_route route = x9d.route[i]; x9d_stats s = {};
        x9d_stats_locked(route, s); IOLockUnlock(x9d.lock);
        printf("X9 CELL ROUTE: mux %u bound %u retiring %u channel %u families %u epoch %llu lifetime %llu MTU %u/%u TX %llu/%lluB RX %llu/%lluB drop %llu/%llu\n",
            i + 1, route.bound, route.retiring, route.channel, route.families, route.epoch, route.lifetime,
            route.mtu4, route.mtu6, s.tx_packets, s.tx_bytes, s.rx_packets, s.rx_bytes, s.tx_dropped, s.rx_dropped);
    }
}
} // namespace
// Native producers must carry the epoch observed before dequeue, not borrow
// the current route's epoch after a reconnect.
extern "C" int coachz_modem_data_send_epoch(uint32_t mux, const uint8_t *ip, uint32_t n,uint64_t epoch) { return x9d_send(mux, ip, n,epoch); }
extern "C" int coachz_modem_data_send(uint32_t, const uint8_t *, uint32_t) { return ENETDOWN; }
// Network gate uses the real MIPC-confirmed route, never IPA/QMAP setup.
// Return epoch too: reconnects with the same family must discard old mbufs.
extern "C" uint32_t coachz_modem_data_state(uint32_t mux, uint32_t *mtu, uint64_t *epoch) {
    if (!x9d.lock || !cbp_mux_ok(mux)) return 0;
    IOLockLock(x9d.lock); const x9d_route &r = x9d.route[mux - 1];
    uint32_t families = x9d.ready && !x9d.failed && r.bound ? r.families : 0;
    if (mtu) *mtu = families==3 ? (r.mtu4<r.mtu6?r.mtu4:r.mtu6) : families==1?r.mtu4:families==2?r.mtu6:0;
    if (epoch) *epoch = r.epoch;
    IOLockUnlock(x9d.lock); return families;
}
// Free mailbox slots for a bound route: the ifnet start-model drain dequeues
// only this many, so congestion stays in the stack AQM queue (no local drop
// and no ENOBUFS, which XNU TCP treats as a reason to collapse cwnd).
extern "C" uint32_t coachz_modem_data_room(uint32_t mux) {
    if (!x9d.lock || !cbp_mux_ok(mux)) return 0;
    IOLockLock(x9d.lock); const x9d_route &r = x9d.route[mux - 1];
    uint32_t room = x9d.ready && !x9d.failed && r.bound ? X9D_MAIL - x9d.count : 0;
    IOLockUnlock(x9d.lock); return room;
}
extern "C" void coachz_modem_data_receiver_counted(int (*cb)(uint32_t, const uint8_t *, uint32_t, uint32_t, uint64_t)) {
    // Registration can precede x9d_prepare but is performed by the bridge owner.
    if (x9d.lock) IOLockLock(x9d.lock); x9d.receiver = cb; if (x9d.lock) IOLockUnlock(x9d.lock);
}
