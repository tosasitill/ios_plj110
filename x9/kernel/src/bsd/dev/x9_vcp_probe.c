/* P0 read-only VCP residency + MM-SMMU owner probe (hevc-ios-bridge.md §5.1/§9).
 * x9_vcp_probe=<1..600>: report at init and once more after <N> seconds.
 * x9_vcp_probe_tcm=1 additionally reads VCP SRAM (default off: SRAM access
 * while the tinysys domain state is unproven is the one untested bus class).
 * NEVER writes a register, votes power, issues SMC/HVC, resets or clears a
 * mailbox bit. Every mapping is transient and unmapped after the report.
 * Register classes follow existing proven access: HWV/pwr-ack are AP/AO
 * (x9_mm_power.c, x9_adsp_mt6993.c read them unconditionally); cfg/mbox/SMMU
 * are read only while HWV proves MM_INFRA ON and idle, exactly the camera/
 * mm_power gate; the DRAM carveout is no-map reserved normal memory excluded
 * from the XNU live RAM map by shim fdt_ram.c. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <pexpert/device_tree.h>
#include "x9_vcp_handoff.h"
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *, int);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern void IOSleep(unsigned int);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
LCK_GRP_DECLARE(vcp_probe_group, "x9-vcp-probe");
LCK_MTX_DECLARE(vcp_probe_lock, &vcp_probe_group);
#define MM_INFRA_BITS ((1u << 31) | (1u << 29))
#define MM_SMMU_BACKUP_BIT (1u << 3)
/* Declared payload first 64 bytes, extracted offline from the fixed
 * vcp_b.img original (x9/out/hevc-p0-probe-20261120/firmware-signatures.json;
 * SHA256 gates RV55 f59d646b..., RV33 20f83107..., DRAM 200294bc...). */
static const uint32_t sig_rv55[16] = {
    0x0001a0b5,0,0,0,0,0,0,0,0x50,0,0x0002c0a8,0,0x1ec00000,0x00100000,0,0
};
static const uint32_t sig_rv33[16] = {
    0x0001a0b5,0,0,0,0,0,0,0,0x50,0,0x00047b80,0,0x1ec00000,0x00010000,0,0
};
static const uint32_t sig_dram[16] = {
    0x65999005,0x00203637,0x8593842a,0x06137345,0x04b32c86,0x35b700b5,
    0x45110020,0x0004c683,0x2ac58593,0x7a3420ef,0x0004c503,0x6519cd05,
    0x72d50513,0x45839522,0x45050005,0x35b7e9c1
};
/* Saved LK FDT: mblock-43-me_vcp_reserved reg <0xbd200000 0xe00000> no-map;
 * vcp-dram-size 0xe00000; LK load chain dram_staging_offset 0x200000
 * (vcp_artifacts.py captured LK signed VCP load chain ABI 1). */
