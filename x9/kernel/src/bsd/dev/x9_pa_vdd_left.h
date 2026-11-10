#ifndef X9_PA_VDD_LEFT_H
#define X9_PA_VDD_LEFT_H
#include <stdint.h>
#define X9_PA_LEFT_VDD_STATUS_VERSION 1u
#define X9_PA_LEFT_VDD_VSEL_REG 0x24bu
#define X9_PA_LEFT_VDD_SW_REG 0x2087u
#define X9_PA_LEFT_VDD_DA_REG 0x20a2u
#define X9_PA_LEFT_VDD_HW_REG 0x20afu
#define X9_PA_LEFT_VDD_UV 1800000u
#define X9_PA_LEFT_VDD_VSEL 0xd0u
#define X9_PA_LEFT_VDD_HW_NORMAL 2u
#define X9_PA_LEFT_VDD_DA_EN 1u
#define X9_PA_LEFT_VDD_DA_LP 4u
enum { X9LeftVDDHeld=1u,X9LeftVDDCaptured=2u,X9LeftVDDConfigured=4u,
       X9LeftVDDTeardown=8u,X9LeftVDDRestored=16u };
typedef struct {
    uint32_t version,flags,generation,error;
    uint32_t dirty,uncertain,pending,restore_error;
    uint32_t selector,control,da,hw_op;
    uint64_t ticket,lease;
} X9AudioLeftVDDStatus; /* 64B, cached; no RIGHT/analogue-voltage/Ready grant */
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
#define X9_PA_LEFT_VDD_POLICY_VERSION 1u
struct x9_pa_left_vdd_policy {
    uint32_t version,generation;
    uint64_t ticket;
    void *context;
    /* Mandatory already-PINNED authority excluding other local/remote
     * SID5 LNC0_8 writers and foreign SID3 GPIO10/11 reset writers for the
     * WHOLE lifetime, including failed cleanup. Must also protect attribution
     * of this owner's pending PMIF SWINF2 response from generic VLDCLR users.
     * The per-transaction PMIF mutex, FDT and DA bits cannot supply it.
     * No default supplier. Bounded, runs OUTSIDE PMIF mutex; no rail/PA/
     * I2C6/AFE/clock lifecycle reentry. Drop authority AFTER rail release. */
    int (*validate_exclusive)(void *,uint64_t ticket,uint32_t generation);
    /* Independently prove the selected AFE has no outstanding access after
     * actual producer halt, not EN0/status cache/input-consumed/time. */
    int (*validate_drained)(void *,uint64_t ticket,uint32_t generation,
                           uint64_t afe_bus,uint32_t bytes);
};
/* Real SID5 LNC0_8 LEFT digital VDD programming: 1.8V selector0xd0,
 * normal LP0/HW1_OP_EN bit1, SW_EN bit0 and actual DA EN/LP readback.
 * Cold/off only; reject borrowed/live rail. Acquire BEFORE PA reset;
 * independent clock child prevents parent teardown. Nonzero lease also
 * returned on partial failure; not an access/profile/unmute grant.
 * Explicit cleanup only, no automatic unwind or acquisition replay.
 * Backend serializes lifecycle. Lock PA -> rail -> PMIF -> clock/board/AP;
 * rail release never takes PA mutex. No PMIF mutex held across callbacks. */
int x9_pa_vdd_left_acquire(const struct x9_pa_left_vdd_policy *,uint64_t *lease);
int x9_pa_vdd_left_validate(uint64_t ticket,uint64_t lease);
/* Only the LEFT part of a real dual-power supplier. Caller must independently
 * pin/prove RIGHT supply, including any analogue rail/boost needed by PA.
 * Cannot pass this alone as x9_pa_reset_policy.validate_power. */
int x9_pa_vdd_left_release(uint64_t ticket,uint64_t lease);
int x9_pa_vdd_left_status(X9AudioLeftVDDStatus *);
#ifdef __cplusplus
}
#endif
#endif
#endif
