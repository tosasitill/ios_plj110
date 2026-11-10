/* OPPO genpool/core_flag ABI, XNU Normal-WB mapping and explicit PoC sync.
 * Neither Device DRAM nor a WC alias over an existing WB physmap is legal.
 * No captured physical address, default EMI region/domain or scene guessing. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/locks.h>
#include <kern/cpu_data.h>
#include <vm/pmap.h>
#include <arm/machine_routines.h>
#include <arm/memory_types.h>
#include "x9_dsp_shared.h"
#include "x9_adsp_mt6993.h"
#include "x9_audio_resources.h"
#include "x9_speaker_memif.h"
/* Frozen BSD exports omit these native declarations, as for UFS/MIC.
 * Signatures from arm/pmap.h, vm/pmap.h and arm/machine_routines.h. */
extern ppnum_t pmap_find_phys(pmap_t,addr64_t);
extern unsigned int pmap_cache_attributes(ppnum_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,unsigned int);
extern void ml_io_unmap(vm_offset_t,vm_size_t);
LCK_GRP_DECLARE(shared_group,"x9-dsp-shared");
LCK_MTX_DECLARE(shared_lock,&shared_group);
enum { SharedCore,SharedATOD,SharedDTOA,SharedInput,SharedAFE,SharedCount };
_Static_assert(X9_DSP_SHARED_CORE_BYTES==16 && X9_DSP_SHARED_CORE_ALLOC==0x100,"core ABI/allocation");
_Static_assert(X9_DSP_SHARED_TASK_BYTES==0x400 && X9_DSP_PCM_RECORD_BYTES<=0x400,"task records");
_Static_assert(X9_DSP_SHARED_RING_BYTES==64*480*4,"whole PCM periods");
_Static_assert(0x4000+X9_DSP_SHARED_RING_BYTES<=0x24000,"input does not overlap AFE");
_Static_assert(0x24000+X9_DSP_SHARED_RING_BYTES<=X9_DSP_SHARED_MAP_BYTES,"AFE inside map");
static struct {
    struct x9_dsp_shared_policy policy;
    struct x9_dsp_pcm_region region[SharedCount];
    uint64_t pool_bus,map_bus,sem_lease,child_lease;
    uint32_t pool_bytes,last_generation,error;
    vm_offset_t map;
    bool attempted,mapped,registered,register_attempted;
} shared;
static void put64(uint8_t *p,uint64_t v)
{for(unsigned n=0;n<8;n++)p[n]=(uint8_t)(v>>(8*n));}
static uint64_t get64(const uint8_t *p)
{uint64_t v=0;for(unsigned n=0;n<8;n++)v|=(uint64_t)p[n]<<(8*n);return v;}
static int fault(int e)
{
    if(e==EAGAIN)e=EIO;
    if(!shared.error)shared.error=(uint32_t)e;
    x9_audio_resources_fault(X9AudioResourceSpeaker,shared.policy.ticket,e);
    return e;
}
static int authority(bool online)
{
    int e=x9_adsp_mt6993_speaker_validate(shared.policy.ticket);
    if(!e)e=shared.policy.validate_memory(shared.policy.context,
        shared.policy.ticket,shared.policy.generation,shared.policy.task_scene,
        shared.policy.daemon_scene,shared.pool_bus,shared.pool_bytes,
        shared.map_bus,X9_DSP_SHARED_MAP_BYTES,shared.policy.afe_memif);
    X9AudioDSPStatus ds={0};
    if(!e)e=x9_adsp_status(0,&ds);
    if(!e && (ds.generation!=shared.policy.generation ||
       ds.state!=(online?X9DSPOnline:X9DSPOffline) || (online && ds.error)))e=ESTALE;
    return e;
}
/* io_map's Normal-WB flag must match pmap metadata AND any existing physmap.
 * pmap_valid_address only checks the enclosing physical aperture: it does
 * NOT prove allocator exclusion, EMI, ownership or even an actual mapping. */
