#pragma once
#include <stdint.h>
/* Log180 qualified G5/FAST/RateB/2 lanes. X9 defaults to that ceiling;
 * sweet_x9_arg still honors ug=0 (off), ug=4 and ug=5 explicitly. */
#define X9U_DEFAULT_HIGH_GEAR 5
/* Lock order: native UFS mutex -> DVFSRC mutex. Setup releases the DVFSRC
 * mutex before entering UFS. The UFS vote is monotonic for the whole boot;
 * no release/downscale API while the link could still run at G4/G5. */
int sweet_x9_vcore_ufs_hold(uint32_t snapshot[3]); /* before/after SW_REQ, current */
int sweet_x9_vcore_ufs_ok(void);
/* Read-only controller snapshot: BASIC, SW_REQ, CUR_TAR_GEAR. No INIT/vote. */
int sweet_x9_vcore_ufs_check(uint32_t snapshot[3]);
void sweet_x9_ufs_late_high(void);
/* Normal pre-panic context only; formats retained state, no hardware access. */
void sweet_x9_ufs_boot_failure(char *out, unsigned capacity);
