/* Native permanently-owned DRAM ring for VUL1. Included in the engine object;
 * no userspace mapping, hard-coded SRAM reservation or source-only bus alias.
 * The current LK FDT supplies direct-bus topology; the board supplies source
 * qualification/drain. This layer supplies allocation, stable page ownership,
 * checked bus translation, cache synchronization and bounded mono copies.
 */
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#include <kern/cpu_data.h>
#include <pexpert/device_tree.h>
#include "x9_mic_dma_handoff.h"
_Static_assert(sizeof(struct x9_mic_dma_handoff)==X9_MIC_DMA_HANDOFF_BYTES,"DMA handoff ABI");
/* Frozen BSD export headers omit these internal pmap declarations. Same
 * signatures as native UFS and vm/pmap.h, not hand-written DMA substitutes. */
extern ppnum_t pmap_find_phys(pmap_t,addr64_t);
extern unsigned int pmap_cache_attributes(ppnum_t);
LCK_GRP_DECLARE(mic_dma_memory_group,"x9-mic-dma-memory");
LCK_MTX_DECLARE(mic_dma_memory_lock,&mic_dma_memory_group);
static struct {
    struct x9_mic_plj110_native_dma platform;
    struct x9_mic_dma_handoff topology;
    vm_offset_t address;
    uint64_t physical,bus;
    uint32_t bytes;
    bool attempted,mapped,bound;
    int error;
} mic_memory;

/* Called under the memory lock, never accesses the AFE registers. Empty,
 * foreign or old ADT contracts fail closed; this is not READY or drain. */
static int mic_memory_topology(struct x9_mic_dma_handoff *out)
{
    DTEntry chosen;const void *p=NULL;unsigned int n=0;
    if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess ||
       SecureDTGetProperty(chosen,"x9-mic-dma-handoff",&p,&n)!=kSuccess ||
       !p || n!=sizeof(*out))return ENXIO;
    memcpy(out,p,sizeof(*out));
    if(out->version!=1 || out->flags!=X9_MIC_DMA_HANDOFF_FLAGS ||
       out->cpu_start || out->dma_start || out->window_bytes!=0x1000000000ULL ||
       out->afe_base!=0x1a110000 || out->afe_bytes!=0x10000)return ENOTSUP;
    for(unsigned i=0;i<4;i++)if(out->reserved[i])return EINVAL;
    return 0;
}
static int mic_memory_bus_address(const struct x9_mic_dma_handoff *h,
                                 uint64_t physical,uint32_t bytes,uint64_t *out)
{
    if(!out || !physical || (physical&PAGE_MASK) || bytes<65536 || bytes>131072 ||
       (bytes&PAGE_MASK) || physical<h->cpu_start)return EINVAL;
    uint64_t off=physical-h->cpu_start;
    if(off>=h->window_bytes || bytes>h->window_bytes-off ||
       h->dma_start>UINT64_MAX-off)return EINVAL;
    uint64_t bus=h->dma_start+off;
    /* OPPO mt6993_afe_pcm_dev_probe: dma_set_mask_and_coherent(34).
     * The 64GiB bus range is not the device's 16GiB addressing capability. */
    if(!bus || (bus&15) || bus>(1ULL<<34)-bytes)return EINVAL;
    *out=bus;return 0;
}
static int mic_memory_afe_mapping(void)
{
    uintptr_t va=(uintptr_t)mic_memory.platform.afe;
    if(!va || (va&PAGE_MASK))return EINVAL;
    /* Require the entire actual 64KiB register window, without dereferencing
     * a potentially unpowered AFE while registering/validating the mapping. */
    for(uint32_t off=0;off<mic_memory.topology.afe_bytes;off+=PAGE_SIZE) {
        ppnum_t p=pmap_find_phys(kernel_pmap,(addr64_t)(va+off));
        if(!p || ptoa_64(p)!=mic_memory.topology.afe_base+off)return EIO;
    }
    return 0;
}
static int mic_memory_bus_validate(void)
{
    struct x9_mic_dma_handoff current;uint64_t bus=0;
    int e=mic_memory_topology(&current);if(e)return e;
    if(memcmp(&current,&mic_memory.topology,sizeof(current)))return EBUSY;
    e=mic_memory_bus_address(&current,mic_memory.physical,mic_memory.bytes,&bus);
    if(!e && bus!=mic_memory.bus)e=EIO;
    if(!e)e=mic_memory_afe_mapping();
    return e;
}

static int mic_memory_pages(void)
{
    if(!mic_memory.address || !mic_memory.physical)return ENXIO;
    for(uint32_t off=0;off<mic_memory.bytes;off+=PAGE_SIZE) {
        ppnum_t p=pmap_find_phys(kernel_pmap,(addr64_t)(mic_memory.address+off));
        if(!p || ptoa_64(p)!=mic_memory.physical+off ||
           (pmap_cache_attributes(p)&VM_WIMG_MASK)!=VM_WIMG_DEFAULT)return EIO;
    }
    return 0;
}
static int mic_memory_validate(void)
{
    if(mic_memory.error)return mic_memory.error;
    if(!mic_memory.mapped)return ENXIO;
    int e=mic_memory_pages();if(e)return e;
    return mic_memory_bus_validate();
}
/* No CPU stores after the initial zeroed allocation was cleaned to PoC.
 * Receive-side synchronization therefore INVALIDATES, never cleans stale CPU
 * bytes over DMA output. Round to entire local cache lines, wholly inside
 * driver-owned pages; do not apply cache operations to device-mapped SRAM.
 */
