/* Bounded PoC operations for validated, wired camera backing only.
 * The frozen SWEET caches.o may use coherent-IO no-op clean/flush wrappers;
 * do not depend on those wrappers for MT6993's NC SMMU/CAMSV transactions.
 * VA is the checked mapping of ONE physical page, never raw_pa[0]+length.
 * No whole-cache operation, pixel access, allocation or ownership grant.
 */
#include <kern/cpu_data.h>
enum { CAM_CACHE_CLEAN, CAM_CACHE_DEVICE, CAM_CACHE_CPU };

static int cam_cache_range(vm_offset_t va,uint32_t bytes,unsigned operation)
{
    uint64_t ctr;
    vm_offset_t line,end;
    if(!va || !bytes || bytes>CAM_S1_PAGE || operation>CAM_CACHE_CPU ||
        va>~(vm_offset_t)0-bytes ||
        bytes>CAM_S1_PAGE-(va&(CAM_S1_PAGE-1)))return EINVAL;
    /* DminLine belongs to the CPU executing the cache instructions, not
     * to the camera allocation or its mutex. Migration to a CPU with a
     * smaller line could otherwise skip dirty/stale lines. Pin ONLY this
     * bounded single-page instruction span; no owner callback, VM lookup,
     * MMIO, allocation or sleep runs with preemption disabled. */
    disable_preemption();
    __asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
    line=(vm_offset_t)4<<((ctr>>16)&15u);
    if(line>CAM_S1_PAGE){enable_preemption();return ENOTSUP;}
    /* Invalidate ONLY complete lines owned by this allocation. Partial-line
     * IVAC/CIVAC must not discard an adjacent CPU writer's dirty bytes. */
    if(operation!=CAM_CACHE_CLEAN && ((va|bytes)&(line-1))) {
        enable_preemption();return EINVAL;
    }
    end=va+bytes;
    __asm__ volatile("dsb sy":::"memory");
    for(vm_offset_t p=va&~(line-1);p<end;p+=line) {
        if(operation==CAM_CACHE_CLEAN)
            __asm__ volatile("dc cvac, %0"::"r"(p):"memory");
        else if(operation==CAM_CACHE_DEVICE)
            __asm__ volatile("dc civac, %0"::"r"(p):"memory");
        else
            /* Never clean stale CPU data OVER a DMA-written image. Device
             * publication already removed dirty lines before DMA started. */
            __asm__ volatile("dc ivac, %0"::"r"(p):"memory");
    }
    __asm__ volatile("dsb sy":::"memory");
    enable_preemption();
    return 0;
}
