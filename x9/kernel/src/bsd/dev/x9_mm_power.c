/* Shared MM_SMMU=0 reference authority. Captured loaded smmu_secure.ko:
 * mtk_smmu_pm_get 0x414/420/43c/448, pm_put 0x4d0/4dc/4f8/504:
 * SMC c2000514, x1=803/804, x2..x7=0, status uses low32 a0.
 * AP HWV 31c00000 MM_INFRA bits31/29 match the existing camera provider.
 * No SMMU reset/SEC_INIT, no HWV vote writes or automatic power acquisition. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
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
#define MM_SMMU_BACKUP_BIT (1u << 3)
static struct { uint64_t token; uint32_t client, children; } parents[MM_POWER_PARENTS];
static struct { uint64_t token, parent; } children[MM_POWER_CHILDREN];
static uint64_t next_token = 1;
static vm_offset_t hwv;
static bool smc_fault;
static bool secure_owned;
static uint32_t holders;

static uint32_t mm_secure_power(bool on)
{
    register uint64_t x0 __asm__("x0") = 0xc2000514u;
    register uint64_t x1 __asm__("x1") = on ? 0x803u : 0x804u;
    register uint64_t x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
    register uint64_t x4 __asm__("x4") = 0, x5 __asm__("x5") = 0;
    register uint64_t x6 __asm__("x6") = 0, x7 __asm__("x7") = 0;
    __asm__ volatile("dsb sy; smc #0; dsb sy; isb"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4),
          "+r"(x5), "+r"(x6), "+r"(x7) : : "memory");
    return (uint32_t)x0;
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
/* Current original ATF: MM first/last PM reference -> 28d48/28f08 ->
 * 21424(type1, group0x37, bit3). hwccf.h independently identifies +1135c
 * as BACKUP1_STA, NOT an enable, DONE or normal-controller semaphore.
 * In particular 28f08 discards the HWV helper's return before returning0.
 * Sample only this transaction's bit, from AO HWV, without polling, writes,
 * accessing firmware's XPU3 vote bank or guessing its retained refcount.
 * An idle pair is necessary, never proof of a power-off/NS/DMA grant. */
static int mm_backup_idle(uint32_t state[2])
{
    state[0] = *(volatile uint32_t *)(hwv + 0x1135c);
    __asm__ volatile("dsb sy" : : : "memory");
    state[1] = *(volatile uint32_t *)(hwv + 0x1135c);
    __asm__ volatile("dsb sy" : : : "memory");
    return ((state[0] | state[1]) & MM_SMMU_BACKUP_BIT) ? EBUSY : 0;
}
static int mm_live(uint64_t token, uint32_t client, unsigned *index)
{
    int e = mm_parent(token, client, index);
    uint32_t ack, sta, again, pending, backup[2];
    if (e) return e;
    if (smc_fault || !hwv || !secure_owned || !holders) return EIO;
    /* These are AP/AO HWV registers, not a speculative SMMU/MMUP read.
     * Only inspect them after finding a successfully acquired reference. */
    ack = *(volatile uint32_t *)(hwv + 0x12900);
    sta = *(volatile uint32_t *)(hwv + 0x1131c);
    __asm__ volatile("dsb sy" : : : "memory");
    again = *(volatile uint32_t *)(hwv + 0x12900);
    pending = *(volatile uint32_t *)(hwv + 0x1131c);
    __asm__ volatile("dsb sy" : : : "memory");
    if (((ack ^ again) & MM_INFRA_BITS) || ((sta | pending) & MM_INFRA_BITS))
        return EBUSY;
    if ((ack & MM_INFRA_BITS) != MM_INFRA_BITS) return ENXIO;
    /* Powered common infra alone does not qualify an in-flight SMMU
     * backup transaction. No MMUP/SMMU access follows a busy observation. */
    return mm_backup_idle(backup);
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
    if (smc_fault) { e = EIO; goto out; }
    if (!next_token || next_token == UINT64_MAX) { e = EOVERFLOW; goto out; }
    for (slot = 0; slot < MM_POWER_PARENTS; slot++) if (!parents[slot].token) break;
    if (slot == MM_POWER_PARENTS) { e = ENOSPC; goto out; }
    if (!hwv) hwv = ml_io_map(0x31c00000, 0x22000);
    if (!hwv) { e = ENOMEM; goto out; }
    if (!holders) {
        if (secure_owned) { e = EIO; goto out; }
        *a0 = mm_secure_power(true);
        if (*a0) {
            smc_fault = true; e = EIO;
            goto out; /* no successful GET, no compensating PUT or retry */
        }
        secure_owned = true;
    } else if (!secure_owned) {
        e = EIO; goto out;
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
    /* No MMIO/SMC is needed to retire an already-drained software child.
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
    if (!e && (smc_fault || !secure_owned || !holders)) e = EIO;
    if (!e && parents[i].children) e = EBUSY;
    if (!e) {
        /* Never issue shared power SMCs while another client's reference
         * (hence possibly its exposed DMA) remains live. */
        if (holders == 1) {
            *a0 = mm_secure_power(false);
            if (*a0) { smc_fault = true; e = EIO; }
            else {
                uint32_t backup[2];
                e = mm_backup_idle(backup);
                if (e) {
                    /* The PUT was ALREADY attempted. Its outer0 can hide
                     * a HWV timeout; do not drop the caller's journal, send
                     * another GET/PUT or claim that firmware still holds
                     * its old count. Fault blocks every live/pin path. */
                    smc_fault = true;
                    printf("X9 MM POWER PUT: client=%u a0=%x backup_sta=%x/%x "
                        "errno=%d (attempted, retain journal, no retry)\n",
                        client, *a0, backup[0], backup[1], e);
                } else secure_owned = false;
            }
        }
        if (!e) {
            parents[i].token = 0; parents[i].client = 0;
            holders--;
            *token = 0;
        }
    }
    lck_mtx_unlock(&mm_power_lock);
    return e;
}
