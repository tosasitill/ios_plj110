#pragma once
#include "ufs_ro.h"
/* Standard UniPro PA attributes in this array order. */
struct x9u_hs_state {
 uint32_t stage,last_command,last_attribute,uic_result,upmcrs;
 uint32_t before[6],after[6]; /* mode,TXgear,RXgear,TXlanes,RXlanes,HSseries */
 uint32_t max_rx_gear,peer_max_rx_gear,tx_lanes,rx_lanes,refclk_before,refclk_after;
 unsigned changed;
};
int x9u_hs_read(const struct x9u_io *,struct x9u_hs_state *,uint32_t out[6]);
int x9u_hs_valid(const uint32_t state[6]);
/* Select FASTAUTO/G1/rateB, up to two physically connected lanes. No DMA or
 * media writes; a failed/uncertain power transition prohibits root handoff.
 * refclk callback follows captured MTK SiP bit3 notification(on,stage0/1).
 */
int x9u_hs_enable(const struct x9u_io *,struct x9u_hs_state *,
                  int (*refclk)(void *,unsigned,unsigned));
