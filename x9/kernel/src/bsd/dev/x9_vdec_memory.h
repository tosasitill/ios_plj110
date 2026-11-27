/* Native per-instance memory ledger for rv_v2 reverse allocation requests. */
#ifndef X9_VDEC_MEMORY_H
#define X9_VDEC_MEMORY_H
#ifdef KERNEL
#include "x9_vdec_dma.h"
#include "x9_vcp_wire.h"
struct x9_vdec_memory;
/* Retain the two same-epoch/same-parent arenas, HFRP SIDa0 and codec SIDb0.
 * Assign a boot-unique AP token and return the INIT request to use on the bus.
 * Arenas may still be private here; actual allocation replies require them
 * INSTALLED. Does not initialize firmware, send INIT or register a decoder.
 * Caller serializes handle lifetime; outputs are disjoint host storage. */
int x9_vdec_memory_create(struct x9_vdec_dma *hfrp, struct x9_vdec_dma *codec,
    uint32_t context_id, struct x9_vdec_memory **, struct x9_vcp_init *);
/* Signature matches bus reverse_ops.alloc. This is a real native allocator:
 * validate the instance/request, allocate a slice from its installed arena,
 * insert a durable record BEFORE PoC handoff, then return the owned IOVA.
 * Clear SW/type0 uses HFRP and includes stock round_up(len,128)+128 padding;
 * HW/type1 and HW_CACHE/type2 use codec. Heap cache labels do not bypass the
 * native CPU/DEVICE cache ownership protocol. UBE/type3 needs a CURRENT route
 * binding and is deliberately not inferred from an Android io-device number.
 * type8 initial SHM is a different owner/ABI, never an instance allocation.
 * Exhausted bounded storage returns an explicit Linux -ENOMEM wire result
 * (errno return0, all addresses zero), NOT an untracked successful allocation.
 * Other failures retain any partial record for teardown and return errno;
 * a failure after allocation latches the first error and blocks further alloc.
 * Missing live domain authority cannot be manufactured here: owner must guard
 * mappings/firmware and stop/drain externally. No success stub or auto-binding.
 */
int x9_vdec_memory_alloc(void *, uint64_t epoch, uint64_t deadline_ns,
    const struct x9_vcp_mem_request *, struct x9_vcp_mem_allocation *);
/* Exact c006 record lookup only; does NOT free/ACK or grant DMA retirement. */
int x9_vdec_memory_match_free(struct x9_vdec_memory *,
    const struct x9_vcp_mem_request *, struct x9_vdec_dma_slice *);
/* Drops references only after all private or whole-domain-retired slices can
 * be freed. DEVICE/quarantined slices keep the ledger and BOTH arenas alive.
 * A partially completed discard remains closing: no new alloc/lookup. This
 * cannot substitute for live c006 retirement or a firmware DEINIT reply. */
int x9_vdec_memory_discard(struct x9_vdec_memory **);
#endif
#endif
