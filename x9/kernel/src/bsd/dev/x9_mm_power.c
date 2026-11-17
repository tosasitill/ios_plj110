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
#define MM_POWER_ACK 0xa8u
#define MM_POWER_ACK_MASK 0xc0000000u
static struct { uint64_t token; uint32_t client, children; } parents[MM_POWER_PARENTS];
static struct { uint64_t token, parent; } children[MM_POWER_CHILDREN];
static uint64_t next_token = 1;
static vm_offset_t hwv, mmup, power_ack;
static bool power_fault, vote_owned, vote_held;
static uint32_t holders;

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
                   uint32_t target, unsigned us)
{
    for (unsigned n = 0; n <= us; n++) {
        if ((mm_read(base, off) & mask) == target) return 0;
        if (n != us) delay(1);
    }
    return ETIMEDOUT;
}
static void mm_log(const char *op, uint32_t client, unsigned stage, int error)
{
    printf("X9 MM POWER %s: client=%u backend=hwccf MM_2 bit=3 stage=%u "
        "owned=%u held=%u ap=%x global=%x sta=%x done=%x ack2=%x "
        "infra=%x all_sta=%x fsm=%x mt0sta=%x mt1sta=%x errno=%d\n",
        op, client, stage, vote_owned, vote_held,
        mm_read(hwv, MM_POWER_EN), mm_read(hwv, MM_POWER_GLOBAL),
        mm_read(hwv, MM_POWER_STA), mm_read(hwv, MM_POWER_DONE),
        mm_read(power_ack, MM_POWER_ACK), mm_read(hwv, 0x12900),
        mm_read(hwv, MM_POWER_ALL_STA), mm_read(hwv, MM_POWER_FSM),
        mm_read(hwv, 0x1131c), mm_read(hwv, 0x11334), error);
}
static int mm_vote_take(uint32_t client)
{
    unsigned stage = 1;
    int e;
    /* Same dummy read as _v1_hwccf_irq_voter_nowait, before its STA check.
     * Never use the SMMU itself as an unpowered probe. */
    (void)mm_read(mmup, 0x1ec);
    e = mm_wait(hwv, MM_POWER_STA, MM_POWER_BIT, 0, 10000);
    if (e) goto out;
    if (!(mm_read(hwv, MM_POWER_EN) & MM_POWER_BIT)) {
        vote_owned = true; /* BEFORE the one potentially ambiguous SET */
        mm_write(MM_POWER_SET, MM_POWER_BIT);
        stage = 2;
        e = mm_wait(hwv, MM_POWER_EN, MM_POWER_BIT, MM_POWER_BIT, 1250);
        if (e) goto out;
    }
    stage = 3;
    e = mm_wait(hwv, MM_POWER_GLOBAL, MM_POWER_BIT, MM_POWER_BIT, 10000);
    if (e) goto out;
    stage = 4;
    e = mm_wait(hwv, MM_POWER_STA, MM_POWER_BIT, 0, 10000);
    if (e) goto out;
    /* mminfra-util verifies its DT-selected two-bit MTCMOS ack after ON.
     * MM_2's upstream domain includes infra0/AO, required by the callers. */
    stage = 5;
    e = mm_wait(power_ack, MM_POWER_ACK, MM_POWER_ACK_MASK,
        MM_POWER_ACK_MASK, 10000);
    if (e) goto out;
    stage = 6;
    e = mm_wait(hwv, 0x12900, MM_INFRA_BITS, MM_INFRA_BITS, 10000);
    if (!e) vote_held = true;
out:
    /* A failed issued vote stays in the journal; do not compensate/replay. */
    if (e && vote_owned) power_fault = true;
    mm_log("GET", client, stage, e);
    return e;
}
static int mm_vote_drop(uint32_t client)
{
    unsigned stage = 1;
    int e = 0;
    (void)mm_read(mmup, 0x1ec);
    e = mm_wait(hwv, MM_POWER_STA, MM_POWER_BIT, 0, 10000);
    if (e) goto out;
    if (!(mm_read(hwv, MM_POWER_EN) & MM_POWER_BIT)) {
        e = ESTALE; goto out;
    }
    if (vote_owned) {
        stage = 2;
        mm_write(MM_POWER_CLR, MM_POWER_BIT); /* Only the AP bit we added */
        e = mm_wait(hwv, MM_POWER_EN, MM_POWER_BIT, 0, 1250);
        if (e) goto out;
        delay(100); /* _v1_hwccf_irq_voter_wait_done UNVOTE settling */
        stage = 3;
        e = mm_wait(hwv, MM_POWER_STA, MM_POWER_BIT, 0, 10000);
        if (e) goto out;
        /* Other XPUs may still need MM_2: do not require global/ACK OFF. */
    }
    vote_owned = vote_held = false;
out:
    if (e) power_fault = true; /* An attempted drop is never retried */
    mm_log("PUT", client, stage, e);
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
