/* Wired XNU storage for the real VCP/MM stage1 backend. Kernel-only API;
 * no userspace PA interface, no automatic provider or READY publication. */
#ifndef X9_VDEC_DMA_H
#define X9_VDEC_DMA_H
#ifdef KERNEL
#include "x9_mm_smmu.h"
#include "x9_mm_power.h"
struct x9_vdec_dma;
struct x9_vdec_dma_request {
    struct x9_mm_smmu_regs regs; /* current owned controller IDRs, not a DT guess */
    uint64_t epoch, iova;
    uint64_t power_token; /* native VCP parent GET, NOT the reset epoch */
    uint32_t sid, asid, bytes, access;
};
/* One native arena per SID, not one STE/CD per firmware allocation. A second
 * live object for the same SID returns EBUSY (including a quarantined object).
 * Suballocate buffers below: replacing this context for each allocation would
 * either fail on a live STE or destroy all earlier buffers' translations.
 * Allocates zeroed wired 16K CPU pages, discovers EVERY backing PA using
 * kernel_pmap, subdivides table pages into explicit 4K pages, builds the full
 * scatter tree/CD/STE. No hardware call, mapping installation or address
 * export. Buffer length rounds to 16K; this padding is owned/zeroed too.
 * Callers must reserve the entire IOVA extent and a unique nonzero ASID for
 * this epoch separately; arguments do not confer that lease. Native power
 * token is validated/pinned before installation, not used to infer an epoch.
 * Private allocation does not issue a GET or lend any hardware address. */
int x9_vdec_dma_create(const struct x9_vdec_dma_request *, struct x9_vdec_dma **);
/* Wraps the actual platform lease/MMIO/stream-table/queue callbacks. Private
 * CD/tree accesses and cache maintenance use THIS object's known CPU maps and
 * independently recorded PAs. Unknown PAs remain the platform's responsibility.
 * Installer succeeds only on direct ACK/consumer and live translation checks.
 * Serializes the object; platform callbacks MUST NOT reenter this API.
 * Uncertain exposure retains its allocations AND the native parent-power
 * child pin; no blind discard/PUT. Only UNEXPOSED or proven RETIRED drops the
 * child. This never supplies missing queue/stream/endpoint ownership. */
int x9_vdec_dma_install(struct x9_vdec_dma *, const struct x9_mm_smmu_install_io *,
                         uint32_t deadline_ms);
int x9_vdec_dma_retire(struct x9_vdec_dma *, const struct x9_mm_smmu_install_io *,
                        uint32_t deadline_ms);
/* Copies only while the allocation is private/uninstalled. This stages the
 * initial BS/SHM contents without lending a CPU pointer or guessing PA/IOVA.
 * After installation, firmware sharing needs the VCP transport's explicit
 * cache/ownership protocol; these APIs cannot be used to bypass that protocol. */
int x9_vdec_dma_write_private(struct x9_vdec_dma *, uint32_t offset,
                               const uint8_t *, uint32_t bytes);
/* Native arena suballocation, with per-buffer CPU/cache ownership. Existing
 * <=16MiB arena/256-scatter bounds remain implementation limits, NOT device
 * capabilities. Up to 256 live slices, no physical-contiguity assumption.
 * Every slice owns complete 16K CPU pages (including zeroed padding), so PoC
 * maintenance cannot discard a neighbour's dirty bytes or a shared cacheline.
 * align must be a power of two, 16..16MiB; actual alignment is at least 16K.
 * Firmware SW padding (round_up(requested,128)+128) must be included in bytes
 * by its allocator BEFORE this call; protocol requested len stays separate.
 * Tokens are native boot-unique, never pointers or inferred device aliases.
 * Handles/results must be private stable host storage. No concurrent discard
 * or reuse of a handle's storage; the arena lock serializes actual operations.
 */
#define X9_VDEC_DMA_MAX_SLICES 256u
struct x9_vdec_dma_slice {
    uint64_t epoch, token;
    uint32_t bytes, capacity;
};
struct x9_vdec_dma_extent {
    uint64_t iova;
    uint32_t bytes, capacity;
};
enum x9_vdec_dma_slice_state {
    X9_VDEC_DMA_CPU_PRIVATE = 1,
    X9_VDEC_DMA_DEVICE,
    X9_VDEC_DMA_CPU_RETIRED
};
/* Allocates from this arena, not a new CD/STE or mapping. Zeroes the ENTIRE
 * rounded capacity before returning a handle. Allowed before install and
 * while installed, but never reuses an exposed slice. The legacy whole-raw
 * staging API and sliced mode are mutually exclusive for the arena lifetime.
 */
int x9_vdec_dma_slice_alloc(struct x9_vdec_dma *, uint64_t epoch,
    uint32_t bytes, uint32_t align, struct x9_vdec_dma_slice *);
int x9_vdec_dma_slice_write(struct x9_vdec_dma *, const struct x9_vdec_dma_slice *,
    uint32_t offset, const uint8_t *, uint32_t bytes);
int x9_vdec_dma_slice_read(struct x9_vdec_dma *, const struct x9_vdec_dma_slice *,
    uint32_t offset, uint8_t *, uint32_t bytes);
/* Only an INSTALLED arena: recheck backing/native power, flush each actual
 * PA to PoC, then mark DEVICE before exporting an IOVA. Caller must still
 * guard the current controller/firmware lease, insert its durable instance
 * record and publish via the real bus; this is NOT a hardware ACK/READY.
 * No CPU read/write/free of DEVICE slices, even when the send later fails.
 * There is deliberately no caller boolean/completion number that can return
 * them to CPU ownership. Currently only successful WHOLE-domain retirement
 * with the existing real drain/TLBI/SYNC contract reclaims DEVICE slices.
 * Per-buffer hardware retirement and simultaneous live SHM field access
 * require the later codec ownership protocol; do not bypass this gate.
 */
int x9_vdec_dma_slice_publish(struct x9_vdec_dma *, const struct x9_vdec_dma_slice *,
    struct x9_vdec_dma_extent *);
int x9_vdec_dma_slice_state(struct x9_vdec_dma *, const struct x9_vdec_dma_slice *,
    uint32_t *state);
/* Free only never-published CPU_PRIVATE or physically retired CPU_RETIRED.
 * A whole arena cannot be discarded while any slice handle remains live.
 * This is not c006 completion; uncertain DEVICE memory stays reserved. */
int x9_vdec_dma_slice_free(struct x9_vdec_dma *, struct x9_vdec_dma_slice *);
/* Frees only never-exposed PRIVATE or authoritatively RETIRED allocations.
 * INSTALLED/QUARANTINED remain wired. Invalid/NULL/double-discard is an error. */
int x9_vdec_dma_discard(struct x9_vdec_dma **);
#endif
#endif
