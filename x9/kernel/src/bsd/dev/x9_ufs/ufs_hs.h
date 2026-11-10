#pragma once
#include "ufs_ro.h"
/* Standard UniPro PA attributes in this array order. */
struct x9u_hs_state {
 uint32_t stage,last_command,last_attribute,uic_result,upmcrs;
 uint32_t before[6],after[6]; /* mode,TXgear,RXgear,TXlanes,RXlanes,HSseries */
 uint32_t max_rx_gear,peer_max_rx_gear,tx_lanes,rx_lanes,refclk_before,refclk_after;
 uint32_t local_version,target_gear;
 unsigned changed,uncertain;
};
int x9u_hs_read(const struct x9u_io *,struct x9u_hs_state *,uint32_t out[6]);
int x9u_hs_valid(const uint32_t state[6]);
/* Select FASTAUTO/G1/rateB, up to two physically connected lanes. No DMA or
 * media writes; a failed/uncertain power transition prohibits root handoff.
 * refclk callback follows captured MTK SiP bit3 notification(on,stage0/1).
 */
int x9u_hs_enable(const struct x9u_io *,struct x9u_hs_state *,
                  int (*refclk)(void *,unsigned,unsigned));
/* Kernel late scaling only. Prepare does GETs, never changes PA configuration.
 * Caller must hold the shared transport mutex and establish/retain the MT6993
 * VCORE floor and high UFS/FDE clocks before apply. No reset or media writes.
 * requested=4/5 is a ceiling, negotiated against both actual RX capabilities.
 * Any uncertain UIC or partial SET prohibits further DMA, not a retry/fallback.
 */
int x9u_hs_prepare_high(const struct x9u_io *,struct x9u_hs_state *,unsigned requested);
int x9u_hs_apply_high(const struct x9u_io *,struct x9u_hs_state *,unsigned gear);
