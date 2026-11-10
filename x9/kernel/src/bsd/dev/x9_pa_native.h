/* Native dual-PA safety mute; not a container/profile/unmute grant. */
#ifndef X9_PA_NATIVE_H
#define X9_PA_NATIVE_H
#include <stdint.h>
#define X9_PA_NATIVE_STATUS_VERSION 1u
#define X9_PA_NATIVE_SYSCTRL 0u
#define X9_PA_NATIVE_AMPE_MASK 8u
#define X9_PA_NATIVE_I2CR_MASK 2u
typedef struct {
    uint32_t version,generation,error,muted,revision_verified,mute_uncertain;
    uint16_t revision[2],sysctrl[2];
    uint32_t side_error[2];
    uint64_t mute_attempts[2],checks;
} X9AudioPANativeStatus; /* 64B cached; no I2C/reset/Ready authorization */
/* Full stepping ids present in OPPO tfa98xx_v6.14.2 tfa_init.c. This lookup
 * alone is not identity or a lease. Only common AMPE-off is admitted, not
 * the vendor's key/boost/OTP/calibration/init sequences. */
#ifdef __cplusplus
static constexpr int
#else
static inline int
#endif
x9_pa_native_revision_supported(uint16_t revision)
{
    switch(revision) {
    case 0x1a72:case 0x2a72:case 0x1b72:case 0x2b72:case 0x3b72:
    case 0x0a73:case 0x0b73:case 0x1a73:
    case 0x0a74:case 0x0b74:case 0x0c74:return 1;
    default:return 0;
    }
}
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
/* Caller already owns a real native I2C6 child. Same controller mutex covers
 * the WHOLE dual-side transaction, so controller release/other transfers
 * cannot interleave ID/SYSCTRL/write/readback. Initial reg03 reads happen
 * once per actual reset epoch; unsupported/failed identity is never retried
 * into write authority. Unknown mute effects are retained and never replayed.
 * Known side may still be muted after a definite NACK on the other side;
 * pending/uncertain controller effects block every following transfer.
 * Fresh physical cleanup is allowed after historical transport faults, but
 * these APIs never erase the first fault or authorize playback/profile.
 * A new binding requires actual new READY generation AND PA reset epoch,
 * proved by native I2C6 parents, not a newly allocated status/context object.
 * No clock/pad/rail/reset/key/status-IRQ access, no AMPE-on, no verified=7.
 * Lock: AFE/pads -> shared I2C6 -> PA reset/power -> PMIF -> clock/board/AP. */
int x9_pa_native_mute(uint64_t ticket,uint64_t i2c6_lease);
int x9_pa_native_muted_validate(uint64_t ticket,uint64_t i2c6_lease);
int x9_pa_native_status(X9AudioPANativeStatus *);
#ifdef __cplusplus
}
#endif
#endif
#endif