static int cache_binding(bool mapped)
{
    if(PAGE_SIZE!=0x4000)return ENOTSUP;
    for(uint32_t off=0;off<X9_DSP_SHARED_MAP_BYTES;off+=0x4000) {
        uint64_t bus=shared.map_bus+off;
        ppnum_t pn=(ppnum_t)(bus>>PAGE_SHIFT);
        if((pmap_cache_attributes(pn)&VM_WIMG_MASK)!=VM_WIMG_COPYBACK)return ENOTSUP;
        vm_offset_t alias=ml_static_ptovirt((vm_offset_t)bus);
        ppnum_t ap=pmap_find_phys(kernel_pmap,alias);
        /* A missing alias is acceptable; a present one must name this page.
         * Cache metadata above rules out changing a pre-existing WC page. */
        if(ap && ap!=pn)return EFAULT;
        if(mapped && pmap_find_phys(kernel_pmap,shared.map+off)!=pn)return EFAULT;
    }
    return 0;
}
static int qualify(void)
{
    if(!shared.mapped)return ENXIO;
    if(shared.error)return (int)shared.error;
    int e=authority(true);
    if(!e)e=cache_binding(true);
    return e?fault(e):0;
}
/* Dedicated 128B boundaries prevent an IVAC of DTOA/core from touching an
 * AP-dirty ATOD/ring line. CPU sync deliberately NEVER cleans stale DSP
 * data back over a firmware write. Backend serializes AP writers/readers. */
