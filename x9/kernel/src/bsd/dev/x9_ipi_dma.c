/* OPPO audio_ipi_dma.c ABI, own byte/state implementation. Never use its
 * modulo repair, whole-carveout zero or timeout/free as physical recovery. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/cpu_data.h>
#include <vm/pmap.h>
#include <arm/machine_routines.h>
#include <arm/memory_types.h>
#include "x9_ipi_dma.h"
#include "x9_tfa_multimsg.h"
#include "x9_adsp_transport.h"
#include "x9_adsp_mt6993.h"
#include "x9_audio_resources.h"
extern ppnum_t pmap_find_phys(pmap_t,addr64_t);
extern unsigned int pmap_cache_attributes(ppnum_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,unsigned int);
extern void ml_io_unmap(vm_offset_t,vm_size_t);
LCK_GRP_DECLARE(ipi_dma_group,"x9-ipi-dma");
LCK_MTX_DECLARE(ipi_dma_lock,&ipi_dma_group);
_Static_assert(X9_DSP_DMA_INFO_BYTES==56,"MTK region descriptor");
_Static_assert(X9_IPI_DMA_REGION_BYTES==3*128,"isolated read/write cache lines");
_Static_assert(X9_IPI_DMA_RING_BYTES>X9_TFA_MULTIMSG_LE32_MAX+128+16,"single batch fits");
_Static_assert(X9_IPI_DMA_READBACK_BYTES==0x4000,"independent 16KiB writeback page");
_Static_assert(sizeof(struct x9_ipi_dma_policy)==80,"mandatory exchange authority v3");
static struct {
    struct x9_ipi_dma_policy policy;
    uint64_t bus,child;
    vm_offset_t map;
    uint32_t pool_bytes,map_bytes,checksum,pool_offset,ring_offset,entry,wb_offset;
    uint32_t write,read,last_generation,error;
    uint32_t get_capacity;
    uint32_t exchange_bytes,exchange_form,exchange_response,exchange_phase;
    bool attempted,mapped,initialized,register_attempted,registered,pending,pending_get;
    bool exchange_active,exchange_published;
    uint8_t exchange_request[16];
    uint8_t staging[X9_TFA_MULTIMSG_LE32_MAX];
    uint8_t readback[X9_IPI_DMA_READBACK_BYTES];
} dma;
static void put32(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void put64(uint8_t *p,uint64_t v)
{for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(i*8));}
static uint32_t get32(const uint8_t *p)
{return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t get64(const uint8_t *p)
{return get32(p)|((uint64_t)get32(p+4)<<32);}
static uint32_t index_read(uint32_t offset)
{
    uint32_t v=*(volatile uint32_t *)(dma.map+offset);
    __asm__ volatile("dmb oshld":::"memory");return v;
}
static int matches(uint64_t ticket,uint32_t generation)
{return !dma.attempted || !ticket || ticket!=dma.policy.ticket ||
    !generation || generation!=dma.policy.generation?EPERM:0;}
static int fault(int e)
{
    if(e==EAGAIN)e=EIO;
    if(!dma.error)dma.error=(uint32_t)e;
    x9_audio_resources_fault(X9AudioResourceSpeaker,dma.policy.ticket,e);return e;
}
static int cache_binding(void)
{
    if(PAGE_SIZE!=0x4000)return ENOTSUP;
    for(uint32_t off=0;off<dma.map_bytes;off+=0x4000) {
        ppnum_t pn=(ppnum_t)((dma.bus+off)>>PAGE_SHIFT);
        if((pmap_cache_attributes(pn)&VM_WIMG_MASK)!=VM_WIMG_COPYBACK)return ENOTSUP;
        vm_offset_t alias=ml_static_ptovirt((vm_offset_t)(dma.bus+off));
        ppnum_t a=pmap_find_phys(kernel_pmap,alias);
        if(a && a!=pn)return EFAULT;
        if(dma.mapped && pmap_find_phys(kernel_pmap,dma.map+off)!=pn)return EFAULT;
    }
    return 0;
}
/* No transport/status call here: also used UNDER the transport mutex. */
static int physical(uint32_t stage)
{
    int e=x9_adsp_mt6993_speaker_validate(dma.policy.ticket);
    if(!e)e=dma.policy.validate_memory(dma.policy.context,dma.policy.ticket,
        dma.policy.generation,dma.bus,dma.pool_bytes,dma.map_bytes,
        dma.policy.task_count,dma.policy.controller_scene,dma.policy.task_scene,
        dma.policy.wire_packing,stage);
    if(!e)e=cache_binding();return e;
}
static uint32_t stage(void)
{return dma.pending || dma.exchange_published?X9IPIDMAPublished:
    dma.initialized?X9IPIDMABound:X9IPIDMACold;}