#define VCP_DRAM_CARVEOUT 0xbd200000ull
#define VCP_DRAM_STAGING 0x200000u
#define VCP_SRAM_BASE 0x31800000u
#define VCP_PWR_ACK_PA 0x1c00d04cu
#define MM_SMMU_PA 0x30800000u
static uint32_t rd(vm_offset_t map, uint32_t off)
{
    uint32_t v = *(volatile uint32_t *)(map + off);
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}
static uint64_t rd64(vm_offset_t map, uint32_t off)
{
    return (uint64_t)rd(map, off) | (uint64_t)rd(map, off + 4) << 32;
}
static void words16(vm_offset_t map, uint32_t off, uint32_t out[16])
{
    for (unsigned i = 0; i < 16; i++) out[i] = rd(map, off + i * 4);
}
static int match16(const uint32_t a[16], const uint32_t b[16])
{
    for (unsigned i = 0; i < 16; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static void log16(const char *when, const char *what, const uint32_t w[16])
{
    printf("X9 VCP PROBE %s: %s %08x %08x %08x %08x %08x %08x %08x %08x\n",
        when, what, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    printf("X9 VCP PROBE %s: %s+%08x %08x %08x %08x %08x %08x %08x %08x\n",
        when, what, w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
}
static int handoff(struct x9_vcp_handoff *h)
{
    DTEntry chosen; const void *p = NULL; unsigned int bytes = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, "x9-vcp-handoff", &p, &bytes) != kSuccess ||
        !p || bytes != sizeof(*h)) return ENXIO;
    memcpy(h, p, sizeof(*h));
    return x9_vcp_handoff_valid(h) ? 0 : ENXIO;
}
/* cfg+70 idle contract from x9_vcp_bus.c verify_halted (captured
 * vcp_wait_core_stop_timeout): gated bit0, H0 halt bit1, H1 halt bit2
 * (VCP twohart only), AXI busy bit4 must be clear. */
static int core_halted(uint32_t a, uint32_t z, unsigned twohart)
{
    uint32_t need = twohart ? 7u : 3u;
    return (a & (need | 16u)) == need && (z & (need | 16u)) == need;
}
static int core_running(uint32_t a, uint32_t z, unsigned twohart)
{
    uint32_t halt = twohart ? 6u : 2u;
    return !(a & halt) && !(z & halt);
}
static void report(const char *when)
{
    struct x9_vcp_handoff h;
    uint32_t dram_base[16], dram_pay[16], tcm[4][16];
    uint32_t ack[2], sta[2], backup[2], pwr, gpr[2][3][2], halt[2][2];
    uint32_t mbox[5], smmu[15];
    int have_h = handoff(&h) == 0, tcm_ok = 0;
    int dram_match, mm_on, cfg_sane = 0;
    vm_offset_t m;
    lck_mtx_lock(&vcp_probe_lock);
    printf("X9 VCP PROBE %s: read-only; no writes/votes/SMC/reset; handoff=%s tcm_optin=%d\n",
        when, have_h ? "valid" : "ABSENT", sweet_x9_arg("x9_vcp_probe_tcm", 0) == 1);
    /* AP/AO HWV: always accessible (x9_mm_power.c mm_live register class). */
    m = ml_io_map(0x31c00000, 0x22000);
    if (!m) { printf("X9 VCP PROBE %s: HWV map failed\n", when); goto out; }
    ack[0] = rd(m, 0x12900); sta[0] = rd(m, 0x1131c); backup[0] = rd(m, 0x1135c);
    __asm__ volatile("dsb sy" : : : "memory");
    ack[1] = rd(m, 0x12900); sta[1] = rd(m, 0x1131c); backup[1] = rd(m, 0x1135c);
    ml_io_unmap(m, 0x22000);
    mm_on = !((ack[0] ^ ack[1]) & MM_INFRA_BITS) &&
        !((sta[0] | sta[1]) & MM_INFRA_BITS) &&
        (ack[0] & MM_INFRA_BITS) == MM_INFRA_BITS &&
        !((backup[0] | backup[1]) & MM_SMMU_BACKUP_BIT);
    printf("X9 VCP PROBE %s: hwv ack=%x/%x sta=%x/%x backup1=%x/%x -> MM_INFRA %s\n",
        when, ack[0], ack[1], sta[0], sta[1], backup[0], backup[1],
        mm_on ? "ON+idle" : "not proven ON/idle");
    /* vcp-pwr-status from saved LK FDT; raw value only, bit semantics not
     * proven by any captured source, never used as a gate. */
    m = ml_io_map(VCP_PWR_ACK_PA & ~0xfffull, 0x1000);
    if (m) { pwr = rd(m, VCP_PWR_ACK_PA & 0xfff); ml_io_unmap(m, 0x1000);
        printf("X9 VCP PROBE %s: vcp_pwr_ack(%llx)=%x (raw, semantics unproven)\n",
            when, (unsigned long long)VCP_PWR_ACK_PA, pwr);
    } else pwr = 0;
    /* Reserved no-map DRAM: normal memory, safe regardless of MM state. */
    m = ml_io_map(VCP_DRAM_CARVEOUT, 0x1000);
    if (m) { words16(m, 0, dram_base); ml_io_unmap(m, 0x1000); }
    else memset(dram_base, 0, sizeof(dram_base));
    m = ml_io_map(VCP_DRAM_CARVEOUT + VCP_DRAM_STAGING, 0x1000);
    if (m) { words16(m, 0, dram_pay); ml_io_unmap(m, 0x1000); }
    else memset(dram_pay, 0, sizeof(dram_pay));
    dram_match = match16(dram_pay, sig_dram);
    log16(when, "dram@bd200000", dram_base);
    log16(when, "dram@bd400000", dram_pay);
    printf("X9 VCP PROBE %s: dram payload signature %s\n", when,
        dram_match ? "MATCH (RV55_A_dram 200294bc...)" : "MISMATCH/absent");
    memset(tcm, 0, sizeof(tcm));
    memset(mbox, 0, sizeof(mbox));
    memset(smmu, 0, sizeof(smmu));
    if (mm_on && have_h) {
        for (unsigned core = 0; core < 2; core++) {
            m = ml_io_map(h.cfg_base[core], 0x1000);
            if (!m) { halt[core][0] = halt[core][1] = 0xffffffff; continue; }
            halt[core][0] = rd(m, 0x70);
            gpr[core][0][0] = rd(m, 0x44); gpr[core][1][0] = rd(m, 0x48);
            gpr[core][2][0] = rd(m, 0x4c);
            __asm__ volatile("dsb sy" : : : "memory");
            halt[core][1] = rd(m, 0x70);
            gpr[core][0][1] = rd(m, 0x44); gpr[core][1][1] = rd(m, 0x48);
            gpr[core][2][1] = rd(m, 0x4c);
            ml_io_unmap(m, 0x1000);
            printf("X9 VCP PROBE %s: cfg%d halt(+70)=%x/%x gpr1(+44)=%x/%x gpr2(+48)=%x/%x gpr3(+4c)=%x/%x -> %s\n",
                when, core, halt[core][0], halt[core][1],
                gpr[core][0][0], gpr[core][0][1], gpr[core][1][0], gpr[core][1][1],
                gpr[core][2][0], gpr[core][2][1],
                core_halted(halt[core][0], halt[core][1], h.twohart[core]) ? "halted" :
                core_running(halt[core][0], halt[core][1], h.twohart[core]) ? "not halted" :
                "indeterminate");
        }
        cfg_sane = halt[0][0] != 0xffffffff && halt[0][1] != 0xffffffff &&
            halt[1][0] != 0xffffffff && halt[1][1] != 0xffffffff;
        for (unsigned i = 0; i < X9_VCP_MBOX_COUNT; i++) {
            m = ml_io_map(h.mbox[i].base, 0x200);
            if (m) { mbox[i] = rd(m, 0x10c); ml_io_unmap(m, 0x200); }
            else mbox[i] = 0xffffffff;
        }
        printf("X9 VCP PROBE %s: mbox recv(+10c) vcp0=%x vcp1=%x vcp2=%x mmup0=%x mmup1=%x (bit7=READY pin latch, transport only)\n",
            when, mbox[0], mbox[1], mbox[2], mbox[3], mbox[4]);
        /* MM-SMMU 0x30800000 (mt6993.dts mm_smmu; camera precedent maps the
         * same bank). Offsets from x9_mm_smmu_owner.c owner_io_regs. Values
         * are AP-visible: if a pKVM protected handler were installed they
         * could be shadows (NOTES 2026-11-16 EL2 share evidence); equality
         * or init state here is NOT physical-bank or NS-delegation proof. */
        m = ml_io_map(MM_SMMU_PA, 0x20000);
        if (m) {
            smmu[0] = rd(m, 0x00); smmu[1] = rd(m, 0x04); smmu[2] = rd(m, 0x14);
            smmu[3] = rd(m, 0x20); smmu[4] = rd(m, 0x24); smmu[5] = rd(m, 0x28);
            smmu[6] = rd(m, 0x60); smmu[7] = rd(m, 0x64);
            smmu[8] = rd(m, 0x88); smmu[9] = rd(m, 0x98); smmu[10] = rd(m, 0x9c);
            smmu[11] = rd(m, 0xa8); smmu[12] = rd(m, 0xac);
            uint64_t stb = rd64(m, 0x80), cqb = rd64(m, 0x90), eqb = rd64(m, 0xa0);
            ml_io_unmap(m, 0x20000);
            printf("X9 VCP PROBE %s: smmu idr0=%x idr1=%x idr5=%x cr0=%x cr0ack=%x gbpa=%x gerror=%x/%x\n",
                when, smmu[0], smmu[1], smmu[2], smmu[3], smmu[4], smmu[5], smmu[6], smmu[7]);
            printf("X9 VCP PROBE %s: smmu strtab=%llx cfg=%x cmdq=%llx prod=%x cons=%x evtq=%llx prod=%x cons=%x\n",
                when, (unsigned long long)stb, smmu[8], (unsigned long long)cqb,
                smmu[9], smmu[10], (unsigned long long)eqb, smmu[11], smmu[12]);
            printf("X9 VCP PROBE %s: smmu owner evidence: %s (AP-visible only; pKVM shadow possible, not NS delegation)\n",
                when, (smmu[3] & 1) && stb && cqb ?
                "SMMUEN+installed strtab/cmdq: an owner initialized this bank" :
                !(smmu[3] & 1) && !stb && !cqb ?
                "no initialized owner visible (cr0/strtab/cmdq zero)" :
                "partial/ambiguous init state");
        } else printf("X9 VCP PROBE %s: smmu map failed\n", when);
        if (sweet_x9_arg("x9_vcp_probe_tcm", 0) == 1 && cfg_sane) {
            static const uint32_t off[4] = {0, 0x2000, 0x2d000, 0x30000};
            m = ml_io_map(VCP_SRAM_BASE, 0x80000);
            if (m) {
                for (unsigned i = 0; i < 4; i++) words16(m, off[i], tcm[i]);
                ml_io_unmap(m, 0x80000);
                for (unsigned i = 0; i < 4; i++) {
                    char name[24];
                    snprintf(name, sizeof(name), "sram@+%x", off[i]);
                    log16(when, name, tcm[i]);
                    printf("X9 VCP PROBE %s: sram@+%x signature %s\n", when, off[i],
                        match16(tcm[i], sig_rv55) ? "MATCH RV55_A (f59d646b...)" :
                        match16(tcm[i], sig_rv33) ? "MATCH RV33_A (20f83107...)" :
                        "no declared TCM payload");
                }
                tcm_ok = 1;
            } else printf("X9 VCP PROBE %s: sram map failed\n", when);
        } else if (sweet_x9_arg("x9_vcp_probe_tcm", 0) == 1)
            printf("X9 VCP PROBE %s: sram read skipped (cfg samples all-ones)\n", when);
    } else if (mm_on)
        printf("X9 VCP PROBE %s: handoff absent; cfg/mbox/sram reads skipped (topology unqualified)\n", when);
    else
        printf("X9 VCP PROBE %s: MM_INFRA not proven ON; no cfg/mbox/smmu/sram reads (bus-hang guard)\n", when);
    /* Three-state residency verdict (design §5.1). DRAM payload presence is
     * the primary discriminator; TCM is corroboration only when opted in. */
    if (!mm_on || !have_h)
        printf("X9 VCP PROBE %s: residency UNKNOWN (VCP bus not read)\n", when);
    else if (!dram_match)
        printf("X9 VCP PROBE %s: residency state C: DRAM payload absent/mismatch (not loaded, or overwritten)\n", when);
    else if (core_halted(halt[0][0], halt[0][1], h.twohart[0]) &&
             core_halted(halt[1][0], halt[1][1], h.twohart[1]))
        printf("X9 VCP PROBE %s: residency state B: firmware loaded, both cores halted/in-reset%s\n",
            when, tcm_ok ? "" : " (sram not read; opt-in x9_vcp_probe_tcm=1)");
    else if (core_running(halt[0][0], halt[0][1], h.twohart[0]) ||
             core_running(halt[1][0], halt[1][1], h.twohart[1]))
        printf("X9 VCP PROBE %s: residency state A: firmware loaded, core not halted (running; READY IPI unproven without transport)\n", when);
    else
        printf("X9 VCP PROBE %s: residency: DRAM loaded, core state indeterminate (see halt samples)\n", when);
out:
    lck_mtx_unlock(&vcp_probe_lock);
}
static void probe_delayed(void *arg, wait_result_t wr)
{
    (void)wr;
    IOSleep((unsigned)(uintptr_t)arg * 1000);
    report("delayed");
}
void sweet_x9_vcp_probe_init(void)
{
    if (!sweet_early_is_x9()) return;
    int delay = sweet_x9_arg("x9_vcp_probe", 0);
    if (delay < 1 || delay > 600) return;
    report("init");
    thread_t t;
    if (kernel_thread_start(probe_delayed, (void *)(uintptr_t)delay, &t) == KERN_SUCCESS)
        thread_deallocate(t);
    else printf("X9 VCP PROBE: delayed report thread failed\n");
}