static int sync_region(unsigned region,uint32_t offset,uint32_t bytes,bool device)
{
    if(region>=SharedCount || !bytes || offset>shared.region[region].bytes ||
       bytes>shared.region[region].bytes-offset)return EINVAL;
    int e=qualify();if(e)return e;
    /* Read CTR AFTER pinning this CPU. Heterogeneous CPUs need not share
     * DminLine, so a cache-line size saved at acquire would be incorrect. */
    disable_preemption();
    uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
    uintptr_t line=(uintptr_t)4<<((ctr>>16)&15);
    if(line<16 || line>128){enable_preemption();return fault(ENOTSUP);}
    uintptr_t lo=(uintptr_t)shared.region[region].cpu+offset;
    uintptr_t hi=(lo+bytes+line-1)&~(line-1);
    lo&=~(line-1);
    __asm__ volatile("dsb sy":::"memory");
    for(uintptr_t at=lo;at<hi;at+=line) {
        if(device)__asm__ volatile("dc cvac, %0"::"r"(at):"memory");
        else __asm__ volatile("dc ivac, %0"::"r"(at):"memory");
    }
    __asm__ volatile("dsb sy":::"memory");
    enable_preemption();
    return qualify();
}
static int pcm_validate(void *context)
{
    if(context!=&shared)return EPERM;
    lck_mtx_lock(&shared_lock);int e=shared.registered?qualify():ENXIO;
    lck_mtx_unlock(&shared_lock);return e;
}
static int pcm_validate_afe(void *context,const struct x9_dsp_pcm_buffer *buffer)
{
    if(context!=&shared || !buffer)return EINVAL;
    /* Never let an otherwise valid input binding publish a different DMA
     * allocation. Snapshot the caller's descriptor before supplier calls. */
    const struct x9_dsp_pcm_buffer b=*buffer;
    lck_mtx_lock(&shared_lock);int e=shared.registered?qualify():ENXIO;
    const struct x9_dsp_pcm_region *r=&shared.region[SharedAFE];
    if(!e && (b.base!=r->bus || b.bytes!=r->bytes || b.hardware!=1 ||
       b.memif!=shared.policy.afe_memif ||
       b.memory!=1 || b.format!=2 ||
       b.rate!=48000 || b.channels!=2 || b.direction!=1 ||
       (b.read&3) || (b.write&3) || (b.count&3) || b.count>b.bytes ||
       !b.period_frames || !b.periods ||
       (uint64_t)b.period_frames*b.periods!=b.bytes/4 ||
       b.read<b.base || b.read>=b.base+b.bytes ||
       b.write<b.base || b.write>=b.base+b.bytes))e=EINVAL;
    if(!e)e=shared.policy.validate_afe_route(shared.policy.context,
        shared.policy.ticket,shared.policy.generation,shared.policy.task_scene,&b);
    if(!e)e=qualify();
    /* EAGAIN is an authority failure, not a DSP-consumption cursor retry. */
    if(e)e=fault(e);
    lck_mtx_unlock(&shared_lock);return e;
}
static int pcm_sync(void *context,unsigned region,uint32_t offset,uint32_t bytes,bool device)
{
    if(context!=&shared || region>=X9PCMRegionCount)return EINVAL;
    const unsigned translate[X9PCMRegionCount]={SharedATOD,SharedDTOA,SharedInput};
    lck_mtx_lock(&shared_lock);
    int e=shared.registered?sync_region(translate[region],offset,bytes,device):ENXIO;
    lck_mtx_unlock(&shared_lock);return e;
}
static int pcm_sync_device(void *c,unsigned r,uint32_t o,uint32_t n)
{return pcm_sync(c,r,o,n,true);}
static int pcm_sync_cpu(void *c,unsigned r,uint32_t o,uint32_t n)
{return pcm_sync(c,r,o,n,false);}
int x9_dsp_shared_acquire(const struct x9_dsp_shared_policy *p)
{
    if(!p || p->version!=X9_DSP_SHARED_POLICY_VERSION || !p->ticket ||
       !p->generation || !p->validate_memory || !p->validate_afe_route || !p->validate_drained ||
       p->reserved || (p->afe_memif!=X9_SPEAKER_DL24_MEMIF &&
                       p->afe_memif!=X9_SPEAKER_DL_24CH_MEMIF) ||
       (p->task_scene!=7 && p->task_scene!=32) || p->daemon_scene<=41 ||
       p->daemon_scene>=64)return EINVAL;
    lck_mtx_lock(&shared_lock);
    if(shared.attempted || shared.mapped || p->generation<=shared.last_generation) {
        lck_mtx_unlock(&shared_lock);return EBUSY;
    }
    struct x9_adsp_handoff h;int e=x9_adsp_mt6993_copy_handoff(&h);
    if(!e) {
        shared.policy=*p;shared.pool_bus=h.reserve_base+h.block[X9ADSPMemAudio].offset;
        shared.pool_bytes=h.block[X9ADSPMemAudio].bytes;
        shared.map_bus=(shared.pool_bus+0x3fff)&~0x3fffULL;
        if(shared.map_bus>shared.pool_bus+shared.pool_bytes ||
           X9_DSP_SHARED_MAP_BYTES>shared.pool_bus+shared.pool_bytes-shared.map_bus)e=ERANGE;
    }
    if(!e) {
        /* Once the authority is invoked, retain immutable attempt metadata
         * even on rejection; no same-generation initialization shortcut. */
        shared.attempted=true;shared.last_generation=p->generation;
        shared.error=0;shared.registered=shared.register_attempted=false;
        uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
        uint32_t line=4u<<((ctr>>16)&15);
        if(line<16 || line>128)e=ENOTSUP;
        if(!e)e=authority(true);
        if(!e)e=cache_binding(false);
        if(!e)e=x9_adsp_mt6993_shared_hold(p->ticket,&shared.child_lease);
        if(!e) {
            /* kernel_map, not the permanent 8MiB Device io_submap. Do not
             * map/zero whole AUDIO_COMMON or its XHCI-shared edge page. */
            shared.map=ml_io_map_unmappable((vm_offset_t)shared.map_bus,
                X9_DSP_SHARED_MAP_BYTES,VM_WIMG_COPYBACK);
            if(!shared.map)e=ENOMEM;
            else {
                shared.mapped=true;
                const uint32_t offsets[SharedCount]={0,0x100,0x500,0x4000,0x24000};
                const uint32_t sizes[SharedCount]={X9_DSP_SHARED_CORE_ALLOC,
                    X9_DSP_SHARED_TASK_BYTES,X9_DSP_SHARED_TASK_BYTES,
                    X9_DSP_SHARED_RING_BYTES,X9_DSP_SHARED_RING_BYTES};
                for(unsigned n=0;n<SharedCount;n++) {
                    shared.region[n].cpu=(void *)(shared.map+offsets[n]);
                    shared.region[n].bus=shared.map_bus+offsets[n];
                    shared.region[n].bytes=sizes[n];
                }
                e=qualify();
            }
        }
        if(e)e=fault(e); /* no unmap, vote withdrawal or implicit retry */
    }
    lck_mtx_unlock(&shared_lock);return e;
}
static int send_register(uint16_t id,const uint8_t *data,uint32_t bytes)
{
    int e=qualify();if(e)return e;
    struct x9_dsp_message m={0};m.scene=(uint8_t)shared.policy.daemon_scene;
    m.source=1;m.target=2;m.id=id;m.ack=X9DSPBypass;m.type=X9DSPPayload;
    m.param1=bytes;memcpy(m.payload,data,bytes);
    e=x9_adsp_request(0,&m,NULL,250);
    if(e)return fault(e);
    return qualify(); /* BYPASS is only TX-consume, not handler acceptance */
}
static int release_sem(void)
{
    if(!shared.sem_lease)return 0;
    int e=x9_adsp_mt6993_audio_sem_release(shared.policy.ticket,shared.sem_lease);
    if(!e)shared.sem_lease=0;
    return e;
}
int x9_dsp_shared_register(void)
{
    lck_mtx_lock(&shared_lock);int e=qualify();
    if(!e && shared.register_attempted)e=EBUSY;
    if(!e) {
        shared.register_attempted=true;
        e=x9_adsp_mt6993_audio_sem_acquire(shared.policy.ticket,&shared.sem_lease);
        if(!e)e=qualify();
        if(!e)e=x9_adsp_mt6993_audio_sem_validate(shared.policy.ticket,shared.sem_lease);
        if(!e) {
            memset(shared.region[SharedCore].cpu,0,X9_DSP_SHARED_CORE_ALLOC);
            e=sync_region(SharedCore,0,X9_DSP_SHARED_CORE_ALLOC,true);
        }
        int release=release_sem();if(!e)e=release;
        if(!e) {
            uint8_t pool[16],core[X9_DSP_DRAM_RECORD_BYTES];
            put64(pool,shared.pool_bus);put64(pool+8,shared.pool_bytes);
            e=send_register(8,pool,sizeof(pool));
            if(!e)e=x9_dsp_pcm_encode_dram(shared.region[SharedCore].bus,
                (uint64_t)(uintptr_t)shared.region[SharedCore].cpu,
                X9_DSP_SHARED_CORE_BYTES,core,sizeof(core));
            if(!e)e=send_register(7,core,sizeof(core));
        }
        if(e)e=fault(e);else shared.registered=true;
    }
    lck_mtx_unlock(&shared_lock);return e;
}
int x9_dsp_shared_pcm_binding(struct x9_dsp_pcm_binding *out)
{
    if(!out)return EINVAL;
    lck_mtx_lock(&shared_lock);int e=shared.registered?qualify():ENXIO;
    if(!e) {
        struct x9_dsp_pcm_binding b={.version=X9_DSP_PCM_BINDING_VERSION,.core=0,
            .generation=shared.policy.generation,.scene=shared.policy.task_scene,
            .control_timeout_ms=250,.copy_timeout_ms=10,.context=&shared,
            .validate=pcm_validate,.validate_afe=pcm_validate_afe,
            .sync_device=pcm_sync_device,.sync_cpu=pcm_sync_cpu};
        b.region[X9PCMRegionATOD]=shared.region[SharedATOD];
        b.region[X9PCMRegionDTOA]=shared.region[SharedDTOA];
        b.region[X9PCMRegionRing]=shared.region[SharedInput];*out=b;
    }
    lck_mtx_unlock(&shared_lock);return e;
}
int x9_dsp_shared_afe_region(struct x9_dsp_pcm_region *out)
{
    if(!out)return EINVAL;
    lck_mtx_lock(&shared_lock);int e=shared.registered?qualify():ENXIO;
    if(!e)*out=shared.region[SharedAFE];
    lck_mtx_unlock(&shared_lock);return e;
}
static int afe_sync(uint32_t offset,uint32_t bytes,bool device)
{
    lck_mtx_lock(&shared_lock);
    int e=shared.registered?sync_region(SharedAFE,offset,bytes,device):ENXIO;
    lck_mtx_unlock(&shared_lock);return e;
}
int x9_dsp_shared_afe_sync_device(uint32_t o,uint32_t n){return afe_sync(o,n,true);}
int x9_dsp_shared_afe_sync_cpu(uint32_t o,uint32_t n){return afe_sync(o,n,false);}
int x9_dsp_shared_irq_validate(struct x9_dsp_pcm_session *s,uint64_t ticket,uint32_t generation)
{
    if(!s)return EINVAL;
    lck_mtx_lock(&shared_lock);int e=shared.registered?qualify():ENXIO;
    unsigned needed=X9PCMPrepared|X9PCMAFEPrepared;
    if(!e && (!s->bound || s->error || (s->flags&needed)!=needed ||
       s->hw.context!=&shared || s->hw.core || shared.policy.ticket!=ticket ||
       s->hw.generation!=generation || shared.policy.generation!=generation ||
       s->hw.scene!=shared.policy.task_scene || s->hw.validate!=pcm_validate ||
       s->hw.validate_afe!=pcm_validate_afe))e=EPERM;
    lck_mtx_unlock(&shared_lock);
    if(!e)e=pcm_validate_afe(&shared,&s->afe_buffer);
    return e;
}
int x9_dsp_shared_dtoa(struct x9_dsp_pcm_session *s)
{
    if(!s)return EINVAL;
    lck_mtx_lock(&shared_lock);int e=shared.registered?qualify():ENXIO;
    if(!e && (!s->bound || s->hw.context!=&shared || s->hw.core ||
       s->hw.generation!=shared.policy.generation || s->hw.scene!=shared.policy.task_scene ||
       s->hw.region[X9PCMRegionDTOA].cpu!=shared.region[SharedDTOA].cpu ||
       s->hw.region[X9PCMRegionDTOA].bus!=shared.region[SharedDTOA].bus ||
       s->hw.region[X9PCMRegionDTOA].bytes!=X9_DSP_SHARED_TASK_BYTES))e=EPERM;
    bool notified=false;uint64_t value=0,bit=1ULL<<shared.policy.task_scene;
    bool attempted=!e;
    if(!e) {
        e=x9_adsp_mt6993_audio_sem_acquire(shared.policy.ticket,&shared.sem_lease);
        if(!e)e=sync_region(SharedCore,0,X9_DSP_SHARED_CORE_BYTES,false);
        if(!e)e=x9_adsp_mt6993_audio_sem_validate(shared.policy.ticket,shared.sem_lease);
        if(!e)e=qualify();
        if(!e) {
            uint8_t *flags=shared.region[SharedCore].cpu;
            value=get64(flags+8);
            /* Never clear another task, including a scene we do not route. */
            if(value&~bit)e=EPROTO;
            else notified=(value&bit)!=0;
        }
    }
    lck_mtx_unlock(&shared_lock);
    /* Hold the REAL semaphore across record observation, not this mutex:
     * PCM's fresh sync/validate callbacks take shared_lock themselves. */
    if(!e && notified)e=x9_dsp_pcm_observe(s);
    lck_mtx_lock(&shared_lock);
    if(attempted) {
        if(!e && notified) {
            e=qualify();
            if(!e)e=x9_adsp_mt6993_audio_sem_validate(shared.policy.ticket,shared.sem_lease);
            if(!e) {
                uint8_t *flags=shared.region[SharedCore].cpu;
                put64(flags+8,value&~bit);
                e=sync_region(SharedCore,0,X9_DSP_SHARED_CORE_BYTES,true);
                if(!e)e=x9_adsp_mt6993_audio_sem_validate(shared.policy.ticket,shared.sem_lease);
            }
        }
        int release=release_sem();if(!e)e=release;
        if(e)e=fault(e);
    }
    lck_mtx_unlock(&shared_lock);
    return e;
}
int x9_dsp_shared_resolve_semaphore(void)
{
    lck_mtx_lock(&shared_lock);int e=shared.attempted?release_sem():ENXIO;
    if(e && shared.attempted)e=fault(e);
    lck_mtx_unlock(&shared_lock);return e;
}
int x9_dsp_shared_release(void)
{
    lck_mtx_lock(&shared_lock);int e=shared.attempted?0:ENXIO;
    /* A failed shared-memory operation is not repaired here. Explicit
     * release of an uncertain semaphore must settle BEFORE halt can pass. */
    if(!e && shared.sem_lease)e=EBUSY;
    if(!e)e=authority(false);
    if(!e)e=x9_adsp_mt6993_speaker_halted_validate(shared.policy.ticket);
    if(!e)e=shared.policy.validate_drained(shared.policy.context,shared.policy.ticket,
        shared.policy.generation,shared.map_bus+0x24000,X9_DSP_SHARED_RING_BYTES,shared.policy.afe_memif);
    if(!e && shared.mapped)e=cache_binding(true);
    if(!e) {
        if(shared.mapped)ml_io_unmap(shared.map,X9_DSP_SHARED_MAP_BYTES);
        shared.map=0;shared.mapped=shared.registered=false;
        memset(shared.region,0,sizeof(shared.region));
        if(shared.child_lease) {
            e=x9_adsp_mt6993_shared_drop(shared.policy.ticket,shared.child_lease);
            if(!e)shared.child_lease=0;
        }
        /* Failed drop retains the child, not a dangling mapping. Cleanup
         * may retry drop only; never unmap twice or restart after failure. */
        if(!e)shared.attempted=false;
        /* last_generation and error intentionally survive cleanup. */
    }
    if(e && shared.attempted)e=fault(e);
    lck_mtx_unlock(&shared_lock);return e;
}