static int online(void)
{
    X9AudioDSPStatus s={0};int e=x9_adsp_status(0,&s);
    if(!e && (s.generation!=dma.policy.generation || s.state!=X9DSPOnline || s.error))e=ESTALE;
    return e;
}
/* Only AP-owned header/config/write cache lines. DSP read_idx is observed
 * separately with IVAC; never CVAC stale DSP data back over its publication. */
static int table(void)
{
    if(!dma.mapped || !dma.initialized)return ENXIO;
    const uint8_t *p=(const uint8_t *)dma.map;
    if(get64(p)!=dma.bus || get64(p+8)!=dma.map || get32(p+16)!=dma.pool_bytes ||
       get32(p+dma.checksum)!=dma.checksum || get32(p+dma.checksum+4)!=dma.pool_offset ||
       get32(p+dma.entry)!=dma.ring_offset || get32(p+dma.entry+4)!=X9_IPI_DMA_RING_BYTES ||
       index_read(dma.entry+256)!=dma.write)return EPROTO;
    return 0;
}
/* Host phases do not establish DSP acceptance. Actual command ownership and
 * result identity must still be freshly proven by the independent supplier. */
static int exchange_authority(void)
{
    if(!dma.exchange_active)return 0;
    return dma.policy.validate_exchange(dma.policy.context,dma.exchange_request,
        dma.exchange_bytes,dma.policy.wire_packing,dma.exchange_form,
        dma.exchange_response,dma.exchange_phase);
}
static void exchange_clear(void)
{
    dma.exchange_active=dma.exchange_published=false;
    dma.exchange_bytes=dma.exchange_form=dma.exchange_response=dma.exchange_phase=0;
    memset(dma.exchange_request,0,sizeof(dma.exchange_request));
}
static int admission(void)
{
    if(dma.error)return (int)dma.error;
    int e=physical(stage());if(!e)e=online();if(!e)e=table();
    if(!e)e=exchange_authority();
    return e?fault(e):0;
}
static int sync_range(uint32_t offset,uint32_t bytes,bool device)
{
    if(!dma.mapped || !bytes || offset>dma.map_bytes || bytes>dma.map_bytes-offset)return EINVAL;
    int e=physical(stage());if(e)return fault(e);
    disable_preemption();uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
    uintptr_t line=(uintptr_t)4<<((ctr>>16)&15);
    if(line<16 || line>128){enable_preemption();return fault(ENOTSUP);}
    uintptr_t lo=(dma.map+offset)&~(line-1),hi=(dma.map+offset+bytes+line-1)&~(line-1);
    __asm__ volatile("dsb sy":::"memory");
    for(uintptr_t at=lo;at<hi;at+=line) {
        if(device)__asm__ volatile("dc cvac, %0"::"r"(at):"memory");
        else __asm__ volatile("dc ivac, %0"::"r"(at):"memory");
    }
    __asm__ volatile("dsb sy":::"memory");enable_preemption();
    e=physical(stage());return e?fault(e):0;
}
static int guarded(void *context,const struct x9_dsp_message *m)
{
    if(context!=&dma || !m || dma.error)return dma.error?(int)dma.error:EPERM;
    int e=physical(stage());if(!e)e=table();
    if(!e && (m->source!=1 || m->target!=2))e=EPERM;
    if(!e && m->id==X9_IPI_DMA_INIT) {
        if(m->scene!=dma.policy.controller_scene || m->type!=X9DSPPayload ||
           m->ack!=X9DSPDirect || m->param1!=16 || m->param2 ||
           get64(m->payload)!=dma.bus || get64(m->payload+8)!=dma.pool_bytes)e=EPERM;
    } else if(!e && m->id==X9_IPI_DMA_UPDATE) {
        if(m->scene!=dma.policy.controller_scene || m->type!=X9DSPOnly ||
           m->ack!=X9DSPNeedAck || m->param1!=dma.policy.task_scene || m->param2)e=EPERM;
    } else if(!e && m->id==X9_IPI_TFA_SET) {
        if(!dma.registered || !dma.pending || dma.pending_get || m->scene!=dma.policy.task_scene ||
           m->type!=X9DSPDMA || m->ack!=X9DSPNeedAck || m->param2!=X9_IPI_TFA_TYPE ||
           get64(m->payload)!=(uint64_t)(uintptr_t)dma.staging ||
           get32(m->payload+32)!=dma.read || get32(m->payload+36)!=m->param1)e=EPERM;
        if(!e && dma.exchange_active &&
           (!dma.exchange_published || dma.exchange_phase!=X9IPIReadSetPending ||
            m->param1!=dma.exchange_bytes))e=EPERM;
    } else if(!e && m->id==X9_IPI_TFA_GET) {
        if(!dma.registered || !dma.pending || !dma.pending_get ||
           m->scene!=dma.policy.task_scene || m->type!=X9DSPPayload ||
           m->ack!=X9DSPNeedAck || m->param1!=X9_DSP_READBACK_INFO_BYTES ||
           m->param2!=X9_IPI_TFA_TYPE || !dma.get_capacity ||
           dma.get_capacity>X9_IPI_DMA_READBACK_BYTES ||
           get32(m->payload)!=dma.get_capacity || get32(m->payload+4) ||
           get64(m->payload+8)!=dma.bus+dma.wb_offset)e=EPERM;
        if(!e)e=dma.policy.validate_readback(dma.policy.context,dma.get_capacity);
        if(!e && dma.exchange_active &&
           (!dma.exchange_published || dma.exchange_phase!=X9IPIReadGetPending ||
            dma.get_capacity!=dma.exchange_response))e=EPERM;
    } else if(!e)e=EPERM;
    if(!e)e=exchange_authority();
    return e?fault(e):0;
}
static int send(struct x9_dsp_message *m,struct x9_dsp_message *received)
{
    int e=admission();if(e)return e;
    const struct x9_adsp_request_guard g={1,dma.policy.generation,&dma,guarded};
    struct x9_dsp_message reply;
    e=x9_adsp_request_guarded(0,m,m->ack==X9DSPNeedAck?&reply:NULL,250,&g);
    if(!e && m->id==X9_IPI_TFA_GET) {
        uint32_t n;
        if(reply.type!=X9DSPPayload || reply.param1!=X9_DSP_READBACK_INFO_BYTES)e=EPROTO;
        else e=x9_dsp_readback_decode(reply.payload,reply.param1,
            dma.bus+dma.wb_offset,dma.get_capacity,&n);
    }
    if(!e && m->ack==X9DSPNeedAck)
        e=dma.policy.validate_reply(dma.policy.context,m,&reply);
    if(!e)e=admission();
    if(!e && received && m->ack==X9DSPNeedAck)*received=reply;
    return e?fault(e):0;
}
int x9_ipi_dma_acquire(const struct x9_ipi_dma_policy *policy)
{
    if(!policy)return EINVAL;
    const struct x9_ipi_dma_policy p=*policy;
    if(p.version!=X9_IPI_DMA_POLICY_VERSION || !p.ticket || !p.generation ||
       p.task_count<48 || p.task_count>64 || p.controller_scene<=41 ||
       p.controller_scene>=p.task_count || (p.task_scene!=14 && p.task_scene!=32) ||
       (p.wire_packing!=X9TFABE24 && p.wire_packing!=X9TFALE32) ||
       !p.validate_memory || !p.validate_parameters || !p.validate_reply ||
       !p.validate_readback || !p.validate_exchange)return EINVAL;
    lck_mtx_lock(&ipi_dma_lock);
    if(dma.attempted || p.generation<=dma.last_generation) {
        lck_mtx_unlock(&ipi_dma_lock);return EBUSY;
    }
    int e=x9_adsp_mt6993_speaker_validate(p.ticket);X9AudioDSPStatus s={0};
    if(!e)e=x9_adsp_status(0,&s);
    if(!e && (s.generation!=p.generation || s.state!=X9DSPOnline || s.error))e=ESTALE;
    struct x9_adsp_handoff h;if(!e)e=x9_adsp_mt6993_copy_handoff(&h);
    if(!e) {
        uint64_t bus=h.reserve_base+h.block[X9ADSPMemIPIA].offset;
        uint32_t bytes=h.block[X9ADSPMemIPIA].bytes;
        if((bus&0x3fff) || bytes!=0x180000)e=ENOTSUP;
        else {
            dma.policy=p;dma.bus=bus;dma.pool_bytes=bytes;
            dma.checksum=X9_IPI_DMA_HEADER_BYTES+p.task_count*2*X9_IPI_DMA_REGION_BYTES;
            /* Vendor aligns the OFFSET of pool_offset, not sizeof(table). */
            dma.pool_offset=(dma.checksum+4+127u)&~127u;
            dma.ring_offset=(dma.pool_offset+0x3fffu)&~0x3fffu;
            dma.wb_offset=dma.ring_offset+X9_IPI_DMA_RING_BYTES;
            dma.map_bytes=dma.wb_offset+X9_IPI_DMA_READBACK_BYTES;
            dma.entry=X9_IPI_DMA_HEADER_BYTES+p.task_scene*2*X9_IPI_DMA_REGION_BYTES;
            if(dma.map_bytes>bytes)e=ERANGE;
        }
    }
    if(!e) {
        dma.attempted=true;dma.last_generation=p.generation;dma.error=0;
        e=physical(X9IPIDMACold);
        if(!e)e=x9_adsp_mt6993_ipi_hold(p.ticket,&dma.child);
        if(!e) {
            dma.map=ml_io_map_unmappable((vm_offset_t)dma.bus,dma.map_bytes,VM_WIMG_COPYBACK);
            if(!dma.map)e=ENOMEM;else dma.mapped=true;
        }
        if(!e)e=physical(X9IPIDMACold);
        if(!e)e=online();
        if(!e) {
            /* Clear only the qualified table/TX/WB inner prefix, never the
             * full IPI_A reservation or any AUDIO_COMMON/foreign region. */
            memset((void *)dma.map,0,dma.map_bytes);
            uint8_t *b=(uint8_t *)dma.map;
            put64(b,dma.bus);put64(b+8,dma.map);put32(b+16,dma.pool_bytes);
            put32(b+dma.checksum,dma.checksum);put32(b+dma.checksum+4,dma.pool_offset);
            put32(b+dma.entry,dma.ring_offset);put32(b+dma.entry+4,X9_IPI_DMA_RING_BYTES);
            e=sync_range(0,dma.map_bytes,true);
            if(!e){dma.initialized=true;e=admission();}
        }
        if(e)e=fault(e); /* Explicit physical halt/unmap/drop, never unwind. */
    }
    lck_mtx_unlock(&ipi_dma_lock);return e;
}
int x9_ipi_dma_register(uint64_t ticket,uint32_t generation)
{
    lck_mtx_lock(&ipi_dma_lock);int e=matches(ticket,generation);
    if(!e && dma.register_attempted)e=EBUSY;
    if(!e)e=admission();
    if(!e) {
        dma.register_attempted=true;
        struct x9_dsp_message m={0};m.scene=(uint8_t)dma.policy.controller_scene;
        m.source=1;m.target=2;m.type=X9DSPPayload;m.ack=X9DSPDirect;
        m.id=X9_IPI_DMA_INIT;m.param1=16;
        put64(m.payload,dma.bus);put64(m.payload+8,dma.pool_bytes);e=send(&m,NULL);
        if(!e) {
            m.type=X9DSPOnly;m.ack=X9DSPNeedAck;m.id=X9_IPI_DMA_UPDATE;
            m.param1=dma.policy.task_scene;memset(m.payload,0,sizeof(m.payload));e=send(&m,NULL);
        }
        if(!e)dma.registered=true;
    }
    lck_mtx_unlock(&ipi_dma_lock);return e;
}
static int observed_read(uint32_t *value)
{
    int e=sync_range(dma.entry+128,128,false);if(!e)e=admission();
    if(!e) {
        uint32_t r=index_read(dma.entry+128);
        if(r>=X9_IPI_DMA_RING_BYTES || (r&127u) || (r!=dma.read && r!=dma.write))e=EPROTO;
        else *value=r;
    }
    return e?fault(e):0;
}
/* Caller holds the one IPI mutex. Staging is already validated wire data.
 * Shared by trusted whole-batch SET and the narrow READ exchange only. */
