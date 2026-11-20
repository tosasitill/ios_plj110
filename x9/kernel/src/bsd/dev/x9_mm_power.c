/* Shared MT6993 MMINFRA reference authority.
 * Live DT mminfra-type0-pwr=MM_2, voter55/bit3; mminfra_ctrl() and
 * mtk_smi_mt6993_disp_resource_ctrl() both use this MM_2 reference.
 * hwccf_api.c v1: MMUP dummy read, STA idle, AP SET/CLR + EN readback,
 * global EN on take, STA idle on both take/drop (DONE is NOT polled).
 * The old smmu_secure PM_GET SMC c2000514/803 is unsupported in log350.
 * Only our added AP BACKUP1 bit3 is removed; inherited AP/other-XPU votes
 * remain intact. This powers MM; it grants no SMMU table/queue ownership. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <mach/clock_types.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <kern/cpu_data.h>
#include "x9_mm_power.h"
extern int sweet_early_is_x9(void);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t, addr64_t);
LCK_GRP_DECLARE(mm_power_group, "x9-mm-power");
LCK_MTX_DECLARE(mm_power_lock, &mm_power_group);
#define MM_POWER_PARENTS 32u
#define MM_POWER_CHILDREN 256u
#define MM_INFRA_BITS ((1u << 31) | (1u << 29))
#define MM_POWER_BIT (1u << 3)
#define MM_POWER_SET 0x20724u
#define MM_POWER_CLR 0x20728u
#define MM_POWER_EN 0x2072cu
#define MM_POWER_GLOBAL 0x11358u
#define MM_POWER_STA 0x1135cu
#define MM_POWER_DONE 0x1136cu
#define MM_POWER_ALL_STA 0x13704u
#define MM_POWER_FSM 0x10000u
#define MM_POWER_REG_EN6 0x152cu
#define MM_POWER_INT_STATUS 0x50u
#define MM_POWER_INT_BACKUP1_TIMEOUT (1u << 5)
#define MM_POWER_VOTE_IRQ_HIS_POINTER 0x124ccu
#define MM_POWER_VOTE_IRQ_HIS_ADDR 0x124d0u
#define MM_POWER_VOTE_IRQ_HIS_DATA 0x12508u
#define MM_POWER_VOTE_IRQ_HIS_SLOTS 14u
#define MM_POWER_ACK 0xa8u
#define MM_POWER_ACK_MASK 0xc0000000u
#define MM_IRQ_PREPARE_TIMEOUT_US 10000u
#define MM_IRQ_VOTE_TIMEOUT_US 1250u
#define MM_IRQ_DONE_TIMEOUT_US 10000u
#define MM_IRQ_GLOBAL_MAX_RETRIES 10000u

/* Android rv_v2 vcp_helper.c + mmup.c + vcp_logger.c boot parameters. */
#define VCP_SIP_CONTROL 0xc200052cu
#define VCP_OP_RESET_SET 2u
#define VCP_OP_RESET_RELEASE 3u
#define MMUP_OP_RESET_SET 5u
#define MMUP_OP_RESET_RELEASE 6u
#define VCP_DRAM_RESV_ADDR_OFF 0x44u
#define VCP_DRAM_RESV_SIZE_OFF 0x48u
#define VCP_DRAM_RESV_LOGGER_OFF 0x4cu
#define VCP_CORE_STATUS_OFF 0x70u
#define VCP_CORE_MON_PC_OFF 0x80u
#define VCP_CORE_RSTN_CLR_OFF 0x00u
#define VCP_MBOX_READY_MSG_OFF 0x38u /* offset 14 * MBOX_SLOT_SIZE(4) */
#define VCP_MBOX_CLR_OFF 0x10cu
#define VCP_MBOX_READY_BIT (1u << 7) /* pin_index 7 for IPI_IN_VCP_READY_0/1 */
#define VCP_READY_TIMEOUT_MS 3000u
#define VCP_MEM_TOTAL_SIZE 0x20e000u
#define VCP_MEM_ALLOC_BYTES ((VCP_MEM_TOTAL_SIZE + PAGE_MASK) & ~(vm_size_t)PAGE_MASK)
#define VCP_MEM_LOGGER_OFFSET 0x08d000u
#define VCP_MEM_LOGGER_SIZE 0x180000u
#define VCP_MEM_WIN_BASE 0x140000000ULL /* mm_hfrp / MMUP & VCP remap window [0x140000000, 0x1c0000000) */
#define VCP_MEM_WIN_END  0x1c0000000ULL
#define VCP_MEM_REMAP_BLOCK 0x08000000ULL /* 128 MiB remap granularity */
#define VCP_ALLOC_HELD_MAX 32u
#define VCP_PLT_LOG_ENABLE 0x504C5402u
#define VCP_DRAM_LOG_BUF_LEN 0x100000u
#define VCP_PACK_IOVA(addr) ((uint32_t)(((addr) & 0xffffffffull) | (((addr) >> 32) & 0xfull)))

