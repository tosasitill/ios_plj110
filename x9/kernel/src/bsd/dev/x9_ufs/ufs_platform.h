#pragma once
#include <stdint.h>
/* Lock order: native UFS mutex -> DVFSRC mutex. Setup releases the DVFSRC
 * mutex before entering UFS. The UFS vote is monotonic for the whole boot;
 * no release/downscale API while the link could still run at G4/G5. */
int sweet_x9_vcore_ufs_hold(uint32_t snapshot[3]); /* before/after SW_REQ, current */
int sweet_x9_vcore_ufs_ok(void);
void sweet_x9_ufs_late_high(void);