static int publish_set(uint32_t n)
{
    if(!n || n>sizeof(dma.staging))return EINVAL;
    int e=0;uint32_t r=0;if(!e)e=observed_read(&r);
    if(!e && r!=dma.write)e=fault(EBUSY); /* no inherited/parallel region consumer */
    if(!e) {
        uint32_t old=dma.write,aligned=(n+127u)&~127u;
        uint32_t end=(old+aligned)%X9_IPI_DMA_RING_BYTES;
        uint8_t *ring=(uint8_t *)(dma.map+dma.ring_offset);
        e=admission();
        if(!e) {
            for(uint32_t i=0;i<aligned;i++)ring[(old+i)%X9_IPI_DMA_RING_BYTES]=i<n?dma.staging[i]:0;
            uint32_t first=X9_IPI_DMA_RING_BYTES-old;if(first>aligned)first=aligned;
            e=sync_range(dma.ring_offset+old,first,true);
            if(!e && first<aligned)e=sync_range(dma.ring_offset,aligned-first,true);
        }
        if(!e)e=admission();
        if(!e)e=observed_read(&r);
        if(!e && r!=old)e=fault(EPROTO);
        if(!e) {
            /* Retain ambiguity before the first index store. Never restore
             * old after a failed clean or a pre-doorbell EAGAIN. */
            dma.read=old;dma.write=end;dma.pending=true;
            if(dma.exchange_active) {
                dma.exchange_published=true;dma.exchange_phase=X9IPIReadSetPending;
            }
            __asm__ volatile("dsb sy":::"memory");
            *(volatile uint32_t *)(dma.map+dma.entry+256)=end;
            e=sync_range(dma.entry+256,128,true);
        }
        if(!e) {
            struct x9_dsp_message m={0};m.scene=(uint8_t)dma.policy.task_scene;
            m.source=1;m.target=2;m.type=X9DSPDMA;m.ack=X9DSPNeedAck;
            m.id=X9_IPI_TFA_SET;m.param1=n;m.param2=X9_IPI_TFA_TYPE;
            /* OPPO sets dma_addr in the first 8B union, then region idx/size.
             * This explicit boot-lifetime private token matches that ABI;
             * it never points at a user buffer or authorizes a DSP address. */
            put64(m.payload,(uint64_t)(uintptr_t)dma.staging);
            put32(m.payload+32,old);put32(m.payload+36,n);e=send(&m,NULL);
        }
        uint64_t ticks;nanoseconds_to_absolutetime(250000000,&ticks);
        uint64_t until=mach_absolute_time()+ticks;
        for(unsigned i=0;!e;i++) {
            e=observed_read(&r);if(e)break;
            if(i>=10000 || mach_absolute_time()>=until){e=fault(ETIMEDOUT);break;}
            if(r==end) {
                dma.read=end;dma.pending=false;
                if(dma.exchange_active) {
                    dma.exchange_phase=X9IPIReadConsumed;e=admission();
                }
                break;
            }
            delay(25);
        }
    }
    if(e && (dma.pending || dma.exchange_published))e=fault(e);
    return e;
}
int x9_ipi_dma_set(uint64_t ticket,uint32_t generation,const void *data,
                    uint32_t bytes,uint32_t source_packing)
{
    if(!data || !bytes || (uintptr_t)data>UINTPTR_MAX-bytes ||
       (source_packing!=X9TFABE24 && source_packing!=X9TFALE32))return EINVAL;
    lck_mtx_lock(&ipi_dma_lock);int e=matches(ticket,generation);
    if(e){lck_mtx_unlock(&ipi_dma_lock);return e;}
    uintptr_t a=(uintptr_t)data;
    if(!e && dma.mapped && a<dma.map+dma.map_bytes && dma.map<a+bytes)e=EINVAL;
    if(!e && (!dma.registered || dma.pending || dma.exchange_active))e=EBUSY;
    if(!e)e=admission();
    uint32_t n=0;
    if(!e)e=x9_tfa_multimsg_convert(data,bytes,source_packing,dma.policy.wire_packing,
        dma.staging,sizeof(dma.staging),&n);
    if(!e) {
        e=dma.policy.validate_parameters(dma.policy.context,dma.staging,n,dma.policy.wire_packing);
        if(e)e=fault(e);
    }
    if(!e)e=publish_set(n);
    /* A pending legacy token/command stays immutable until physical halt;
     * clearing staging on an uncertain SET would destroy that lifetime. */
    if(!dma.pending && !dma.exchange_active)memset(dma.staging,0,sizeof(dma.staging));
    lck_mtx_unlock(&ipi_dma_lock);return e;
}
static bool overlap(uintptr_t a,uint32_t n,uintptr_t b,uint32_t m)
{return a<b+m && b<a+n;}
/* Exact physical byte ranges, not merely distinct virtual addresses. Caller
 * pins each range; small bounded spans can share a page if bytes are disjoint. */
