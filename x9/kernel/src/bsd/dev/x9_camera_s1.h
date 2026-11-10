/* Kernel-private normal-camera Stage-1 preparation. Not an ioctl or lease.
 * Geometry/attributes follow MT6993's ARM_64_LPAE_S1 path, not the 4K
 * UniXNU/SMMUv2 kit. The native SMMUv3 controller owns SID/ASID allocation,
 * publication, command queue/TLBI, DMA stop and worker drain separately.
 */
#ifndef X9_CAMERA_S1_H
#define X9_CAMERA_S1_H
#ifdef KERNEL
#include <stdint.h>

struct x9_camera_s1;
/* An export pins ALL allocations before returning any hardware address.
 * The CPU pointer and IOVA are deliberately distinct. This record proves
 * only a prepared software translation; it NEVER proves attach or DMA.
 * No map may be freed after export until native controller retirement is
 * implemented. There is intentionally no caller-supplied 'drained' bool.
 */
struct x9_camera_s1_export {
    const void *raw;
    uint64_t iova, root_pa, cd_pa;
    uint32_t bytes, mapped_bytes, pages, oas;
};
/* Thread context. Caller must serialize handle lifetime, including create
 * and discard, and pin the handle across each call. In-object operations
 * additionally use the component mutex. IDR5 is a capability input,
 * NOT authority to use the shared MM SMMU. Normal IAS34, 16K, OAS <=48 only.
 * IOVA must be reserved by the future controller before hardware attach.
 * *out must be NULL; rejection leaves it unchanged and does no MMIO.
 */
int x9_camera_s1_create(uint32_t idr5, uint64_t iova, uint32_t bytes,
    struct x9_camera_s1 **out);
/* Re-walk every PTE and every wired CPU mapping. Does not read RAW pixels,
 * attach a stream, consult a queue, touch MMIO or qualify a frame.
 */
int x9_camera_s1_validate(struct x9_camera_s1 *);
/* Build one SSID0 AArch64 context descriptor for controller-reserved ASID
 * 1..255, clean tables to PoC and irrevocably pin them. Stall/ATS/dirty
 * update disabled; no HVC donation, fake lease or SID89 substitution.
 * Export is once-only; no repeated publication/rebuild after uncertainty.
 */
int x9_camera_s1_export(struct x9_camera_s1 *, uint32_t asid,
    struct x9_camera_s1_export *);
/* Only unpublished maps can be discarded. EBUSY after export, including
 * failed attach. The pointer stays valid on failure; no retry/unmap/free
 * based on an adapter return code. Successful discard sets *map=NULL.
 */
int x9_camera_s1_discard(struct x9_camera_s1 **map);
#endif
#endif
