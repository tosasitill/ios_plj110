/* Native global bootstrap storage and the actual service type8 reply owner. */
#ifndef X9_VDEC_SHARED_H
#define X9_VDEC_SHARED_H
#ifdef KERNEL
#include "x9_vdec_memory.h"
struct x9_vdec_shared;
/* Must precede arena install. Reads the CURRENT ADT handoff, allocates one
 * zeroed slice in the same HFRP arena used by instance SW memory, initializes
 * paired cold common+logger headers. Retains an arena ref; once per arena.
 * Other global blocks stay reserved/zeroed, not lent to another subsystem.
 * Caller must establish actual firmware pairing and cold ownership before
 * publish/start. Layout profile alone is NOT that proof or an owner callback.
 * No IOVA export, bus/SMC call or decoder registration in this function.
 * Caller serializes all handle lifetimes; output storage is disjoint. */
/* Also owns BOTH bitdepth groups of VP auxiliary storage in the same-epoch
 * codec arena. The CURRENT handoff supplies the multiplier. These are not
 * decoded NV12/P010 images. No lazy writes to live common cache lines. */
int x9_vdec_shared_create(struct x9_vdec_dma *hfrp, struct x9_vdec_dma *codec,
    struct x9_vdec_shared **);
/* Requires installed native arena, syncs ALL allocated pages to PoC, marks
 * DEVICE before returning the actual global/shared and single logger extents
 * for x9_vcp_bus_bind. A publication failure retains the object for teardown.
 * Publish all six initialized VP buffers first, then their actual IOVAs and
 * flags in still-CPU-private common, then flush the ENTIRE global allocation.
 * Any partial exposure retains both arenas. Cannot reset live common. */
int x9_vdec_shared_publish(struct x9_vdec_shared *,
    struct x9_vcp_shm_region *shared, struct x9_vcp_shm_region *logger);
/* Signature matches reverse_ops.shm. Responds to the one sentinel type8
 * request with the actual VDEC/prop/log regions of this published global
 * allocation, not a fresh instance allocation or CPU physical address.
 * Checks native power/domain/slice state and deadline. Once offered, even a
 * failed send does not allow a retry/reset/free. Physical d005 consumption is
 * journalled separately by the bus, NOT inferred by this callback. */
int x9_vdec_shared_reply(void *, uint64_t epoch, uint64_t deadline_ns,
    const struct x9_vcp_shm_request *, struct x9_vcp_shm_mapping *);
/* Private or authoritatively whole-domain-retired only. DEVICE/uncertain
 * slices retain both storage and arena claim/ref. No implicit stop/drain. */
int x9_vdec_shared_discard(struct x9_vdec_shared **);
#endif
#endif
