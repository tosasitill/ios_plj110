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
constexpr unsigned X9D_DRB = 2048, X9D_BAT = 1024, X9D_PIT = 2048;
constexpr unsigned X9D_TX_SIZE = 3072, X9D_PKT_SIZE = 1664, X9D_FRG_SIZE = 1920;
constexpr unsigned X9D_MAIL = 64, X9D_BUDGET = 128;
constexpr uint64_t X9D_DMA_LIMIT = 1ULL << 40;
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
struct x9d_route { uint64_t epoch; unsigned mtu; uint8_t channel, families; bool bound; x9d_stats stats; };
struct x9d_completion { uint64_t epoch; uint16_t size; uint8_t mux; };
struct x9d_tx { uint64_t epoch; uint16_t size; uint8_t mux; uint8_t ip[X9D_TX_SIZE]; };
struct x9d_rx {
    unsigned rd, bytes; uint8_t seq, channel; bool message, normal, drop;
    uint64_t seq_wait, epoch; unsigned mux;
    uint8_t ip[X9D_TX_SIZE];
};
struct x9d_state {
    IOLock *lock;
    uintptr_t ao, pd, md_misc, infra, sram1;
    x9d_dma drb, txpool, pit[2], bat[2], pool[2];
    bool posted[2][X9D_BAT];
    bool used_channel[20]; // no ambiguous delayed old-bearer RX after channel reuse
    unsigned bat_wr[2], tx_wr, tx_rd, head, count;
    x9d_tx mail[X9D_MAIL]; x9d_route route[CBP_INTERFACES]; x9d_rx rx[2];
    x9d_completion completion[X9D_DRB / 2];
    unsigned mode; bool prepared, ready, failed, published, hw_attempted, clocks_live;
    unsigned irq_reports;
    const char *phase;
    uint32_t last_ul, last_dl; bool legacy_pit_reported;
    uint64_t tx_packets, tx_done, rx_packets, rx_dropped, rejected, flow_events;
    void (*receiver)(uint32_t, const uint8_t *, uint32_t, uint32_t);
};
static x9d_state x9d;
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
static void x9d_fail(const char *reason) {
    if (x9d.failed) return;
    IOLockLock(x9d.lock); x9d.failed = true; x9d.ready = false;
    for (x9d_route &r : x9d.route) { r.bound = false; r.families = 0; r.epoch++; }
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
static void x9d_bat_entry(unsigned kind, unsigned bid) {
    uint64_t pa = x9d.pool[kind].pa + bid * (kind ? X9D_FRG_SIZE : X9D_PKT_SIZE);
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
    x9d_wr(x9d.ao, 0x88, ~0u); x9d_wr(x9d.ao, 0x98, ~0u);
    if (!x9d_expect(x9d.ao, 0x80, 15 | X9D_UL_FLOW | X9D_UL_ERROR, 15 | X9D_UL_FLOW | X9D_UL_ERROR) ||
        !x9d_expect(x9d.ao, 0x90, 0x1fff, 0x1fff)) return false;
    // Clear inherited dummy status before our queues exist; later fatal status
    // is preserved. IP-busy IRQ remains masked for the polled-only candidate.
    x9d_wr(misc, 0, ~0u); x9d_wr(misc, 0x50, ~0u);
    x9d_wr(x9d.ao, 0x9c, ~0u); x9d_wr(misc, 0x60, ~0u);
    x9d_wr(misc, 0x6c, 0xff);
    x9d_wr(dls, 0x00, 64 | (4u << 8) | (1u << 16)); // 64B remain / 1 packet per BAT
    x9d_wr(dls, 0x04, 3080);
    x9d_wr(dls, 0x08, (124u << 24) | (13u << 8)); // PIT threshold / 1664B
    uint32_t cfg = x9d_rd(dls, 0x0c); // RDY_CHK_THRES (not FRG_THRES)
    cfg = (cfg & ~((3u << 22) | (63u << 16) | (1u << 31))) | (1u << 23) | (1u << 13) | (62u << 16);
    x9d_wr(dls, 0x0c, cfg);
    x9d_wr(dls, 0x10, (1u << 28) | (15u << 8) | 62); // fragment BAT, 1920B
    // Initialize both PIT rings, preserving native HPC path mode2 (RX0 only).
    // Capability 2RXQ does not itself prove traffic is steered to both queues.
    // Explicitly disable all 16 aggregation bits.
    // Multiple normal buffers in one packet are an unexpected-LRO fault below,
    // never silently concatenated or reported as a successful received packet.
    x9d_wr(dls, 0x38, 2 | (13u << 4) | (16u << 8));
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
    for (unsigned q = 0; q < 2; q++) {
        x9d_wr(lro, 0x14, (uint32_t)x9d.pit[q].pa);
        // drv3_dl_set_lro_pit_base_addr uses CON1[31:24], including on 6993.
        x9d_wr(lro, 0x18, X9D_PIT | (uint32_t)(x9d.pit[q].pa >> 32) << 24);
        x9d_wr(lro, 0x1c, 0); // reset INIT_CON2; do not inherit queue state
        x9d_wr(lro, 0x20, 1u << 20);
        x9d_wr(lro, 0x24, 0); // Not the high address register for the LRO PIT path.
        uint32_t seq = x9d_rd(dls, 0x40), shift = q ? 20 : 8;
        x9d_wr(dls, 0x40, (seq & ~(255u << shift)) | (255u << shift));
        if (!x9d_expect(lro, 0x14, ~0u, (uint32_t)x9d.pit[q].pa) ||
            !x9d_expect(lro, 0x18, 0xff03ffff, X9D_PIT | (uint32_t)(x9d.pit[q].pa >> 32) << 24) ||
            !x9d_expect(lro, 0x20, 1u << 20, 1u << 20)) return false;
        x9d.published = true; // first queue INIT can expose our DMA; never free it
        if (!x9d_command(lro, 0, X9D_BUSY_BIT | 1 | (q << 16))) return false;
        printf("X9 DPMAIF: PIT%u INIT command complete (DMA reachability not yet proven)\n", q);
    }
    x9d_phase("BAT-init/credit");
    for (unsigned kind = 0; kind < 2; kind++) {
        x9d_wr(dl, 0x08, (uint32_t)x9d.bat[kind].pa);
        x9d_wr(dl, 0x50, (uint32_t)(x9d.bat[kind].pa >> 32)); // 6993 BAT INIT_CON3
        x9d_wr(dl, 0x0c, X9D_BAT | (1u << 16) | (1u << 22));
        x9d_wr(dl, 0x10, 0); // INIT_CON2: initial indices
        if (!x9d_expect(dl, 0x08, ~0u, (uint32_t)x9d.bat[kind].pa) ||
            !x9d_expect(dl, 0x50, ~0u, (uint32_t)(x9d.bat[kind].pa >> 32)) ||
            !x9d_expect(dl, 0x0c, 0x0041ffff, X9D_BAT | (1u << 16) | (1u << 22))) return false;
        if (!x9d_command(dl, 0, X9D_BUSY_BIT | 1 | (kind << 16))) return false;
        if (!x9d_command(dl, 4, X9D_BUSY_BIT | (X9D_BAT - 1) | (kind << 16))) return false;
    }
    x9d_phase("TXQ0/SRAM-sync");
    // One TX queue initially, others explicitly disabled. DRB size is WORDS.
    x9d_wr(x9d.ao, 0x1c, 0);
    x9d_wr(uls, 0x10, (uint32_t)x9d.drb.pa); x9d_wr(uls, 0x14, X9D_DRB * 2);
    x9d_wr(uls, 0x18, (uint32_t)(x9d.drb.pa >> 32)); // 6993 UL_DRBSIZE_ADDRH_n_1
    x9d_wr(x9d.ao, 0x1c, 1 | (1u << 8));
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
        !x9d_expect(x9d.ao, 0x1c, 0x1f1f, 0x101) ||
        !x9d_expect(uls, 0xe0, ~0u, 0) ||
        !x9d_expect(dls, 0xa4, 0x3ffff, 0) || !x9d_expect(dls, 0xc4, 0x3ffff, 0) ||
        !x9d_expect(dls, 0xd8, 65535, 0) || !x9d_expect(dls, 0x78, 65535, 0) ||
        !x9d_expect(dls, 0xdc, 65535, X9D_BAT - 1) ||
        !x9d_expect(x9d.sram1, 0x04, 65535, 0)) return false;
    x9d_phase("INIT_DONE");
    x9d_wr(x9d.ao, 0, 1); x9d_wr(x9d.ao + 0x400, 0, 1);
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
    for (unsigned kind = 0; kind < 2; kind++) {
        if (!x9d_alloc(x9d.pit[kind], X9D_PIT * 16) || !x9d_alloc(x9d.bat[kind], X9D_BAT * 8) ||
            !x9d_alloc(x9d.pool[kind], X9D_BAT * (kind ? X9D_FRG_SIZE : X9D_PKT_SIZE))) {
            x9d_fail("private RX DMA allocation/range"); return;
        }
        for (unsigned bid = 0; bid < X9D_BAT; bid++) {
            x9d_bat_entry(kind, bid); x9d.posted[kind][bid] = bid != X9D_BAT - 1;
        }
        x9d.bat_wr[kind] = X9D_BAT - 1;
        x9d_sync(x9d.bat[kind], 0, X9D_BAT * 8);
        // Each slot must differ from ITS expected first-lap sequence, including
        // slots 255/511/etc. A fixed 0xff poison would match those descriptors.
        uint32_t *pit = reinterpret_cast<uint32_t *>(x9d.pit[kind].va);
        for (unsigned i = 0; i < X9D_PIT; i++) pit[i * 4 + 3] = (i ^ 128u) & 255u;
        x9d_sync(x9d.pit[kind], 0, X9D_PIT * 16);
    }
    printf("X9 DPMAIF: private 40-bit DMA allocated, DRB high 0x%x PIT high 0x%x/0x%x BAT high 0x%x/0x%x, TX 2048/BAT 1024/PIT 2048x2; %s\n",
        (unsigned)(x9d.drb.pa >> 32), (unsigned)(x9d.pit[0].pa >> 32), (unsigned)(x9d.pit[1].pa >> 32),
        (unsigned)(x9d.bat[0].pa >> 32), (unsigned)(x9d.bat[1].pa >> 32),
        x9d.mode == 1 ? "probe only, no descriptors published" : "experimental initialization requested");
    // Physical HS2 can precede the serialized AT initializer's CFUN completion.
    // Active initialization is deferred until the MD owner is actually online.
}
static bool x9d_ready() { return x9d.ready && !x9d.failed; } // owner only
// Called only AFTER native MIPC has proved call ID -> interface/ccmni binding.
// Neither mux nor QMI client id is a physical ccmni channel.
static bool x9d_bind(unsigned mux, unsigned channel, unsigned families, unsigned mtu) {
    if (!x9d_ready() || !cbp_mux_ok(mux) || channel >= 20 || !families || (families & ~3u) || mtu < 68 || mtu > X9D_TX_SIZE) return false;
    IOLockLock(x9d.lock);
    if (x9d.used_channel[channel]) { IOLockUnlock(x9d.lock); return false; }
    for (unsigned i = 0; i < CBP_INTERFACES; i++) if (i != mux - 1 && x9d.route[i].bound && x9d.route[i].channel == channel) {
        IOLockUnlock(x9d.lock); return false;
    }
    x9d_route &r = x9d.route[mux - 1]; r.epoch++; r.channel = (uint8_t)channel; r.families = (uint8_t)families; r.mtu = mtu; r.bound = true;
    bzero(&r.stats, sizeof(r.stats));
    x9d.used_channel[channel] = true;
    IOLockUnlock(x9d.lock); return true;
}
static void x9d_unbind(unsigned mux) {
    if (!x9d.lock || !cbp_mux_ok(mux)) return;
    IOLockLock(x9d.lock); x9d_route &r = x9d.route[mux - 1];
    if (r.bound) { r.bound = false; r.families = 0; r.epoch++; }
    IOLockUnlock(x9d.lock);
}
static bool x9d_account(unsigned mux, x9d_stats &stats) {
    if (!x9d.lock || !cbp_mux_ok(mux)) return false;
    IOLockLock(x9d.lock); stats = x9d.route[mux - 1].stats; IOLockUnlock(x9d.lock);
    return true;
}
static int x9d_send(unsigned mux, const uint8_t *ip, unsigned n) {
    if (!x9d.lock || !cbp_mux_ok(mux)) return ENETDOWN;
    if (!x9d_ip(ip, n)) return EINVAL;
    IOLockLock(x9d.lock); x9d_route &r = x9d.route[mux - 1];
    if (!x9d.ready || x9d.failed || !r.bound || !(r.families & cbp_family_bit(ip[0] >> 4)) || n > r.mtu) {
        x9d.rejected++; r.stats.tx_dropped++; IOLockUnlock(x9d.lock); return n > r.mtu ? EMSGSIZE : ENETDOWN;
    }
    // Backpressure is retryable; don't count a retained packet as dropped.
    if (x9d.count == X9D_MAIL) { IOLockUnlock(x9d.lock); return EBUSY; }
    x9d_tx &t = x9d.mail[(x9d.head + x9d.count) % X9D_MAIL];
    t.epoch = r.epoch; t.size = (uint16_t)n; t.mux = (uint8_t)mux; memcpy(t.ip, ip, n); x9d.count++;
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
        if (r.bound && r.epoch == c.epoch) { r.stats.tx_packets++; r.stats.tx_bytes += c.size; }
        IOLockUnlock(x9d.lock); bzero(&c, sizeof(c)); x9d.tx_done++;
    }
    x9d.tx_rd = rd;
    for (unsigned budget = 0; budget < 16; budget++) {
        unsigned free = X9D_DRB - 1 - (x9d.tx_wr + X9D_DRB - x9d.tx_rd) % X9D_DRB;
        if (free < 2) break;
        static x9d_tx t; uint8_t channel;
        IOLockLock(x9d.lock);
        if (!x9d.count) { IOLockUnlock(x9d.lock); break; }
        t = x9d.mail[x9d.head]; x9d.head = (x9d.head + 1) % X9D_MAIL; x9d.count--;
        const x9d_route &r = x9d.route[t.mux - 1];
        bool valid = r.bound && r.epoch == t.epoch && (r.families & cbp_family_bit(t.ip[0] >> 4)); channel = r.channel;
        IOLockUnlock(x9d.lock); if (!valid) { bzero(&t, sizeof(t)); continue; }
        unsigned index = x9d.tx_wr, pool = index / 2 * X9D_TX_SIZE;
        memcpy(x9d.txpool.va + pool, t.ip, t.size); x9d_sync(x9d.txpool, pool, t.size);
        uint64_t pa = x9d.txpool.pa + pool;
        uint32_t *d = reinterpret_cast<uint32_t *>(x9d.drb.va + index * 8);
        d[0] = 1 | 4 | ((uint32_t)t.size << 16); d[1] = (uint32_t)channel << 16; // MSG dtype=1/cbit=1
        d[2] = ((uint32_t)(pa >> 32) << 8) | ((uint32_t)t.size << 16); d[3] = (uint32_t)pa; // PD dtype=0/cbit=0
        x9d_sync(x9d.drb, index * 8, 16);
        x9d.completion[index / 2] = {t.epoch, t.size, t.mux};
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
        r.mux = 0; r.epoch = 0;
        IOLockLock(x9d.lock);
        for (unsigned i = 0; i < CBP_INTERFACES; i++) if (x9d.route[i].bound && x9d.route[i].channel == r.channel) {
            r.mux = i + 1; r.epoch = x9d.route[i].epoch;
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
    unsigned stride = kind ? X9D_FRG_SIZE : X9D_PKT_SIZE, off = bid * stride;
    uint64_t addr = d[1] | ((uint64_t)d[2] << 32), base = x9d.pool[kind].pa + off;
    // Buffer address is observed in PIT but vendor treats BAT as authoritative.
    // Reject out-of-buffer addresses; do not follow arbitrary physical pointers.
    if (!len || len > stride || hoff > stride - len || addr < base || addr >= base + stride) {
        x9d_fail("RX DMA address/length"); return false;
    }
    if (kind && hoff) { x9d_fail("fragment header offset"); return false; }
    unsigned bytes = len + (kind ? 0 : hoff);
    if (bytes > sizeof(r.ip) - r.bytes) { x9d_fail("RX packet exceeds bounded MTU"); return false; }
    x9d_sync(x9d.pool[kind], off, stride);
    memcpy(r.ip + r.bytes, x9d.pool[kind].va + off, bytes); r.bytes += bytes; r.normal = true;
    x9d.posted[kind][bid] = false; // CPU copy complete; global BAT replenisher owns reuse
    if (!(a & 2)) {
        unsigned mux = 0; void (*receiver)(uint32_t, const uint8_t *, uint32_t, uint32_t);
        IOLockLock(x9d.lock);
        for (unsigned i = 0; i < CBP_INTERFACES; i++) if (x9d.route[i].bound && x9d.route[i].channel == r.channel &&
            i + 1 == r.mux && x9d.route[i].epoch == r.epoch && r.bytes <= x9d.route[i].mtu &&
            (x9d.route[i].families & cbp_family_bit(r.ip[0] >> 4))) mux = i + 1;
        receiver = x9d.receiver; IOLockUnlock(x9d.lock);
        bool valid = !r.drop && mux && receiver && x9d_ip(r.ip, r.bytes);
        IOLockLock(x9d.lock);
        if (cbp_mux_ok(r.mux) && x9d.route[r.mux - 1].epoch == r.epoch) {
            x9d_stats &s = x9d.route[r.mux - 1].stats;
            if (valid) { s.rx_packets++; s.rx_bytes += r.bytes; } else s.rx_dropped++;
        }
        IOLockUnlock(x9d.lock);
        if (valid) { receiver(mux, r.ip, r.bytes, r.ip[0] >> 4); x9d.rx_packets++; }
        else x9d.rx_dropped++;
        bzero(r.ip, r.bytes); r.bytes = 0; r.message = r.normal = false;
    }
    return true;
}
static bool x9d_rx_poll(unsigned q) {
    x9d_rx &r = x9d.rx[q]; unsigned wr = x9d_rd(x9d.pd + 0xc00, q ? 0xc4 : 0xa4) & 0x3ffff;
    if (wr >= X9D_PIT) { x9d_fail("RX hardware write index"); return false; }
    unsigned consumed = 0, released = 0;
    while (r.rd != wr && consumed < X9D_BUDGET) {
        x9d_sync(x9d.pit[q], r.rd * 16, 16);
        uint32_t item[4]; memcpy(item, x9d.pit[q].va + r.rd * 16, sizeof(item));
        if ((item[3] & 255) != r.seq) {
            // Producer index can become visible before DMA data. Bounded defer,
            // not a busy loop and not acceptance of a stale/poison descriptor.
            if (!r.seq_wait) r.seq_wait = x9d_ms();
            else if (x9d_ms() - r.seq_wait >= 20) x9d_fail("RX sequence deadline");
            break;
        }
        r.seq_wait = 0;
        if (!x9d_rx_item(q, item)) return false;
        r.rd = (r.rd + 1) % X9D_PIT; r.seq++; consumed++;
        if (!(r.rd & 3)) {
            // Eight-bit sequence wraps eight times in a 2048-entry PIT ring:
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
    return !released || x9d_command(x9d.pd + 0x900, 0x10, X9D_BUSY_BIT | (q << 20) | released);
}
static bool x9d_replenish(unsigned kind) {
    const uintptr_t dls = x9d.pd + 0xc00;
    unsigned rd = x9d_rd(dls, kind ? 0x78 : 0xd8) & 65535;
    if (rd >= X9D_BAT) { x9d_fail("BAT hardware read index"); return false; }
    unsigned n = 0, w = x9d.bat_wr[kind], stride = kind ? X9D_FRG_SIZE : X9D_PKT_SIZE;
    // Keep one entry empty; require BOTH hardware consumption and CPU release.
    // A BID held by either RX queue stops the contiguous refill, never overwrites.
    while ((w + 1) % X9D_BAT != rd && n < X9D_BUDGET && !x9d.posted[kind][w] && !x9d.posted[kind][(w + 1) % X9D_BAT]) {
        x9d_sync(x9d.pool[kind], w * stride, stride); x9d_bat_entry(kind, w);
        x9d_sync(x9d.bat[kind], w * 8, 8); x9d.posted[kind][w] = true;
        w = (w + 1) % X9D_BAT; n++;
    }
    if (!n) return true;
    x9d.bat_wr[kind] = w;
    return x9d_command(x9d.pd + 0x100, 4, X9D_BUSY_BIT | (kind << 16) | n);
}
static void x9d_poll() {
    if (x9d.mode == 2 && !x9d.hw_attempted && !x9d.failed && x9_md_at_online()) {
        x9d.hw_attempted = true; (void)x9d_hw_init();
    }
    if (!x9d_ready()) return;
    if (!x9_md_at_online()) { x9d_fail("MD/AT offline"); return; }
    if (x9d_rd(x9d.infra, 0xd20) & 12) { x9d_fail("clock gated while active"); return; }
    uint32_t ul = x9d_rd(x9d.pd + 0x400, 0), dl = x9d_rd(x9d.pd + 0x400, 0x50);
    uint32_t checked_dl = dl;
    if ((dl & X9D_DL_LEGACY_PIT) &&
        (x9d_rd(x9d.ao, 0x90) & X9D_DL_LEGACY_PIT) &&
        (x9d_rd(x9d.pd + 0xc00, 0x38) & 3u) == 2u) {
        // 175.txt: raw 0x8 with both LRO rings untouched, legacy PIT absent.
        // A polled owner cannot filter by the entire interrupt mask (all IRQs
        // are masked here). Exempt ONLY this inactive legacy source, and only
        // while its mask and path2 selection still match our configuration.
        checked_dl &= ~X9D_DL_LEGACY_PIT;
        if (!x9d.legacy_pit_reported) {
            x9d.legacy_pit_reported = true;
            printf("X9 DPMAIF: masked legacy PIT count status 0x8 in LRO path2; LRO/DMA errors remain fatal\n");
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
    if (!x9d_tx_poll() || !x9d_rx_poll(0) || !x9d_rx_poll(1) || !x9d_replenish(0) || !x9d_replenish(1)) return;
    if (ul) x9d_wr(x9d.pd + 0x400, 0, ul);
    if (dl) x9d_wr(x9d.pd + 0x400, 0x50, dl);
}
static void x9d_report(unsigned second) {
    if (!x9d.lock || !x9d.mode) return;
    IOLockLock(x9d.lock); uint64_t rejected = x9d.rejected; unsigned queued = x9d.count; IOLockUnlock(x9d.lock);
    printf("X9 DPMAIF: %u s mode %u ready %u quarantined %u published %u TX submitted %llu completed %llu RX delivered %llu dropped %llu queued %u rejected %llu flow %llu phase %s\n",
        second, x9d.mode, x9d.ready, x9d.failed, x9d.published, x9d.tx_packets, x9d.tx_done, x9d.rx_packets, x9d.rx_dropped, queued, rejected, x9d.flow_events,
        x9d.phase ? x9d.phase : "prepare");
}
} // namespace
extern "C" int coachz_modem_data_send(uint32_t mux, const uint8_t *ip, uint32_t n) { return x9d_send(mux, ip, n); }
// Network gate uses the real MIPC-confirmed route, never IPA/QMAP setup.
// Return epoch too: reconnects with the same family must discard old mbufs.
extern "C" uint32_t coachz_modem_data_state(uint32_t mux, uint32_t *mtu, uint64_t *epoch) {
    if (!x9d.lock || !cbp_mux_ok(mux)) return 0;
    IOLockLock(x9d.lock); const x9d_route &r = x9d.route[mux - 1];
    uint32_t families = x9d.ready && !x9d.failed && r.bound ? r.families : 0;
    if (mtu) *mtu = families ? r.mtu : 0;
    if (epoch) *epoch = r.epoch;
    IOLockUnlock(x9d.lock); return families;
}
extern "C" void coachz_modem_data_receiver(void (*cb)(uint32_t, const uint8_t *, uint32_t, uint32_t)) {
    // Registration can precede x9d_prepare but is performed by the bridge owner.
    if (x9d.lock) IOLockLock(x9d.lock); x9d.receiver = cb; if (x9d.lock) IOLockUnlock(x9d.lock);
}
