#pragma once
#include "ufs_ro.h"
/* Generic/MTK pre-link paths plus opt-in X9 full host/device reset startup.
 * No DMA buffers or storage writes in this module. */
struct x9u_link {
 uint32_t original_hce,original_ie,stage;
 uint32_t hce,hcs,is,uic_result,uic_arg3;
 uint32_t phy_error,dl_error,nl_error,tl_error,dme_error;
 unsigned activated;
 uint32_t last_uic_command,last_uic_attribute,savepower_before,savepower_after,prelink_completed;
 uint32_t ip_version,original_xoufs,original_opt,original_coherence,original_ahit;
 uint32_t reset_low_status,reset_high_status,attempts,coherence;
 unsigned platform_changed;
 uint32_t ready_hcs,repeat_startup_skipped;
};
int x9u_link_start(const struct x9u_io *,struct x9u_link *,int dma_verified);
int x9u_link_start_mtk(const struct x9u_io *,struct x9u_link *,int dma_verified);
/* Reset callback must implement captured LK SMC 0xc200040d, selector2,
 * high in x2, host0 in x4; operation2 performs DT-verified host reset.
 * Return 0 only on successful operation. */
int x9u_link_start_full(const struct x9u_io *,struct x9u_link *,int,
                      int (*reset)(void *,unsigned));
int x9u_link_stop(const struct x9u_io *,struct x9u_link *);

int x9u_link_handoff(const struct x9u_io *,struct x9u_link *);