struct vcp_log_ctrl {
    uint32_t base, size, enable, info_ofs, buff_ofs, buff_size;
};
struct vcp_log_buf_info {
    uint32_t r_pos, w_pos;
};

static struct { uint64_t token; uint32_t client, children; } parents[MM_POWER_PARENTS];
static struct { uint64_t token, parent; } children[MM_POWER_CHILDREN];
static uint64_t next_token = 1;
static vm_offset_t hwv, mmup, power_ack, vcp_cfg_core0, vcp_cfg_core1, vcp_mbox2, mmup_mbox4;
static vm_offset_t vcp_diag_pwr, vcp_diag_clk, vcp_diag_slp, vcp_diag_sec, vcp_diag_ap, vcp_diag_sram;
static vm_offset_t vcp_mem_va;
static uint64_t vcp_mem_pa;
static bool power_fault, vote_owned, vote_held, vcp_booted, vcp_fault;
static uint32_t holders;
struct mm_irq_timeout {
    unsigned offset;
    uint32_t value;
};

static uint32_t mm_read(vm_offset_t base, unsigned off)
{
    uint32_t value = *(volatile uint32_t *)(base + off);
    __asm__ volatile("dsb sy" : : : "memory");
    return value;
}
static void mm_write_reg(vm_offset_t base, unsigned off, uint32_t value)
{
    *(volatile uint32_t *)(base + off) = value;
    __asm__ volatile("dsb sy" : : : "memory");
}
static void mm_write(unsigned off, uint32_t value)
{
    mm_write_reg(hwv, off, value);
}
static int mm_wait(vm_offset_t base, unsigned off, uint32_t mask,
                   uint32_t target, unsigned count, unsigned step_us)
{
    for (unsigned n = 0; n <= count; n++) {
        if ((mm_read(base, off) & mask) == target) return 0;
        if (n != count) delay(step_us);
    }
    return ETIMEDOUT;
}
static int vcp_cache_clean_invalidate(vm_offset_t va, vm_size_t bytes)
{
    uint64_t ctr;
    vm_offset_t line, end;
    disable_preemption();
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    line = (vm_offset_t)4 << ((ctr >> 16) & 15u);
    if (!line || line > PAGE_SIZE || ((va | bytes) & (line - 1))) {
        enable_preemption();
        return EINVAL;
    }
    end = va + bytes;
    __asm__ volatile("dsb sy" : : : "memory");
    for (vm_offset_t p = va; p < end; p += line)
        __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
    enable_preemption();
    return 0;
}
static uint64_t vcp_smc(uint32_t op, uint32_t val)
{
    register uint64_t x0 __asm__("x0") = VCP_SIP_CONTROL;
    register uint64_t x1 __asm__("x1") = op;
    register uint64_t x2 __asm__("x2") = val;
    register uint64_t x3 __asm__("x3") = 0;
    register uint64_t x4 __asm__("x4") = 0;
    register uint64_t x5 __asm__("x5") = 0;
    register uint64_t x6 __asm__("x6") = 0;
    register uint64_t x7 __asm__("x7") = 0;
    __asm__ volatile("dsb sy\nsmc #0\ndsb sy\nisb"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
          "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
        : : "memory", "cc", "x8", "x9", "x10", "x11",
            "x12", "x13", "x14", "x15", "x16", "x17");
    return x0;
}
static int vcp_smc_ok(uint32_t op, uint32_t val, uint64_t *raw)
{
    uint64_t r = vcp_smc(op, val);
    *raw = r;
    if (!r) return 0;
    return ((int64_t)r == -1 || r == 0xffffffffULL) ? ENOTSUP : EIO;
}
static int vcp_poll_ready(vm_offset_t mbox, uint32_t *img_size, uint32_t *clr_seen)
{
    uint64_t deadline;
    clock_interval_to_deadline(VCP_READY_TIMEOUT_MS, NSEC_PER_MSEC, &deadline);
    for (;;) {
        uint32_t clr = mm_read(mbox, VCP_MBOX_CLR_OFF);
        if (clr & VCP_MBOX_READY_BIT) {
            *clr_seen = clr;
            *img_size = mm_read(mbox, VCP_MBOX_READY_MSG_OFF);
            mm_write_reg(mbox, VCP_MBOX_CLR_OFF, VCP_MBOX_READY_BIT);
            return 0;
        }
        if (mach_absolute_time() >= deadline) {
            *clr_seen = mm_read(mbox, VCP_MBOX_CLR_OFF);
            return ETIMEDOUT;
        }
        delay_for_interval(100, NSEC_PER_USEC);
    }
}
static int vcp_check_contig_pa(vm_offset_t va, uint64_t *base_pa_out)
{
    uint64_t base_pa = 0;
    for (vm_size_t off = 0; off < VCP_MEM_ALLOC_BYTES; off += PAGE_SIZE) {
        ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)(va + off));
        uint64_t pa = ptoa_64(pn);
        if (!pn || (pa & PAGE_MASK) || pa >= (1ULL << 36)) return EFAULT;
        if (!off) base_pa = pa;
        else if (pa != base_pa + off) return EFAULT;
    }
    *base_pa_out = base_pa;
    return 0;
}
static bool vcp_pa_in_remap_window(uint64_t pa)
{
    uint64_t last = pa + VCP_MEM_TOTAL_SIZE - 1ULL;
    if (pa < VCP_MEM_WIN_BASE || pa + VCP_MEM_ALLOC_BYTES > VCP_MEM_WIN_END)
        return false;
    return (pa & ~(VCP_MEM_REMAP_BLOCK - 1ULL)) == (last & ~(VCP_MEM_REMAP_BLOCK - 1ULL));
}
static int vcp_alloc_reserved(void)
{
    static const uint64_t aligns[] = {
        0x40000000ULL, /* 1 GiB: lands directly on 0x140000000 / 0x180000000 */
        0x20000000ULL, /* 512 MiB */
        0x10000000ULL, /* 256 MiB */
        0x08000000ULL, /* 128 MiB */
        0x04000000ULL, /* 64 MiB */
        0x02000000ULL, /* 32 MiB */
        0x01000000ULL, /* 16 MiB */
    };
    vm_offset_t held[VCP_ALLOC_HELD_MAX];
    unsigned held_cnt = 0;
    vm_offset_t va = 0;
    ppnum_t maximum = (ppnum_t)((VCP_MEM_WIN_END - 1ULL) >> PAGE_SHIFT);
    uint64_t base_pa = 0;
    int e = 0;
    if (vcp_mem_va) return 0;
    for (unsigned a = 0; a < sizeof(aligns) / sizeof(aligns[0]) && !va; a++) {
        ppnum_t pnum_mask = (ppnum_t)((aligns[a] - 1ULL) >> PAGE_SHIFT);
        while (held_cnt < VCP_ALLOC_HELD_MAX) {
            vm_offset_t cand = 0;
            uint64_t cand_pa = 0;
            if (kmem_alloc_contig(kernel_map, &cand, VCP_MEM_ALLOC_BYTES, PAGE_MASK,
                    maximum, pnum_mask, KMA_DATA | KMA_ZERO,
                    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
                break;
            e = vcp_check_contig_pa(cand, &cand_pa);
            if (!e && vcp_pa_in_remap_window(cand_pa)) {
                va = cand;
                base_pa = cand_pa;
                break;
            }
            held[held_cnt++] = cand;
            if (e) break;
        }
        if (e) break;
    }
    for (unsigned i = 0; i < held_cnt; i++)
        kmem_free(kernel_map, held[i], VCP_MEM_ALLOC_BYTES);
    if (!va) return e ? e : ENOMEM;
    {
        struct vcp_log_ctrl *ctl = (struct vcp_log_ctrl *)(va + VCP_MEM_LOGGER_OFFSET);
        uint32_t info_ofs = sizeof(*ctl);
        uint32_t buff_ofs = (info_ofs + sizeof(struct vcp_log_buf_info) + 3u) & ~3u;
        ctl->base = VCP_PLT_LOG_ENABLE;
        ctl->size = sizeof(*ctl);
        ctl->enable = 0;
        ctl->info_ofs = info_ofs;
        ctl->buff_ofs = buff_ofs;
        ctl->buff_size = VCP_DRAM_LOG_BUF_LEN;
        e = vcp_cache_clean_invalidate(va, VCP_MEM_ALLOC_BYTES);
    }
    if (e) {
        kmem_free(kernel_map, va, VCP_MEM_ALLOC_BYTES);
        return e;
    }
    vcp_mem_va = va;
    vcp_mem_pa = base_pa;
    return 0;
}
static void vcp_boot_diag_log(void)
{
    if (!vcp_diag_pwr) vcp_diag_pwr = ml_io_map(0x1c00d000, 0x1000);
    if (!vcp_diag_clk) vcp_diag_clk = ml_io_map(0x10040000, 0x1000);
    if (!vcp_diag_slp) vcp_diag_slp = ml_io_map(0x31aa0000, 0x1000);
    if (!vcp_diag_sec) vcp_diag_sec = ml_io_map(0x31a70000, 0x1000);
    if (!vcp_diag_ap)  vcp_diag_ap  = ml_io_map(0x31bd0000, 0x1000);
    if (!vcp_diag_sram) vcp_diag_sram = ml_io_map(0x31800000, 0x80000);
    printf("X9 MM VCP DIAG1: pwr_ack=%x pdn=%x fenc=%x slp=%x/%x sys=%x sec=%x/%x wdt=%x/%x\n",
        vcp_diag_pwr ? mm_read(vcp_diag_pwr, 0x4c) : 0xdeadbeefu,
        vcp_diag_clk ? mm_read(vcp_diag_clk, 0x10) : 0xdeadbeefu,
        vcp_diag_clk ? mm_read(vcp_diag_clk, 0x270) : 0xdeadbeefu,
        vcp_diag_slp ? mm_read(vcp_diag_slp, 0x00) : 0xdeadbeefu,
        vcp_diag_slp ? mm_read(vcp_diag_slp, 0x68) : 0xdeadbeefu,
        mm_read(mmup, 0x00),
        vcp_diag_sec ? mm_read(vcp_diag_sec, 0x48) : 0xdeadbeefu,
        vcp_diag_sec ? mm_read(vcp_diag_sec, 0x4c) : 0xdeadbeefu,
        vcp_diag_ap  ? mm_read(vcp_diag_ap, 0x50) : 0xdeadbeefu,
        vcp_diag_ap  ? mm_read(vcp_diag_ap, 0x54) : 0xdeadbeefu);
    printf("X9 MM VCP DIAG2: c1_latch=%x lr=%x sp=%x gpr5=%x c0_latch=%x gpr5=%x sram=%x/%x exc=%x/%x/%x\n",
        mm_read(vcp_cfg_core1, 0xd0), mm_read(vcp_cfg_core1, 0x84),
        mm_read(vcp_cfg_core1, 0x88), mm_read(vcp_cfg_core1, 0x54),
        mm_read(vcp_cfg_core0, 0xd0), mm_read(vcp_cfg_core0, 0x54),
        vcp_diag_sram ? mm_read(vcp_diag_sram, 0x00000) : 0xdeadbeefu,
        vcp_diag_sram ? mm_read(vcp_diag_sram, 0x37000) : 0xdeadbeefu,
        vcp_diag_sram ? mm_read(vcp_diag_sram, 0x37000 + 0x29b0c) : 0xdeadbeefu,
        vcp_diag_sram ? mm_read(vcp_diag_sram, 0x37000 + 0x29b14) : 0xdeadbeefu,
        vcp_diag_sram ? mm_read(vcp_diag_sram, 0x37000 + 0x29b1c) : 0xdeadbeefu);
}
static int vcp_boot_ensure(void)
{
    int e;
    uint32_t init_c0, init_c1;
    uint64_t smc_s0 = 0, smc_s1 = 0, smc_r1 = 0, smc_r0 = 0;
    uint32_t mmup_sz = 0, mmup_clr = 0, vcp_sz = 0, vcp_clr = 0;
    unsigned stage = 1;
    if (vcp_booted) return 0;
    if (vcp_fault) return EIO;
    e = vcp_alloc_reserved();
    if (e) return e;
    init_c0 = mm_read(vcp_cfg_core0, VCP_CORE_STATUS_OFF);
    init_c1 = mm_read(vcp_cfg_core1, VCP_CORE_STATUS_OFF);
    e = vcp_smc_ok(VCP_OP_RESET_SET, 1, &smc_s0);
    if (!e) { stage = 2; e = vcp_smc_ok(MMUP_OP_RESET_SET, 1, &smc_s1); }
    if (!e) {
        stage = 3;
        vcp_fault = true; /* Never free or re-release after first release attempt */
        mm_write_reg(vcp_cfg_core1, VCP_DRAM_RESV_ADDR_OFF, VCP_PACK_IOVA(vcp_mem_pa));
        mm_write_reg(vcp_cfg_core1, VCP_DRAM_RESV_SIZE_OFF, VCP_MEM_TOTAL_SIZE);
        mm_write_reg(vcp_cfg_core1, VCP_DRAM_RESV_LOGGER_OFF,
            VCP_PACK_IOVA(vcp_mem_pa + VCP_MEM_LOGGER_OFFSET));
        e = vcp_smc_ok(MMUP_OP_RESET_RELEASE, 1, &smc_r1);
        if (!e) { stage = 4; e = vcp_poll_ready(mmup_mbox4, &mmup_sz, &mmup_clr); }
    }
    if (!e) {
        stage = 5;
        mm_write_reg(vcp_cfg_core0, VCP_DRAM_RESV_ADDR_OFF, VCP_PACK_IOVA(vcp_mem_pa));
        mm_write_reg(vcp_cfg_core0, VCP_DRAM_RESV_SIZE_OFF, VCP_MEM_TOTAL_SIZE);
        mm_write_reg(vcp_cfg_core0, VCP_DRAM_RESV_LOGGER_OFF,
            VCP_PACK_IOVA(vcp_mem_pa + VCP_MEM_LOGGER_OFFSET + VCP_MEM_LOGGER_SIZE));
        e = vcp_smc_ok(VCP_OP_RESET_RELEASE, 1, &smc_r0);
        if (!e) { stage = 6; e = vcp_poll_ready(vcp_mbox2, &vcp_sz, &vcp_clr); }
    }
    if (!e) {
        vcp_fault = false;
        vcp_booted = true;
    }
    printf("X9 MM VCP BOOT: stage=%u pa=%llx init=%x/%x end=%x/%x pc=%x/%x rstn=%x/%x "
        "smc=%llx/%llx/%llx/%llx mmup=%x/%x vcp=%x/%x errno=%d\n",
        stage, (unsigned long long)vcp_mem_pa, init_c0, init_c1,
        mm_read(vcp_cfg_core0, VCP_CORE_STATUS_OFF), mm_read(vcp_cfg_core1, VCP_CORE_STATUS_OFF),
        mm_read(vcp_cfg_core0, VCP_CORE_MON_PC_OFF), mm_read(vcp_cfg_core1, VCP_CORE_MON_PC_OFF),
        mm_read(vcp_cfg_core0, VCP_CORE_RSTN_CLR_OFF), mm_read(vcp_cfg_core1, VCP_CORE_RSTN_CLR_OFF),
        (unsigned long long)smc_s0, (unsigned long long)smc_s1,
        (unsigned long long)smc_r1, (unsigned long long)smc_r0,
        mmup_clr, mmup_sz, vcp_clr, vcp_sz, e);
    if (e) vcp_boot_diag_log();
    return e;
}
/* Android regmap_read_poll_timeout_atomic uses an elapsed-time budget and
 * takes one final sample when the deadline has passed. */
static int mm_irq_poll(unsigned off, uint32_t target, unsigned step_us,
                       unsigned timeout_us, struct mm_irq_timeout *timeout)
{
    uint64_t deadline;
    clock_interval_to_deadline(timeout_us, NSEC_PER_USEC, &deadline);
    for (;;) {
        if ((mm_read(hwv, off) & MM_POWER_BIT) == target) return 0;
        if (mach_absolute_time() > deadline) {
            uint32_t value = mm_read(hwv, off);
            if ((value & MM_POWER_BIT) == target) return 0;
            *timeout = (struct mm_irq_timeout){ .offset = off, .value = value };
            return ETIMEDOUT;
        }
        delay(step_us);
    }
}
/* v1 HWCCF wait_done delays after every global EN read, including the
 * sample that succeeds or exhausts the retry count. */
static int mm_irq_wait_global(struct mm_irq_timeout *timeout)
{
    for (unsigned retry = 0;; retry++) {
        uint32_t global = mm_read(hwv, MM_POWER_GLOBAL);
        delay(1);
        if (global & MM_POWER_BIT) return 0;
        if (retry == MM_IRQ_GLOBAL_MAX_RETRIES) {
            *timeout = (struct mm_irq_timeout){ .offset = MM_POWER_GLOBAL, .value = global };
            return ETIMEDOUT;
        }
    }
}
/* MT6993 clkchk lists parallel IRQ address and data history registers.
 * Read in slot order. Firmware may update the ring during the snapshot. */
struct mm_irq_history {
    uint32_t first_pointer, last_pointer;
    uint32_t address[MM_POWER_VOTE_IRQ_HIS_SLOTS];
    uint32_t data[MM_POWER_VOTE_IRQ_HIS_SLOTS];
};

static void mm_irq_history_read(struct mm_irq_history *history)
{
    history->first_pointer = mm_read(hwv, MM_POWER_VOTE_IRQ_HIS_POINTER);
    for (unsigned i = 0; i < MM_POWER_VOTE_IRQ_HIS_SLOTS; i++) {
        history->address[i] = mm_read(hwv, MM_POWER_VOTE_IRQ_HIS_ADDR + 4 * i);
        history->data[i] = mm_read(hwv, MM_POWER_VOTE_IRQ_HIS_DATA + 4 * i);
    }
    history->last_pointer = mm_read(hwv, MM_POWER_VOTE_IRQ_HIS_POINTER);
}

static void mm_irq_history_log(const struct mm_irq_history *history)
{
    printf("X9 MM POWER IRQ HISTORY: pointer_before=%x pointer_after=%x\n",
        history->first_pointer, history->last_pointer);
    for (unsigned i = 0; i < MM_POWER_VOTE_IRQ_HIS_SLOTS; i++)
        printf("X9 MM POWER IRQ HISTORY: slot=%u addr=%08x data=%08x\n",
            i, history->address[i], history->data[i]);
}

static void mm_log(const char *op, uint32_t client, unsigned stage, int error,
                   struct mm_irq_timeout timeout, uint32_t int_before, uint32_t int_status)
{
    printf("X9 MM POWER %s: client=%u backend=hwccf MM_2 bit=3 stage=%u "
        "owned=%u held=%u ap=%x global=%x sta=%x done=%x ack2=%x "
        "infra=%x all_sta=%x fsm=%x mt0sta=%x mt1sta=%x en6=%x int=%x "
        "int_raw=%x irqhis=%x errno=%d timeout_off=%x timeout_raw=%x int_before=%x\n",
        op, client, stage, vote_owned, vote_held,
        mm_read(hwv, MM_POWER_EN), mm_read(hwv, MM_POWER_GLOBAL),
        mm_read(hwv, MM_POWER_STA), mm_read(hwv, MM_POWER_DONE),
        mm_read(power_ack, MM_POWER_ACK), mm_read(hwv, 0x12900),
        mm_read(hwv, MM_POWER_ALL_STA), mm_read(hwv, MM_POWER_FSM),
        mm_read(hwv, 0x1131c), mm_read(hwv, 0x11334),
        mm_read(hwv, MM_POWER_REG_EN6),
        int_status & MM_POWER_INT_BACKUP1_TIMEOUT, int_status,
        mm_read(hwv, MM_POWER_VOTE_IRQ_HIS_POINTER), error, timeout.offset, timeout.value,
        int_before);
}
static int mm_vote_take(uint32_t client)
{
    struct mm_irq_timeout timeout = {0};
    struct mm_irq_history history;
    bool log_history;
    uint32_t int_before, int_status;
    unsigned stage = 1;
    int e;
    /* v1 nowait checks for a repeat after waking the voter bus.
     * Even a borrowed vote goes through the outer wait_done. */
    (void)mm_read(mmup, 0x1ec);
    int_before = mm_read(hwv, MM_POWER_INT_STATUS);
    if (!(mm_read(hwv, MM_POWER_EN) & MM_POWER_BIT)) {
        e = mm_irq_poll(MM_POWER_STA, 0, 1, MM_IRQ_PREPARE_TIMEOUT_US, &timeout);
        if (e) goto out;
        if (!(mm_read(hwv, MM_POWER_EN) & MM_POWER_BIT)) {
            vote_owned = true; /* BEFORE the one potentially ambiguous SET */
            mm_write(MM_POWER_SET, MM_POWER_BIT);
            stage = 2;
            e = mm_irq_poll(MM_POWER_EN, MM_POWER_BIT, 2, MM_IRQ_VOTE_TIMEOUT_US, &timeout);
            if (e) goto out;
        }
    }
    stage = 3;
    e = mm_irq_wait_global(&timeout);
    if (e) goto out;
    stage = 4;
    e = mm_irq_poll(MM_POWER_STA, 0, 1, MM_IRQ_DONE_TIMEOUT_US, &timeout);
    if (e) goto out;
    /* Android samples the MM_2 ACK once after HWCCF completes.
     * Native additionally gates token admission on MM_2 and infra0/AO ACK. */
    stage = 5;
    e = mm_wait(power_ack, MM_POWER_ACK, MM_POWER_ACK_MASK,
        MM_POWER_ACK_MASK, 10000, 1);
    if (e) goto out;
    stage = 6;
    e = mm_wait(hwv, 0x12900, MM_INFRA_BITS, MM_INFRA_BITS, 10000, 1);
    if (!e) vote_held = true;
out:
    /* A failed issued vote stays in the journal; do not compensate/replay. */
    if (e && vote_owned) power_fault = true;
    int_status = mm_read(hwv, MM_POWER_INT_STATUS);
    log_history = stage == 4 && e == ETIMEDOUT;
    if (log_history) mm_irq_history_read(&history);
    mm_log("GET", client, stage, e, timeout, int_before, int_status);
    if (log_history) {
        /* Keep both samples on a short separate line: concurrent console
         * output can truncate the longer GET line. VIP route and ACK are
         * observations only; no interrupt status or pending vote is cleared. */
        printf("X9 MM POWER IRQ SAMPLE: before=%x after=%x output=%x req_ack=%x\n",
            int_before, int_status, mm_read(hwv, 0x10004), mm_read(hwv, 0x10008));
        /* RV_v2 maps core0 status at cfg_core0+0x70 and core1 (MMUP) at
         * cfg_core1+0x70. GPR1/2/3 are the reset-time MMUP reserved-memory
         * address/size/logger words. These are observations only. */
        if (vcp_cfg_core0 && vcp_cfg_core1)
            printf("X9 MM POWER VCP STATE: core0=%x core1=%x mmup_gpr=%x/%x/%x\n",
                mm_read(vcp_cfg_core0, 0x70), mm_read(vcp_cfg_core1, 0x70),
                mm_read(vcp_cfg_core1, 0x44), mm_read(vcp_cfg_core1, 0x48),
                mm_read(vcp_cfg_core1, 0x4c));
        mm_irq_history_log(&history);
    }
    return e;
}
static int mm_vote_drop(uint32_t client)
{
    struct mm_irq_timeout timeout = {0};
    uint32_t int_before;
    unsigned stage = 1;
    int e = 0;
    (void)mm_read(mmup, 0x1ec);
    int_before = mm_read(hwv, MM_POWER_INT_STATUS);
    e = mm_irq_poll(MM_POWER_STA, 0, 1, MM_IRQ_PREPARE_TIMEOUT_US, &timeout);
    if (e) goto out;
    if (!(mm_read(hwv, MM_POWER_EN) & MM_POWER_BIT)) {
        e = ESTALE; goto out;
    }
    if (vote_owned) {
        stage = 2;
        mm_write(MM_POWER_CLR, MM_POWER_BIT); /* Only the AP bit we added */
        e = mm_irq_poll(MM_POWER_EN, 0, 2, MM_IRQ_VOTE_TIMEOUT_US, &timeout);
        if (e) goto out;
        delay(100); /* _v1_hwccf_irq_voter_wait_done UNVOTE settling */
        stage = 3;
        e = mm_irq_poll(MM_POWER_STA, 0, 1, MM_IRQ_DONE_TIMEOUT_US, &timeout);
        if (e) goto out;
        /* Other XPUs may still need MM_2: do not require global/ACK OFF. */
    }
    vote_owned = vote_held = false;
out:
    if (e) power_fault = true; /* An attempted drop is never retried */
    mm_log("PUT", client, stage, e, timeout, int_before, mm_read(hwv, MM_POWER_INT_STATUS));
    return e;
}
static bool mm_client(uint32_t client)
{ return client == X9_MM_POWER_CAMERA || client == X9_MM_POWER_VCP; }
static int mm_parent(uint64_t token, uint32_t client, unsigned *index)
{
    if (!token || !mm_client(client)) return EINVAL;
    for (unsigned i = 0; i < MM_POWER_PARENTS; i++)
        if (parents[i].token == token) {
            if (parents[i].client != client) return EACCES;
            *index = i;
            return 0;
        }
    return ESTALE;
}
static int mm_child(uint64_t parent, uint64_t token, unsigned *index)
{
    if (!token) return EINVAL;
    for (unsigned i = 0; i < MM_POWER_CHILDREN; i++)
        if (children[i].token == token) {
            if (children[i].parent != parent) return EACCES;
            *index = i;
            return 0;
        }
    return ESTALE;
}
static int mm_vote_live(void)
{
    for (unsigned pass = 0; pass < 2; pass++) {
        if (mm_read(hwv, MM_POWER_STA) & MM_POWER_BIT) return EBUSY;
        if (!(mm_read(hwv, MM_POWER_EN) & MM_POWER_BIT) ||
            !(mm_read(hwv, MM_POWER_GLOBAL) & MM_POWER_BIT) ||
            (mm_read(power_ack, MM_POWER_ACK) & MM_POWER_ACK_MASK) !=
                MM_POWER_ACK_MASK) return ENXIO;
        if (mm_read(hwv, 0x1131c) & MM_INFRA_BITS) return EBUSY;
        if ((mm_read(hwv, 0x12900) & MM_INFRA_BITS) != MM_INFRA_BITS) return ENXIO;
        if (mm_read(hwv, MM_POWER_STA) & MM_POWER_BIT) return EBUSY;
    }
    return 0;
}
static int mm_live(uint64_t token, uint32_t client, unsigned *index)
{
    int e = mm_parent(token, client, index);
    if (e) return e;
    if (power_fault || !hwv || !power_ack || !vote_held || !holders) return EIO;
    return mm_vote_live();
}
static bool mm_output_overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x > UINTPTR_MAX - an || y > UINTPTR_MAX - bn ||
        (x < y + bn && y < x + an);
}

int x9_mm_power_acquire(uint32_t client, uint64_t *token, uint32_t *a0)
{
    unsigned slot;
    int e = 0;
    if (!token || !a0 || *token || !mm_client(client) ||
        mm_output_overlap(token, sizeof(*token), a0, sizeof(*a0))) return EINVAL;
    *a0 = 0;
    if (!sweet_early_is_x9()) return ENXIO;
    lck_mtx_lock(&mm_power_lock);
    if (power_fault) { e = EIO; goto out; }
    if (!next_token || next_token == UINT64_MAX) { e = EOVERFLOW; goto out; }
    for (slot = 0; slot < MM_POWER_PARENTS; slot++) if (!parents[slot].token) break;
    if (slot == MM_POWER_PARENTS) { e = ENOSPC; goto out; }
    if (!hwv) hwv = ml_io_map(0x31c00000, 0x22000);
    if (!mmup) mmup = ml_io_map(0x31a04000, 0x1000);
    if (!power_ack) power_ack = ml_io_map(0x31b50000, 0x1000);
    /* Live DT vcp@31800000 + mmup@31a00000: core0/1 cfg + mbox2/4 READY. */
    if (!vcp_cfg_core0) vcp_cfg_core0 = ml_io_map(0x31a0a000, 0x1000);
    if (!vcp_cfg_core1) vcp_cfg_core1 = ml_io_map(0x31a0d000, 0x1000);
    if (!vcp_mbox2) vcp_mbox2 = ml_io_map(0x31ba0000, 0x1000);
    if (!mmup_mbox4) mmup_mbox4 = ml_io_map(0x31bc0000, 0x1000);
    if (!hwv || !mmup || !power_ack || !vcp_cfg_core0 || !vcp_cfg_core1 ||
        !vcp_mbox2 || !mmup_mbox4) { e = ENOMEM; goto out; }
    e = vcp_boot_ensure();
    if (e) goto out;
    if (!holders) {
        if (vote_held || vote_owned) { e = EIO; goto out; }
        e = mm_vote_take(client);
        if (e && !vote_owned) goto out; /* No physical mutation to retain */
    } else {
        if (!vote_held) { e = EIO; goto out; }
        e = mm_vote_live();
        if (e) goto out;
    }
    parents[slot].token = next_token++;
    parents[slot].client = client;
    parents[slot].children = 0;
    holders++;
    *token = parents[slot].token;
out:
    lck_mtx_unlock(&mm_power_lock);
    return e;
}
int x9_mm_power_validate(uint64_t token, uint32_t client)
{
    unsigned i = 0;
    int e;
    lck_mtx_lock(&mm_power_lock);
    e = mm_live(token, client, &i);
    lck_mtx_unlock(&mm_power_lock);
    return e;
}
int x9_mm_power_pin(uint64_t parent, uint32_t client, uint64_t *child)
{
    unsigned i = 0, slot;
    int e;
    if (!child || *child) return EINVAL;
    lck_mtx_lock(&mm_power_lock);
    e = mm_live(parent, client, &i);
    if (e) goto out;
    if (!next_token || next_token == UINT64_MAX) { e = EOVERFLOW; goto out; }
    for (slot = 0; slot < MM_POWER_CHILDREN; slot++) if (!children[slot].token) break;
    if (slot == MM_POWER_CHILDREN) { e = ENOSPC; goto out; }
    children[slot].token = next_token++;
    children[slot].parent = parent;
    parents[i].children++;
    *child = children[slot].token;
out:
    lck_mtx_unlock(&mm_power_lock);
    return e;
}
int x9_mm_power_validate_pin(uint64_t parent, uint64_t child, uint32_t client)
{
    unsigned i = 0, c = 0;
    int e;
    lck_mtx_lock(&mm_power_lock);
    e = mm_parent(parent, client, &i);
    if (!e) e = mm_child(parent, child, &c);
    if (!e) e = mm_live(parent, client, &i);
    lck_mtx_unlock(&mm_power_lock);
    return e;
}
int x9_mm_power_unpin(uint64_t parent, uint64_t *child, uint32_t client)
{
    unsigned i = 0, c = 0;
    int e;
    if (!child) return EINVAL;
    lck_mtx_lock(&mm_power_lock);
    e = mm_parent(parent, client, &i);
    if (!e) e = mm_child(parent, *child, &c);
    /* No MMIO is needed to retire an already-drained software child.
     * This cannot release a faulted parent's physical reference. */
    if (!e && !parents[i].children) e = EFAULT;
    if (!e) {
        children[c].token = 0; children[c].parent = 0;
        parents[i].children--; *child = 0;
    }
    lck_mtx_unlock(&mm_power_lock);
    return e;
}
int x9_mm_power_release(uint64_t *token, uint32_t client, uint32_t *a0)
{
    unsigned i = 0;
    int e;
    if (!token || !a0 || mm_output_overlap(token, sizeof(*token), a0, sizeof(*a0)))
        return EINVAL;
    *a0 = 0;
    lck_mtx_lock(&mm_power_lock);
    e = mm_parent(*token, client, &i);
    if (!e && (power_fault || !vote_held || !holders)) e = EIO;
    if (!e && parents[i].children) e = EBUSY;
    if (!e) e = mm_vote_live();
    if (!e) {
        /* Keep the physical vote through every live parent/child. */
        if (holders == 1) e = mm_vote_drop(client);
        if (!e) {
            parents[i].token = 0; parents[i].client = 0;
            holders--;
            *token = 0;
        }
    }
    lck_mtx_unlock(&mm_power_lock);
    return e;
}
