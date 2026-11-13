/* PLJ110 read-only PA identity diagnostic. Separate from TFA write lease. */
#ifndef X9_TFA_PLJ110_H
#define X9_TFA_PLJ110_H
#include <stdint.h>
#define X9_TFA_BOARD_STATUS_VERSION 1u
enum { X9PAProbeAttempted=1u, X9PAPMICCaptured=2u, X9PABusCaptured=4u,
    X9PAPMICStable=8u, X9PAProbeComplete=16u, X9PAJournalHeld=32u,
    X9PAPoisoned=64u, X9PAResetInput=128u, X9PASupplyHWVote=256u,
    X9PAClockUngated=512u, X9PAClockRegated=1024u };
enum { X9PAStageMap=1, X9PAStagePMIC, X9PAStageClock, X9PAStageIdle,
    X9PAStageTiming, X9PAStageLeft, X9PAStageRight, X9PAStageRecheck,
    X9PAStageRestore, X9PAStageDone };
/* All values are CACHED observations, never voltage/rail ownership claims.
 * reset[]: SID3 DIR/DO/DI/MODE/AD_SW_SWITCH/AD_SWITCH raw bytes.
 * supply[]: SID5 selector/EN/DA/HW_OP_EN0 raw bytes, LEFT supply only.
 * DA is not reduced to regulator.c's ambiguous da_en_mask/da_lp_mask.
 * No fresh-reset, calibration, bus-write capability or Ready authorization. */
typedef struct {
    uint32_t version, flags, stage, error, restore_error, dirty_mask;
    uint32_t revision[2], revision_read, known_family;
    uint32_t reset[6], supply[4], reset_after[6], supply_after[4];
    uint32_t pmif_status, gate, dma_gate, dma_enable, pin[2];
    uint32_t parent, pll_con1, nominal_parent_hz, timing_hz;
    uint32_t dma_parent, dma_pll_con1;
    uint32_t bank_start[2], bank_mask[2], bank_status[2], bank_fifo[2];
    uint32_t last_slave, request_status, request_start, request_fifo;
} X9AudioAmpStatus;
#if defined(KERNEL)
#ifdef __cplusplus
extern "C" {
#endif
/* Internal controller mutex also excludes the native I2C6 lease. Status is
 * cached only; neither call registers a provider or automatically probes. */
int x9_tfa_plj110_probe(void);
int x9_tfa_plj110_status(X9AudioAmpStatus *);
#ifdef __cplusplus
}
#endif
#endif
#endif
