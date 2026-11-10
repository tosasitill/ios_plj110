/* NXP container byte parser shared with iOS diagnostics; vendor data excluded. */
#ifndef X9_TFA98XX_H
#define X9_TFA98XX_H
#include <stdint.h>
#define X9_TFA_CONTAINER_MAX (1024u*1024u)
struct x9_tfa_profile {
    uint32_t crc, device_offset[2], profile_offset[2], prefix_items[2];
    uint32_t opaque_items[2];
    char device_name[2][64], profile_name[2][64];
};
/* Exact profile name is required; no default/fallback/reverse profile guess.
 * CRC, size, both I2C-kind (bus=0) devices and selected profile descriptors checked.
 * Output changes only after full validation; never a hardware-ready flag. */
int x9_tfa_select_profile(const void *,uint32_t,const char *,struct x9_tfa_profile *);
/* Baseline-independent exclusions, shared by kernel and offline audioctl.
 * Passing is NOT a revision-specific allowlist, write lease or Ready proof.
 * Only leading register/bitfield prefixes are checked; opaque items are not
 * executed. Failure location is UINT32_MAX when container parsing failed. */
struct x9_tfa_prefix_check {
    uint32_t version, checked[2], error;
    uint32_t device, profile, descriptor, reg, mask, value;
};
int x9_tfa_check_register_prefix(const void *,uint32_t,const char *,struct x9_tfa_prefix_check *);
#if defined(KERNEL)
struct x9_tfa_bus {
    uint32_t version, verified;
    uint16_t expected_revision[2]; /* actual full reg03 readback, not DT label */
    void *context;
    /* I2C6 lease includes correct reset GPIO controller, rails, audited bus clock
     * and mutual exclusion. read/write implement reg8/value16 big-endian;
     * each access bounded; no blind replay of a timed-out write. */
    int (*read16)(void *,uint8_t slave,uint8_t reg,uint16_t *);
    int (*write16)(void *,uint8_t slave,uint8_t reg,uint16_t);
    /* Confirmed per-revision init/profile whitelist, excludes MTP/OTP,
     * calibration/boost/key/trigger writes and power/AMPE transitions. Must
     * also exclude undocumented action bits in any otherwise admitted CSR;
     * the shared guard is only a conservative baseline-independent filter. */
    int (*allow_patch)(void *,uint16_t revision,uint8_t reg,uint16_t mask,uint16_t value);
};
#define X9_TFA_BUS_REQUIRED 7u /* real I2C/rail/reset lease + fresh reset + trusted patch policy */
struct x9_tfa_status {
    uint16_t revision[2];uint32_t muted, applied[2], error, mute_error;
    uint32_t revision_verified, poisoned, write_attempts[2];
    uint32_t mute_attempts[2], sysctrl[2], mute_uncertain;
};
/* These are fresh-reset transactions, not recovery/retry APIs. The provider
 * must retain the lease after any poisoned/uncertain result and obtain a real
 * reset before calling again; replacing the status object is not recovery. */
int x9_tfa_probe_and_mute(const struct x9_tfa_bus *,struct x9_tfa_status *);
int x9_tfa_apply_register_prefix(const struct x9_tfa_bus *,const void *,uint32_t,
    const char *profile,struct x9_tfa_status *);
#endif
#endif