static int mic_memory_cache(uint32_t offset,uint32_t bytes,bool initial)
{
    if(offset>mic_memory.bytes || !bytes || bytes>mic_memory.bytes-offset)return EINVAL;
    uint64_t ctr;__asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));
    uintptr_t line=(uintptr_t)4<<((ctr>>16)&15);
    if(line<16 || line>PAGE_SIZE)return ENOTSUP;
    uintptr_t first=(mic_memory.address+offset)&~(line-1),end=mic_memory.address+offset+bytes;
    disable_preemption();
    __asm__ volatile("dsb sy":::"memory");
    for(uintptr_t at=first;at<end;at+=line) {
        if(initial)__asm__ volatile("dc civac,%0"::"r"(at):"memory");
        else __asm__ volatile("dc ivac,%0"::"r"(at):"memory");
    }
    __asm__ volatile("dsb sy":::"memory");
    enable_preemption();return 0;
}
/* Called only by the AFE engine while its actual domain/clock lease is held.
 * Validate address registers as well as VUL1 format; do not treat a coincident
 * CUR value from another memif owner as permission to read our buffer.
 */
static int mic_memory_cursor(uint32_t *offset)
{
    if((rd(VUL1_CON0)&(VUL1_CAPTURE_MASK|VUL1_ON|(1u<<7)))!=(VUL1_CAPTURE_CONFIG|VUL1_ON) ||
       (rd(VUL1_MON0)&VUL1_MON_FULL))return EIO;
    uint64_t end=mic_memory.bus+mic_memory.bytes-1;
    if((rd(VUL1_BASE_MSB)&0x1ff)!=(mic_memory.bus>>32) ||
       (rd(VUL1_BASE)&0xfffffff0)!=(uint32_t)mic_memory.bus ||
       (rd(VUL1_END_MSB)&0x1ff)!=(end>>32) ||
       (rd(VUL1_END)&0xfffffff0)!=((uint32_t)end&0xfffffff0))return EBUSY;
    for(unsigned i=0;i<3;i++) {
        uint32_t hi=rd(VUL1_CUR_MSB),lo=rd(VUL1_CUR);
        if(hi!=rd(VUL1_CUR_MSB))continue;
        if(hi&~0x1ffu)return EIO;
        uint64_t v=((uint64_t)hi<<32)|lo;
        if(v<mic_memory.bus || v>=mic_memory.bus+mic_memory.bytes || (v&3))return EIO;
        *offset=(uint32_t)(v-mic_memory.bus);return 0;
    }
    return EAGAIN;
}
static int mic_memory_sync_device(void)
{
    lck_mtx_lock(&mic_dma_memory_lock);int e=mic_memory_validate();
    if(!e)e=mic_memory_cache(0,mic_memory.bytes,false);
    if(!e)e=mic_memory_validate();
    if(e && !mic_memory.error)mic_memory.error=e;
    lck_mtx_unlock(&mic_dma_memory_lock);return e;
}
static int mic_memory_copy(uint32_t offset,int16_t *mono,uint32_t frames,uint32_t channel)
{
    /* Engine chunks never wrap, and no more than 256 stereo words are read
     * while checking that the producer has not lapped this copy. No zeros,
     * averaging, test patterns or speculative data are delivered on failure. */
    if(!mono || !frames || frames>256 || channel!=0 || (offset&3))return EINVAL;
    uint32_t bytes=frames*4;
    lck_mtx_lock(&mic_dma_memory_lock);
    int e=!mic_memory.bound?ENXIO:mic_memory_validate();uint32_t before=0,after=0;
    uint64_t start=mach_absolute_time();
    if(!e && (offset>mic_memory.bytes || bytes>mic_memory.bytes-offset))e=EINVAL;
    if(!e)e=mic_memory_cursor(&before);
    if(!e) {
        uint32_t available=(before+mic_memory.bytes-offset)%mic_memory.bytes;
        if(available<bytes || available>mic_memory.bytes/2)e=EOVERFLOW;
    }
    if(!e)e=mic_memory_cache(offset,bytes,false);
    if(!e) {
        volatile const uint32_t *pcm=(volatile const uint32_t *)(mic_memory.address+offset);
        for(uint32_t i=0;i<frames;i++)mono[i]=(int16_t)(uint16_t)pcm[i];
        __asm__ volatile("dmb oshld":::"memory");
        e=mic_memory_cursor(&after);
    }
    if(!e) {
        uint64_t elapsed;absolutetime_to_nanoseconds(mach_absolute_time()-start,&elapsed);
        uint32_t progress=(after+mic_memory.bytes-before)%mic_memory.bytes;
        uint32_t until_overlap=(offset+mic_memory.bytes-before)%mic_memory.bytes;
        /* 5ms is a conservative copy deadline, not a hardware frame counter.
         * Combined with checked 48k format and >=341ms ring, late copies fail
         * rather than pretending a modulo cursor proves no discontinuity. */
        if(elapsed>=5000000 || progress>mic_memory.bytes/2 ||
           (progress && progress>=until_overlap))e=EOVERFLOW;
    }
    if(!e)e=mic_memory_validate();
    if(e) {
        bzero(mono,frames*sizeof(*mono));
        if(e!=EAGAIN && !mic_memory.error)mic_memory.error=e;
    }
    lck_mtx_unlock(&mic_dma_memory_lock);return e;
}
static int mic_memory_quiesce(void)
{
    lck_mtx_lock(&mic_dma_memory_lock);
    /* A prior copy error must NOT skip the board's actual drain operation.
     * Retain the mapping/pages even when either validation or drain fails. */
    int e=!mic_memory.mapped?ENXIO:mic_memory_bus_validate();
    if(!e)e=mic_memory.platform.quiesce(mic_memory.platform.context);
    if(!e)e=mic_memory_bus_validate();
    if(e && !mic_memory.error)mic_memory.error=e;
    lck_mtx_unlock(&mic_dma_memory_lock);return e;
}
int x9_mic_plj110_register_native_dma(const struct x9_mic_plj110_native_dma *p)
{
    if(!p || p->version!=2 || p->flags!=(X9_MIC_PLJ110_AIN0_VERIFIED|X9_MIC_PLJ110_DMA_DRAIN_VERIFIED) ||
       !p->afe || ((uintptr_t)p->afe&PAGE_MASK) || !p->quiesce ||
       p->ring_bytes<65536 || p->ring_bytes>131072 || (p->ring_bytes&PAGE_MASK))return EINVAL;
    lck_mtx_lock(&afe_mic_lock);
    bool busy=registered || prepared || active || dma_stuck ||
        (board_diagnostic.flags&X9_MIC_BOARD_ATTEMPTED);
    lck_mtx_unlock(&afe_mic_lock);if(busy)return EBUSY;
    lck_mtx_lock(&mic_dma_memory_lock);
    if(mic_memory.attempted){lck_mtx_unlock(&mic_dma_memory_lock);return EALREADY;}
    mic_memory.attempted=true;mic_memory.platform=*p;mic_memory.bytes=p->ring_bytes;
    int e=mic_memory_topology(&mic_memory.topology);
    if(!e)e=mic_memory_afe_mapping();
    /* OPPO AFE device's coherent DMA mask is 34 bits. This bounds allocation;
     * it does NOT equate the allocated CPU PA with an AFE bus address. */
    ppnum_t maximum=(ppnum_t)(((1ULL<<34)-1)>>PAGE_SHIFT);
    if(!e)e=kmem_alloc_contig(kernel_map,&mic_memory.address,p->ring_bytes,PAGE_MASK,maximum,0,
        KMA_DATA|KMA_ZERO|KMA_PERMANENT,VM_KERN_MEMORY_IOKIT)==KERN_SUCCESS?0:ENOMEM;
    if(!e)mic_memory.physical=ptoa_64(pmap_find_phys(kernel_pmap,(addr64_t)mic_memory.address));
    if(!e)e=mic_memory_pages();
    if(!e)e=mic_memory_cache(0,p->ring_bytes,true);
    if(!e)e=mic_memory_bus_address(&mic_memory.topology,mic_memory.physical,p->ring_bytes,&mic_memory.bus);
    if(!e) {
        mic_memory.mapped=true;
        e=mic_memory_bus_validate();
        if(!e)e=mic_memory_pages();
    }
    if(e)mic_memory.error=e;
    struct x9_mic_plj110_dma binding={.version=2,
        .flags=X9_MIC_PLJ110_DMA_OWNED|X9_MIC_PLJ110_DMA_MAPPED|p->flags,
        .afe=p->afe,.ring=(void *)mic_memory.address,.bus_address=mic_memory.bus,
        .ring_bytes=p->ring_bytes,.adc_channel=0,.memory_type=X9_MIC_DMA_MEMORY_DRAM,
        .quiesce=mic_memory_quiesce,.copy_frames=mic_memory_copy,
        .sync_for_device=mic_memory_sync_device};
    /* Never hold memory lock while entering AFE registration: active worker
     * copies have the opposite AFE -> memory order. Pages/context are already
     * stable and retained even if a competing provider won registration. */
    if(!e)mic_memory.bound=true; /* before registration can publish READY */
    lck_mtx_unlock(&mic_dma_memory_lock);
    if(!e)e=x9_mic_plj110_register_dma(&binding);
    lck_mtx_lock(&mic_dma_memory_lock);
    if(e){mic_memory.bound=false;if(!mic_memory.error)mic_memory.error=e;}
    lck_mtx_unlock(&mic_dma_memory_lock);
    printf("X9 MIC DMA memory: owned=%u mapped=%u bound=%u bytes=%u errno=%d (mapping/drain qualification separate)\n",
        mic_memory.address!=0,mic_memory.mapped,mic_memory.bound,mic_memory.bytes,e);
    return e;
}
