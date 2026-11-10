/* Native core-A IPI_DMA table/region owner for MTK SmartPA SET/GET_BUF. */
#ifndef X9_IPI_DMA_H
#define X9_IPI_DMA_H
#include <stdint.h>
#include "x9_audio_dsp.h"
#define X9_IPI_DMA_HEADER_BYTES 128u
#define X9_IPI_DMA_REGION_BYTES 384u
#define X9_IPI_DMA_ALIGNMENT 128u
#define X9_IPI_DMA_RING_BYTES 0x4000u
#define X9_IPI_DMA_READBACK_BYTES 0x4000u
#define X9_IPI_DMA_INIT 0xadd0u
#define X9_IPI_DMA_UPDATE 0xadd1u
#define X9_IPI_TFA_SET 0x204u
#define X9_IPI_TFA_GET 0x205u
#define X9_IPI_TFA_TYPE 3u
#if defined(KERNEL)
enum { X9IPIDMACold, X9IPIDMABound, X9IPIDMAPublished, X9IPIDMACleanup };
struct x9_ipi_dma_policy {
    uint32_t version,generation,task_count,controller_scene,task_scene,wire_packing;
    uint64_t ticket;
    void *context;
    /* Actual current firmware's TASK_SCENE_SIZE/controller/runtime route,
     * AP/DSP EMI access and whole IPI_A table/pool exclusion. The mapped
     * inner prefix includes an independent 16KiB GET writeback page after
     * the TX ring, distinct from table/TX cache lines, AUDIO_COMMON, IPI_B
     * and other clients. Prove GET can address this pool allocation and
     * DSP output is PoC-visible with this cache policy; no DSP dirty alias
     * can survive a successful GET. Ownership lasts through physical halt.
     * Prove the region-index handler ABI: legacy reserve dma_addr is opaque,
     * NOT a DSP-dereferenceable AP pointer or a replacement bus address.
     * Cold proves no firmware/foreign reader owns the old table; retain
     * physical ownership through faults/halt/unmap. Stage is our journal,
     * NOT proof for the supplier. No allocation/mutation/re-entry, including
     * transport/status, in this callback. No default implementation. */
    int (*validate_memory)(void *,uint64_t ticket,uint32_t generation,
        uint64_t pool_bus,uint32_t pool_bytes,uint32_t map_bytes,
        uint32_t task_count,uint32_t controller_scene,uint32_t task_scene,
        uint32_t wire_packing,uint32_t stage);
    /* Prove immutable canonical batch is trusted for THIS firmware/route,
     * full PA revisions/profile/protection/calibration state. A framing
     * pass, filename or old calibration is NOT permission. No mutation,
     * I2C/SMC, unmute or callback recursion. Called on private wire copy. */
    int (*validate_parameters)(void *,const void *,uint32_t bytes,uint32_t packing);
    /* Actual firmware reply semantics, not param1==0 assumed from Linux.
     * INIT DIRECT has no reply; UPDATE, SET and GET require validation.
     * GET must prove actual write completion and NO retained/late writer
     * before the WB page may be copied/reused. Matching ACK/echo alone is
     * insufficient. Reply is private bytes, never a dereferenceable address. */
    int (*validate_reply)(void *,const struct x9_dsp_message *request,
                           const struct x9_dsp_message *reply);
    /* Authorize THIS pending firmware read-command/route and capacity;
     * GET carries no selector of its own. Prove any preceding command was
     * accepted, no competing mailbox client can change it, and reading is
     * allowed by full PA profile/protection state. No mutation, I2C/SMC,
     * callback recursion or unmute. Also called UNDER transport mutex;
     * never call transport/status or acquire this component's lock. Not a
     * userspace/raw-read permission. Remains valid through GET completion. */
    int (*validate_readback)(void *,uint32_t capacity);
};
#define X9_IPI_DMA_POLICY_VERSION 2u
#ifdef __cplusplus
extern "C" {
#endif
/* Singleton; same real Speaker ticket/READY generation. One acquire/register
 * attempt per generation. Map only current handoff IPI_A header/table/TX
 * plus dedicated GET page as Normal-WB after child pin/physical authority.
 * No power/reset, firmware start, backend/Ready registration or unmute. */
int x9_ipi_dma_acquire(const struct x9_ipi_dma_policy *);
int x9_ipi_dma_register(uint64_t ticket,uint32_t generation);
/* Kernel immutable/pinned canonical multimessage only. Convert into private
 * wire packing, validate trusted parameters, clean data BEFORE publishing
 * 128B-aligned write index, then guarded DMA SET/ACK and actual DSP read index.
 * No pipelining. Any publication/send/ACK/consume ambiguity pins the region;
 * never retry/retract indices/free memory or accept a late ACK as recovery. */
int x9_ipi_dma_set(uint64_t ticket,uint32_t generation,const void *,
                    uint32_t bytes,uint32_t source_packing);
/* Kernel-owned writable/pinned output only, capacity<=dedicated WB page.
 * NEED_ACK PAYLOAD16 GET, not a DMA union or TX-ring read. Exact address /
 * memory_size / data_size echo checks, real completion supplier, IVAC, then
 * private copy and final fresh authority before publishing output/size.
 * No pipelining or new request after ambiguity. Output/size unchanged on
 * failure; scalar/output alias and physical aliases to IPI_A are rejected.
 * Zero-size success is not calibration/Ready/unmute evidence. */
int x9_ipi_dma_get(uint64_t ticket,uint32_t generation,void *,uint32_t capacity,
                    uint32_t *data_size);
/* Coordinator first mutes/drains AFE and actually halts the core. Release
 * proves same-generation physical halt again; no online-only admission or
 * historical-error gate on safe unmap/drop. No mailbox free/replay/reset.
 * Board/EMI parents and original ticket outlive this final retirement. */
int x9_ipi_dma_release(uint64_t ticket,uint32_t generation);
#ifdef __cplusplus
}
#endif
#endif
#endif
