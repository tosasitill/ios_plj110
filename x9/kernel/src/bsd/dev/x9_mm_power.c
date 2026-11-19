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
#include "x9_mm_power.h"
extern int sweet_early_is_x9(void);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
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
static struct { uint64_t token; uint32_t client, children; } parents[MM_POWER_PARENTS];
static struct { uint64_t token, parent; } children[MM_POWER_CHILDREN];
static uint64_t next_token = 1;
static vm_offset_t hwv, mmup, power_ack, vcp_cfg_core0, vcp_cfg_core1;
static bool power_fault, vote_owned, vote_held;
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
static void mm_write(unsigned off, uint32_t value)
{
    *(volatile uint32_t *)(hwv + off) = value;
    __asm__ volatile("dsb sy" : : : "memory");
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
    /* Live DT vcp@31800000 reg-names: vcp_cfgreg_core0/core1 at
     * 31a0a000/31a0d000. The read-only status diagnostic is mapped lazily so
     * an unrelated mapping failure cannot change the existing vote path. */
    if (!vcp_cfg_core0) vcp_cfg_core0 = ml_io_map(0x31a0a000, 0x1000);
    if (!vcp_cfg_core1) vcp_cfg_core1 = ml_io_map(0x31a0d000, 0x1000);
    if (!hwv || !mmup || !power_ack) { e = ENOMEM; goto out; }
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