static int physical_disjoint(uintptr_t a,uint32_t an,uintptr_t b,uint32_t bn)
{
    uintptr_t ae=a+an,be=b+bn;
    for(uintptr_t x=a;x<ae;) {
        ppnum_t xp=pmap_find_phys(kernel_pmap,x);if(!xp)return EFAULT;
        uint32_t xn=(uint32_t)(PAGE_SIZE-(x&(PAGE_SIZE-1)));
        if(xn>ae-x)xn=(uint32_t)(ae-x);
        uint64_t xa=((uint64_t)xp<<PAGE_SHIFT)+(x&(PAGE_SIZE-1));
        for(uintptr_t y=b;y<be;) {
            ppnum_t yp=pmap_find_phys(kernel_pmap,y);if(!yp)return EFAULT;
            uint32_t yn=(uint32_t)(PAGE_SIZE-(y&(PAGE_SIZE-1)));
            if(yn>be-y)yn=(uint32_t)(be-y);
            uint64_t ya=((uint64_t)yp<<PAGE_SHIFT)+(y&(PAGE_SIZE-1));
            if(xa<ya+yn && ya<xa+xn)return EINVAL;
            y+=yn;
        }
        x+=xn;
    }
    return 0;
}
static int caller_binding(uintptr_t a,uint32_t bytes)
{
    /* Caller retains pinned kernel pages for the whole call. Reject user
     * addresses/unmapped pages and EVERY physical IPI_A alias, not just the
     * native mapped VA (a physmap alias must not overwrite WB/table/TX). */
    uintptr_t end=a+bytes;
    for(uintptr_t at=a&~(PAGE_SIZE-1);at<end;at+=PAGE_SIZE) {
        ppnum_t pn=pmap_find_phys(kernel_pmap,at);
        if(!pn)return EFAULT;
        uint64_t pa=(uint64_t)pn<<PAGE_SHIFT;
        if(pa>=dma.bus && pa<dma.bus+dma.pool_bytes)return EINVAL;
        if(at>UINTPTR_MAX-PAGE_SIZE)break;
    }
    return physical_disjoint(a,bytes,(uintptr_t)&dma,sizeof(dma));
}
static int output_arguments(void *out,uint32_t capacity,uint32_t *data_size)
{
    uintptr_t a=(uintptr_t)out,b=(uintptr_t)data_size;
    if(!out || !data_size || !capacity || capacity>X9_IPI_DMA_READBACK_BYTES ||
       a>UINTPTR_MAX-capacity || b>UINTPTR_MAX-sizeof(*data_size) ||
       overlap(a,capacity,b,sizeof(*data_size)) ||
       overlap(a,capacity,(uintptr_t)&dma,sizeof(dma)) ||
       overlap(b,sizeof(*data_size),(uintptr_t)&dma,sizeof(dma)))return EINVAL;
    return 0;
}
static int output_bindings(void *out,uint32_t capacity,uint32_t *data_size)
{
    uintptr_t a=(uintptr_t)out,b=(uintptr_t)data_size;int e=0;
    if(dma.mapped && (overlap(a,capacity,dma.map,dma.map_bytes) ||
       overlap(b,sizeof(*data_size),dma.map,dma.map_bytes)))e=EINVAL;
    if(!e)e=caller_binding(a,capacity);
    if(!e)e=caller_binding(b,sizeof(*data_size));
    if(!e)e=physical_disjoint(a,capacity,b,sizeof(*data_size));
    return e;
}
/* ONE lock across the pair; never call a public entrypoint recursively. */
static int get_locked(void *out,uint32_t capacity,uint32_t *data_size)
{
    int e=0;
    if(!dma.registered || dma.pending)e=EBUSY;
    if(!e && dma.exchange_active &&
       (!dma.exchange_published || dma.exchange_phase!=X9IPIReadConsumed ||
        capacity!=dma.exchange_response))e=EPERM;
    if(!e)e=admission();
    if(!e)e=output_bindings(out,capacity,data_size);
    if(!e) {
        e=dma.policy.validate_readback(dma.policy.context,capacity);
        if(e)e=fault(e);
    }
    uint32_t r=0;if(!e)e=observed_read(&r);
    if(!e && r!=dma.write)e=fault(EBUSY);
    struct x9_dsp_message m={0},reply;
    if(!e) {
        m.scene=(uint8_t)dma.policy.task_scene;m.source=1;m.target=2;
        m.type=X9DSPPayload;m.ack=X9DSPNeedAck;m.id=X9_IPI_TFA_GET;
        m.param1=X9_DSP_READBACK_INFO_BYTES;m.param2=X9_IPI_TFA_TYPE;
        e=x9_dsp_readback_encode(dma.bus+dma.wb_offset,capacity,m.payload,m.param1);
    }
    if(!e)e=admission();
    if(!e) {
        /* No outstanding writer here: cold exclusion or the previous GET's
         * actual completion supplier was proven. Whole dedicated WB page
         * only; never clean table/DSP indices or TX stale data. */
        memset((void *)(dma.map+dma.wb_offset),0,X9_IPI_DMA_READBACK_BYTES);
        e=sync_range(dma.wb_offset,X9_IPI_DMA_READBACK_BYTES,true);
    }
    if(!e)e=admission();
    if(!e) {
        /* Mark before attempting TX: a pre-doorbell error is NOT permission
         * to reuse the address. Unknown writers retain it until real halt. */
        dma.get_capacity=capacity;dma.pending_get=true;dma.pending=true;
        if(dma.exchange_active)dma.exchange_phase=X9IPIReadGetPending;
        e=send(&m,&reply);
    }
    uint32_t n=0;
    if(!e)e=x9_dsp_readback_decode(reply.payload,reply.param1,
        dma.bus+dma.wb_offset,capacity,&n);
    if(!e && dma.exchange_active && n!=dma.exchange_response)e=EPROTO;
    /* send already proved exact reply fields AND actual firmware completion.
     * DSP no longer owns WB; invalidate its isolated cache lines, even for
     * zero-size reply. Never dereference an address from the reply. */
    if(!e)e=sync_range(dma.wb_offset,X9_IPI_DMA_READBACK_BYTES,false);
    if(!e)e=admission();
    if(!e && n)memcpy(dma.readback,(const void *)(dma.map+dma.wb_offset),n);
    if(!e && dma.exchange_active) {
        e=x9_tfa_words_scan(dma.readback,n,dma.policy.wire_packing);
        if(!e)dma.exchange_phase=X9IPIReadComplete;
    }
    if(!e)e=admission();
    if(!e)e=observed_read(&r);
    if(!e && r!=dma.write)e=fault(EPROTO);
    if(!e)e=output_bindings(out,capacity,data_size);
    if(!e)e=admission();
    if(!e) {
        if(n)memcpy(out,dma.readback,n);
        *data_size=n;
        dma.pending=dma.pending_get=false;dma.get_capacity=0;
    }
    if(e && (dma.pending || dma.exchange_published))e=fault(e);
    memset(dma.readback,0,sizeof(dma.readback));
    return e;
}
int x9_ipi_dma_get(uint64_t ticket,uint32_t generation,void *out,
                    uint32_t capacity,uint32_t *data_size)
{
    int e=output_arguments(out,capacity,data_size);if(e)return e;
    lck_mtx_lock(&ipi_dma_lock);e=matches(ticket,generation);
    if(!e && dma.exchange_active)e=EBUSY;
    if(!e)e=get_locked(out,capacity,data_size);
    lck_mtx_unlock(&ipi_dma_lock);return e;
}
int x9_ipi_dma_exchange(uint64_t ticket,uint32_t generation,const void *request,
    uint32_t request_bytes,uint32_t form,void *out,uint32_t response_bytes,
    uint32_t *data_size)
{
    uintptr_t c=(uintptr_t)request,a=(uintptr_t)out,b=(uintptr_t)data_size;
    if(!request || !request_bytes || request_bytes>sizeof(dma.exchange_request) ||
       c>UINTPTR_MAX-request_bytes ||
       (form!=X9IPIReadWarmWord && form!=X9IPIReadColdBatch))return EINVAL;
    int e=output_arguments(out,response_bytes,data_size);if(e)return e;
    if(overlap(c,request_bytes,a,response_bytes) ||
       overlap(c,request_bytes,b,sizeof(*data_size)) ||
       overlap(c,request_bytes,(uintptr_t)&dma,sizeof(dma)))return EINVAL;
    lck_mtx_lock(&ipi_dma_lock);e=matches(ticket,generation);bool started=false;
    if(!e && (!dma.registered || dma.pending || dma.exchange_active))e=EBUSY;
    uint32_t w=dma.policy.wire_packing==X9TFABE24?3u:4u;
    uint32_t bound=dma.policy.wire_packing==X9TFABE24?
        X9_TFA_MULTIMSG_BE24_MAX:X9_TFA_MULTIMSG_LE32_MAX;
    if(!e && (request_bytes!=(form==X9IPIReadWarmWord?w:4u*w) ||
       response_bytes%w || response_bytes>bound))e=EINVAL;
    if(!e)e=admission();
    /* Prove ALL destinations and input aliases BEFORE any SET publication,
     * not only on GET after changing the firmware's command state. */
    if(!e)e=output_bindings(out,response_bytes,data_size);
    if(!e)e=caller_binding(c,request_bytes);
    if(!e)e=physical_disjoint(c,request_bytes,a,response_bytes);
    if(!e)e=physical_disjoint(c,request_bytes,b,sizeof(*data_size));
    if(!e) {
        memcpy(dma.exchange_request,request,request_bytes);
        dma.exchange_bytes=request_bytes;dma.exchange_form=form;
        dma.exchange_response=response_bytes;dma.exchange_phase=X9IPIReadPrepared;
        dma.exchange_active=true;started=true;
        if(form==X9IPIReadWarmWord)
            e=x9_tfa_words_scan(dma.exchange_request,request_bytes,dma.policy.wire_packing);
        else {
            struct x9_tfa_multimsg_info info;
            e=x9_tfa_multimsg_scan(dma.exchange_request,request_bytes,dma.policy.wire_packing,&info);
            if(!e && (info.messages!=1 || info.payload_words!=1))e=EBADMSG;
        }
        /* No inferred ext_dsp mode, spkr_select OR, wrapping or conversion.
         * Supplier must approve the exact private wire bytes and lifetime. */
        if(!e)e=admission();
        if(!e) {
            memcpy(dma.staging,dma.exchange_request,request_bytes);
            e=publish_set(request_bytes);
        }
        if(!e)e=get_locked(out,response_bytes,data_size);
    }
    if(started) {
        if(e && dma.exchange_published)e=fault(e);
        /* Once published, even GET preflight failure retains the immutable
         * pair despite SET already consumed. Only actual halt can retire it.
         * Rejected calls must NOT clear another pending owner's staging. */
        if(!e || !dma.exchange_published) {
            exchange_clear();memset(dma.staging,0,sizeof(dma.staging));
        }
    }
    lck_mtx_unlock(&ipi_dma_lock);return e;
}
int x9_ipi_dma_release(uint64_t ticket,uint32_t generation)
{
    lck_mtx_lock(&ipi_dma_lock);int e=matches(ticket,generation);
    X9AudioDSPStatus s={0};if(!e)e=x9_adsp_status(0,&s);
    if(!e && (s.generation!=generation || s.state!=X9DSPOffline))e=ESTALE;
    if(!e)e=x9_adsp_mt6993_speaker_halted_validate(ticket);
    if(!e)e=physical(X9IPIDMACleanup);
    if(!e && dma.mapped) {
        ml_io_unmap(dma.map,dma.map_bytes);dma.map=0;dma.mapped=false;
    }
    if(!e && dma.child)e=x9_adsp_mt6993_ipi_drop(ticket,dma.child);
    if(!e) {
        dma.child=0;dma.attempted=dma.initialized=dma.register_attempted=dma.registered=dma.pending=false;
        dma.pending_get=false;dma.get_capacity=0;
        exchange_clear();
        dma.write=dma.read=dma.error=0;memset(&dma.policy,0,sizeof(dma.policy));
        memset(dma.staging,0,sizeof(dma.staging));
        memset(dma.readback,0,sizeof(dma.readback));
    }
    lck_mtx_unlock(&ipi_dma_lock);return e;
}
