/* Sole CAMSV0 GIC1NS IAR -> one-shot bank sample -> pinned thread worker.
 * The inactive prepare path creates only a dormant worker. begin still needs
 * the real controller's exact S1/domain/route/request lease, and enables NO
 * CAMSV source/DMA/CMOS/VF/CQ. After RAW arm it publishes the
 * actual S1 IOVA/tag0 FH metadata, then no-credit FBC_EN with unchanged empty
 * counters and both-bank DB consumption, before GIC changes. After all
 * three actual GIC lines are published, it issues one RCNT command and
 * requires the full both-bank counter/control acceptance twice. No replay,
 * no automatic rearm, no credit cancellation by EN-off/reset or CPU RETIRED.
 * It is not a capture provider registration.
 * Lock order: camera_control_lock -> camera_irq_lock -> camera_camsv_lock.
 * Worker NEVER acquires camera_control_lock; close can drain its pin there.
 * Hard IRQ uses only spin-protected immutable binding and permanent mappings.
 */
#include "x9_camera_irq_layout.h"
#include <kern/thread.h>
#include <machine/machine_routines.h>
LCK_MTX_DECLARE(camera_irq_lock,&camera_group);
LCK_SPIN_DECLARE(camera_irq_spin,&camera_group);
_Static_assert(sizeof(struct x9_camera_irq_layout)==64,"camera IRQ topology ABI");
enum { CAM_IRQ_RING=4, CAM_IRQ_PRIORITY=0xa0 };
struct cam_irq_event {
    struct x9_camera_camsv_irq frame;
    uint32_t role, sof, channel, common, top, fifo;
    int error;
};
static struct {
    struct x9_camera_irq_layout layout;
    vm_offset_t gic,view[2],hwv,cam_cg,sv_cg,top;
    uint32_t fence;
    uint64_t epoch,last_epoch,route_before[3],route_expected,reported;
    uint8_t priority_before[3];
    uint32_t dirty_route,dirty_priority,uncertain_route,uncertain_priority;
    bool prepared,thread_started,close_attempted;
    struct x9_camera_camsv_slot slots[2], *slot;
    /* All fields below protected by spin, including first fault. No hard-IRQ
     * access to slot/PTE/RAW, owner callbacks, power APIs or sleeping locks. */
    struct x9_camera_camsv_request request;
    struct x9_camera_camsv_irq sof_frame;
    struct cam_irq_event queue[CAM_IRQ_RING];
    unsigned head,count;
    uint64_t deliveries[3];
    bool owns,accepting,sof_seen,busy;
    int error;
} cam_irq;
static boolean_t cam_irq_spin_take(void)
{
    boolean_t old=ml_set_interrupts_enabled(FALSE);
    lck_spin_lock(&camera_irq_spin);return old;
}
static void cam_irq_spin_drop(boolean_t old)
{lck_spin_unlock(&camera_irq_spin);ml_set_interrupts_enabled(old);}
static uint32_t cam_irq_rd(vm_offset_t base,unsigned offset)
{
    uint32_t v=*(volatile uint32_t *)(base+offset);
    __asm__ volatile("dmb oshld":::"memory");return v;
}
static void cam_irq_wr(unsigned offset,uint32_t v)
{
    *(volatile uint32_t *)(cam_irq.gic+offset)=v;
    __asm__ volatile("dsb sy":::"memory");
}
static unsigned cam_irq_bank(unsigned role,unsigned base)
{return base+4*(cam_irq.layout.intid[role]/32);}
static uint32_t cam_irq_bit(unsigned role)
{return 1u<<(cam_irq.layout.intid[role]%32);}
static uint64_t cam_irq_route(unsigned role)
{
    uint64_t v=*(volatile uint64_t *)(cam_irq.gic+0x6000+8*cam_irq.layout.intid[role]);
    __asm__ volatile("dmb oshld":::"memory");return v;
}
static uint8_t cam_irq_priority(unsigned role)
{return *(volatile uint8_t *)(cam_irq.gic+0x400+cam_irq.layout.intid[role]);}
static int cam_irq_mapping(void)
{
    if(PAGE_SIZE!=0x4000||!cam_irq.gic)return ENXIO;
    for(unsigned off=0;off<0x40000;off+=0x4000) {
        addr64_t a=(addr64_t)cam_irq.gic+off;ppnum_t p=pmap_find_phys(kernel_pmap,a);
        if(!p||ptoa_64(p)+(a&PAGE_MASK)!=0x06800000ULL+off)return EFAULT;
    }
    for(unsigned b=0;b<2;b++) {
        addr64_t a=cam_irq.view[b];ppnum_t p=pmap_find_phys(kernel_pmap,a);
        if(!p||ptoa_64(p)+(a&PAGE_MASK)!=(b?0x3a608000ULL:0x3a600000ULL))return EFAULT;
    }
    return 0;
}
static int cam_irq_controller_impl(bool pending_ok)
{
    uint32_t ctl=cam_irq_rd(cam_irq.gic,0);
    uint32_t lines;
    if((ctl&0x11)!=0x11||(!pending_ok&&(ctl&(1u<<31))))return EBUSY;
    lines=((cam_irq_rd(cam_irq.gic,4)&31)+1)*32;
    for(unsigned i=0;i<3;i++) {
        unsigned id=cam_irq.layout.intid[i];uint32_t bit=cam_irq_bit(i);
        if(lines<=id||!(cam_irq_rd(cam_irq.gic,cam_irq_bank(i,0x80))&bit)||
           (cam_irq_rd(cam_irq.gic,0xc00+4*(id/16))&(3u<<((id%16)*2))))return EBUSY;
    }
    return 0;
}
static int cam_irq_controller(void)
{return cam_irq_controller_impl(false);}
static int cam_irq_wait_rwp(void)
{
    uint64_t span,end;
    nanoseconds_to_absolutetime(1000000,&span);end=mach_absolute_time()+span;
    for(unsigned i=0;i<500;i++) {
        if(mach_absolute_time()>=end)return ETIMEDOUT;
        if(!(cam_irq_rd(cam_irq.gic,0)&(1u<<31)))return 0;
        delay(2);
    }
    return ETIMEDOUT;
}
static int cam_irq_idle(void)
{
    for(unsigned i=0;i<3;i++)if((cam_irq_rd(cam_irq.gic,cam_irq_bank(i,0x100))|
        cam_irq_rd(cam_irq.gic,cam_irq_bank(i,0x200))|
        cam_irq_rd(cam_irq.gic,cam_irq_bank(i,0x300)))&cam_irq_bit(i))return EBUSY;
    return 0;
}
static int cam_irq_wait_idle(void)
{
    uint64_t span,end;
    nanoseconds_to_absolutetime(1000000,&span);end=mach_absolute_time()+span;
    for(unsigned i=0;i<500;i++) {
        if(mach_absolute_time()>=end)return ETIMEDOUT;
        if(!cam_irq_idle()){delay(2);if(!cam_irq_idle())return 0;}
        delay(2);
    }
    return ETIMEDOUT;
}
/* Only the bound lines; no distributor/redistributor/timer/IPI/security writes.
 * Called under spin. RWP/ACTIVE retirement is checked in thread context. */
static void cam_irq_mask_all(void)
{for(unsigned i=0;i<3;i++)cam_irq_wr(cam_irq_bank(i,0x180),cam_irq_bit(i));}
static int cam_irq_fault(int e)
{
    boolean_t old=cam_irq_spin_take();
    if(!cam_irq.error)cam_irq.error=e;
    cam_irq.accepting=false;if(cam_irq.owns)cam_irq_mask_all();
    e=cam_irq.error;cam_irq_spin_drop(old);return e;
}
/* Fast, non-sleeping power eligibility checks, with parent ACK FIRST. These
 * do not replace the thread-context authority check. The bound MM lifetime
 * forbids OUR unvote/unmap until filter+worker/GIC retirement below. */
static int cam_irq_power(void)
{
    uint32_t ack=CAM_RES_MM_INFRA|(1u<<16)|(1u<<15)|(1u<<8);
    if((cam_irq_rd(cam_irq.hwv,0x12900)&ack)!=ack||
       !(cam_irq_rd(cam_irq.hwv,CAM_RES_CSI_ACK)&CAM_RES_CSI_BIT))return ENXIO;
    if((cam_irq_rd(cam_irq.cam_cg,0)&((1u<<1)|(1u<<6)|(1u<<7)))||
       (cam_irq_rd(cam_irq.sv_cg,0)&(7u<<12))||
       (cam_irq_rd(cam_irq.top,0x270)&cam_irq.fence)!=cam_irq.fence)return ESTALE;
    return 0;
}
static int cam_irq_config(bool pending_ok)
{
    int e=cam_irq_controller_impl(pending_ok);if(e)return e;
    for(unsigned i=0;i<3;i++)if(cam_irq_route(i)!=cam_irq.route_expected||
        cam_irq_priority(i)!=CAM_IRQ_PRIORITY)return ESTALE;
    return 0;
}
/* Whole prior/consumed set before EACH route/priority mutation. An accepted
 * target cannot regress to prior; only an unresolved posted write may still
 * read prior. This is not an external ownership grant or an ABA detector. */
static int cam_irq_partial_config(void)
{
    for(unsigned i=0;i<3;i++) {
        uint32_t bit=1u<<i;
        uint64_t r=cam_irq_route(i),want_r=(cam_irq.dirty_route&bit)?cam_irq.route_expected:cam_irq.route_before[i];
        uint8_t p=cam_irq_priority(i),want_p=(cam_irq.dirty_priority&bit)?CAM_IRQ_PRIORITY:cam_irq.priority_before[i];
        if(r!=want_r&&(!(cam_irq.uncertain_route&bit)||r!=cam_irq.route_before[i]))return ESTALE;
        if(p!=want_p&&(!(cam_irq.uncertain_priority&bit)||p!=cam_irq.priority_before[i]))return ESTALE;
    }
    return 0;
}
static int cam_irq_mutation_guard(void)
{
    const struct x9_camera_camsv_dma *d=&cam_irq.slot->dma;
    int e=d->check(d->context,d->lease,&cam_irq.slot->request);
    if(!e)e=cam_sv_request_guard_locked(cam_irq.epoch,cam_irq.slot);
    if(!e)e=cam_irq_power();if(!e)e=cam_irq_controller();
    if(!e)e=cam_irq_idle();if(!e)e=cam_irq_partial_config();return e;
}
/* Thread-only, camera_control_lock + camera_irq_lock held. A topology,
 * masked line or zero FBC counter is not authority: request_guard independently
 * calls the real immutable lease check. Here prove the actual prepared GIC
 * publication still exists, with no sampled/pending/active input history. */
static int cam_irq_credit_guard_locked(uint64_t epoch)
{
    boolean_t old;
    int e;
    old=cam_irq_spin_take();
    e=epoch!=cam_irq.epoch || !cam_irq.prepared || !cam_irq.owns || !cam_irq.accepting ||
        cam_irq.close_attempted || cam_irq.count || cam_irq.busy || cam_irq.sof_seen?ESTALE:cam_irq.error;
    cam_irq_spin_drop(old);if(e)return e;
    e=cam_irq_mapping();if(!e)e=cam_irq_power();if(!e)e=cam_irq_config(false);
    for(unsigned i=0;i<3 && !e;i++) {
        uint32_t bit=cam_irq_bit(i);
        if(!(cam_irq_rd(cam_irq.gic,cam_irq_bank(i,0x100))&bit) ||
           ((cam_irq_rd(cam_irq.gic,cam_irq_bank(i,0x200))|
             cam_irq_rd(cam_irq.gic,cam_irq_bank(i,0x300)))&bit))e=EBUSY;
    }
    return e;
}
/* Under spin, after actual IAR and only while accepting. Ordinary FIRST/FH
 * reads select exactly the vendor bank. No multi-tag fallback or status sweep. */
static void cam_irq_sample(unsigned role,struct cam_irq_event *ev)
{
    struct x9_camera_camsv_irq *f=&ev->frame;
    unsigned tag=0;
    f->version=X9_CAMERA_CAMSV_VERSION;f->generation=cam_irq.request.frame.generation;
    f->capture_id=cam_irq.request.frame.capture_id;
    f->first_tag=cam_irq_rd(cam_irq.view[1],0x1cc);
    while(tag<8&&!(f->first_tag&(1u<<tag)))tag++;
    if(tag==8){ev->error=EIO;tag=0;} /* source error still sampled once */
    f->outer_sequence=cam_irq_rd(cam_irq.view[0],0x57c+0x40*tag);
    f->inner_sequence=cam_irq_rd(cam_irq.view[1],0x57c+0x40*tag);
    if(role==X9_CAM_IRQ_SOF) {
        f->last_tag=cam_irq_rd(cam_irq.view[1],0x1c8);
        ev->sof=cam_irq_rd(cam_irq.view[0],0x360);
        ev->channel=cam_irq_rd(cam_irq.view[0],0x370);
        for(unsigned i=0;i<4;i++)f->group_tags[i]=cam_irq_rd(cam_irq.view[1],0x1b8+4*i);
        /* Single tag0/group0 implementation. DMA_DONE in CHANNEL is not
         * PASS1_DONE; drop/unknown bits or another SOF are terminal. */
        if(cam_irq.sof_seen||ev->sof!=4||ev->channel&~4u||f->first_tag!=1||f->last_tag!=1||
           f->inner_sequence!=cam_irq.request.inner_sequence||f->group_tags[0]!=1||
           f->group_tags[1]||f->group_tags[2]||f->group_tags[3])ev->error=EIO;
        f->time=mach_absolute_time();
        if(!ev->error){cam_irq.sof_frame=*f;cam_irq.sof_seen=true;}
    } else if(role==X9_CAM_IRQ_HYBRID) {
        f->err_status=cam_irq_rd(cam_irq.view[1],0x350);
        f->done_status=cam_irq_rd(cam_irq.view[0],0x348);
        f->cq_status=cam_irq_rd(cam_irq.view[0],0x201c);
        f->time=mach_absolute_time();
        /* Group/last are ONLY the prior SOF binding, not late live reads. */
        if(cam_irq.sof_seen) {
            f->last_tag=cam_irq.sof_frame.last_tag;
            memcpy(f->group_tags,cam_irq.sof_frame.group_tags,sizeof(f->group_tags));
            if(cam_irq.sof_frame.inner_sequence!=f->inner_sequence||
               f->time<cam_irq.sof_frame.time)ev->error=ESTALE;
        } else if(f->done_status&0x000f0000u)ev->error=EIO;
    } else {
        /* Never consume CHANNEL here: SOF is its one read-clear owner. No
         * raw_lock_done/debug-selector/CCU write follows the debug sample. */
        ev->common=cam_irq_rd(cam_irq.view[0],0x368);
        ev->top=cam_irq_rd(cam_irq.view[0],0x378);
        ev->fifo=cam_irq_rd(cam_irq.view[0],0x104c);
        f->time=mach_absolute_time();ev->error=EIO;
    }
}
bool x9_camera_irq_filter(uint32_t intid)
{
    boolean_t old;
    unsigned role=0;
    bool wake=false;
    int qualification;
    if(intid!=0x26d+32&&intid!=0x273+32&&intid!=0x279+32)return false;
    old=cam_irq_spin_take();
    if(!cam_irq.owns){cam_irq_spin_drop(old);return false;}
    while(role<3&&cam_irq.layout.intid[role]!=intid)role++;
    if(role==3){cam_irq_spin_drop(old);return false;}
    /* Before EOI, mask ONLY the actual line. A completed/error sample then
     * masks all. SOF remains masked for this request, hybrid stays available.
     * Never acknowledge sources in a dormant/quiescing/foreign lifetime. */
    qualification=cam_irq.accepting?cam_irq_config(true):0;
    cam_irq_wr(cam_irq_bank(role,0x180),cam_irq_bit(role));
    if(cam_irq.accepting) {
        struct cam_irq_event ev={.role=role,.error=qualification};
        if(cam_irq.deliveries[role]==UINT64_MAX)ev.error=EOVERFLOW;
        else cam_irq.deliveries[role]++;
        if(!ev.error)ev.error=cam_irq_power();
        if(!ev.error)cam_irq_sample(role,&ev);
        if(ev.error||(ev.frame.done_status&0x000f0000u)||ev.frame.err_status||
           (ev.frame.cq_status&~1u))cam_irq_mask_all();
        if(cam_irq.count==CAM_IRQ_RING) {
            if(!cam_irq.error)cam_irq.error=EOVERFLOW;
            cam_irq.accepting=false;cam_irq_mask_all();
        } else {
            cam_irq.queue[(cam_irq.head+cam_irq.count)%CAM_IRQ_RING]=ev;
            cam_irq.count++;
            if(ev.error){if(!cam_irq.error)cam_irq.error=ev.error;cam_irq.accepting=false;}
        }
        wake=true;
    }
    cam_irq_spin_drop(old);
    if(wake)thread_wakeup((event_t)&cam_irq.count);
    return true; /* sleh still performs its original EOI, exactly once */
}
static int cam_irq_unmask(unsigned role)
{
    boolean_t old=cam_irq_spin_take();
    uint64_t before;
    int e;
    if(!cam_irq.accepting||cam_irq.error){cam_irq_spin_drop(old);return ECANCELED;}
    before=cam_irq.deliveries[role];
    cam_irq_wr(cam_irq_bank(role,0x100),cam_irq_bit(role));cam_irq_spin_drop(old);
    e=cam_irq_wait_rwp();
    old=cam_irq_spin_take();
    if(!e&&!(cam_irq_rd(cam_irq.gic,cam_irq_bank(role,0x100))&cam_irq_bit(role))&&
       cam_irq.deliveries[role]==before)e=EIO;
    cam_irq_spin_drop(old);return e;
}
/* A first real completion can race the sensor stream-on ACK -> core flag
 * publication. Wait for that actual publication, never synthesize STREAMING.
 * stop invalidates/wakes this event BEFORE waiting for our worker pin. */
static int cam_irq_core_ready(const struct x9_camera_camsv_irq *f)
{
    uint64_t span,end;
    nanoseconds_to_absolutetime(500000000,&span);end=mach_absolute_time()+span;
    for(;;) {
        int e=0;
        lck_mtx_lock(&camera_lock);
        if((st.flags&X9_CAMERA_FAULT)||st.generation!=f->generation||
           st.sensor!=cam_irq.request.frame.sensor)e=ESTALE;
        else if(!(st.flags&X9_CAMERA_STREAMING))e=EAGAIN;
        if(e==EAGAIN&&mach_absolute_time()>=end)e=ETIMEDOUT;
        if(e!=EAGAIN){lck_mtx_unlock(&camera_lock);return e;}
        assert_wait_deadline((event_t)&st.generation,THREAD_UNINT,end);
        lck_mtx_unlock(&camera_lock);thread_block(THREAD_CONTINUE_NULL);
    }
}
static void __attribute__((noreturn)) cam_irq_worker(void *arg,wait_result_t result)
{
    (void)arg;(void)result;
    for(;;) {
        boolean_t old=cam_irq_spin_take();
        struct cam_irq_event ev;
        bool current,discard;
        int e;
        if(!cam_irq.count) {
            assert_wait((event_t)&cam_irq.count,THREAD_UNINT);
            cam_irq_spin_drop(old);thread_block(THREAD_CONTINUE_NULL);continue;
        }
        ev=cam_irq.queue[cam_irq.head];
        cam_irq.head=(cam_irq.head+1)%CAM_IRQ_RING;cam_irq.count--;cam_irq.busy=true;
        cam_irq_spin_drop(old);
        lck_mtx_lock(&camera_irq_lock);
        old=cam_irq_spin_take();current=cam_irq.accepting;e=cam_irq.error;
        cam_irq_spin_drop(old);
        if(!e)e=ev.error;
        discard=!e&&!current;
        if(discard)e=ECANCELED;
        if(!e)e=cam_irq_power();
        if(!e)e=cam_irq_wait_rwp();
        if(!e)e=cam_irq_config(false);
        if(!e&&ev.role==X9_CAM_IRQ_HYBRID) {
            if(!ev.frame.err_status&&!(ev.frame.cq_status&~1u)&&
               !(ev.frame.done_status&0x000f0000u)) {
                /* CQ-only/input events are not frame completion and need
                 * not wait for the core's sensor stream publication. */
                const struct x9_camera_camsv_dma *d=&cam_irq.slot->dma;
                e=d->check(d->context,d->lease,&cam_irq.slot->request);
                if(!e)e=EAGAIN;
            } else {
                e=ev.frame.err_status||(ev.frame.cq_status&~1u)?EIO:cam_irq_core_ready(&ev.frame);
                if(!e)e=cam_irq_power();
                if(!e)e=x9_camera_camsv_finish(cam_irq.slot,&ev.frame);
            }
            if(e==EAGAIN&&cam_irq.slot->state==X9_CAMSV_ARMED)e=cam_irq_unmask(X9_CAM_IRQ_HYBRID);
            else if(cam_irq.slot->state==X9_CAMSV_RETIRED) {
                old=cam_irq_spin_take();cam_irq.accepting=false;cam_irq_mask_all();
                cam_irq_spin_drop(old);
                if(e==EBUSY||e==ESTALE)e=0; /* qualified drop/stop; never replay or FBC/RCNT */
            }
        }
        if(e&&!discard) {
            e=cam_irq_fault(e);
            lck_mtx_lock(&camera_camsv_lock);
            if(cam_irq.slot->state==X9_CAMSV_ARMED||cam_irq.slot->state==X9_CAMSV_RETAINED)
                cam_camsv_retain(cam_irq.slot,e);
            lck_mtx_unlock(&camera_camsv_lock);
        }
        if(e||cam_irq.reported<32) {
        if(cam_irq.reported<32)cam_irq.reported++;
        printf("X9 CAMERA IRQ: epoch=%llu role=%u seq=%x/%x done=%x err=%x cq=%x "
            "sof=%x channel=%x debug=%x/%x/%x state=%u errno=%d (IAR sample, no DMA reuse)\n",
            cam_irq.epoch,ev.role,ev.frame.inner_sequence,ev.frame.outer_sequence,
            ev.frame.done_status,ev.frame.err_status,ev.frame.cq_status,ev.sof,ev.channel,
            ev.common,ev.top,ev.fifo,cam_irq.slot->state,e);
        }
        lck_mtx_unlock(&camera_irq_lock);
        old=cam_irq_spin_take();cam_irq.busy=false;cam_irq_spin_drop(old);
        thread_wakeup((event_t)&cam_irq.busy);
    }
}
static int cam_irq_topology(void)
{
    DTEntry chosen,gic;const void *p;unsigned n;
    struct x9_camera_irq_layout q;
    uint64_t r[4];
    if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess||
       SecureDTGetProperty(chosen,"x9-camera-irq-layout",&p,&n)!=kSuccess||!p||n!=64)return ENXIO;
    memcpy(&q,p,sizeof(q));
    if(!x9_camera_irq_layout_valid(&q))return ENXIO;
    /* The SAME relative GICD address consumed by pe_init_fiq, not a captured
     * dump fallback or an unrelated interrupt controller alias. */
    if(SecureDTLookupEntry(NULL,"/arm-io/gic",&gic)!=kSuccess||
       SecureDTGetProperty(gic,"reg",&p,&n)!=kSuccess||!p||n!=32)return ENXIO;
    memcpy(r,p,sizeof(r));
    if(r[0]!=q.gicd_base-0x400000ULL||r[1]!=q.gicd_bytes||
       r[2]!=0x06880000-0x400000ULL||r[3]!=0x200000)return ENXIO;
    cam_irq.layout=q;
    if(!cam_irq.gic)cam_irq.gic=ml_io_map(q.gicd_base,q.gicd_bytes);
    return cam_irq_mapping();
}
/* Called by the actual inactive CAMSV geometry path. No enable, status-read,
 * prior mutation or ownership flag. A missing live topology is not READY. */
static int cam_irq_prepare_locked(uint64_t epoch)
{
    int e;
    if(!epoch||cam_irq.prepared||epoch<=cam_irq.last_epoch)return ESTALE;
    e=cam_irq.error;if(e)return e;
    e=cam_sv_environment();if(e)return e;
    lck_mtx_lock(&camera_irq_lock);
    cam_irq.view[0]=cam_sv.view[0];cam_irq.view[1]=cam_sv.view[1];
    cam_irq.hwv=cam_mm.receive.hwv;cam_irq.cam_cg=cam_mm.cam_cg;
    cam_irq.sv_cg=cam_mm.sv_cg;cam_irq.top=cam_mm.top;
    cam_irq.fence=cam_mm.desc[X9_CAM_MM_CAM_MUX].effect_mask|
        cam_mm.desc[X9_CAM_MM_CAMTM_MUX].effect_mask|cam_mm.desc[X9_CAM_MM_SENINF_MUX].effect_mask;
    e=cam_irq_topology();if(!e)e=cam_irq_controller();if(!e)e=cam_irq_idle();
    if(!e&&!cam_irq.thread_started) {
        thread_t thread;
        if(kernel_thread_start(cam_irq_worker,NULL,&thread)!=KERN_SUCCESS)e=ENOMEM;
        else {thread_deallocate(thread);cam_irq.thread_started=true;}
    }
    if(!e)e=cam_irq_power();
    if(!e) {
        cam_irq.slot=&cam_irq.slots[cam_sv.status.sensor-1];
        cam_irq.epoch=epoch;cam_irq.last_epoch=epoch;
        cam_irq.prepared=true;cam_irq.close_attempted=false;
    }
    lck_mtx_unlock(&camera_irq_lock);return e;
}
static int cam_irq_guard_locked(uint64_t epoch)
{
    boolean_t old;
    int e;
    if(!cam_irq.prepared)return 0;
    if(epoch!=cam_irq.epoch)return ESTALE;
    old=cam_irq_spin_take();e=cam_irq.error;cam_irq_spin_drop(old);
    return e;
}
/* Actual normal controller must call before enabling a hardware source. The
 * source is still inactive, and no history is cleared to manufacture idle. */
static int cam_irq_begin_locked(uint64_t epoch,const struct x9_camera_camsv_request *request,
    const struct x9_camera_camsv_dma *d)
{
    struct x9_camera_camsv_request r;
    struct x9_camera_camsv_dma dma;
    const unsigned order[]={X9_CAM_IRQ_SOF,X9_CAM_IRQ_HYBRID,X9_CAM_IRQ_DEBUG};
    int e;
    boolean_t old;
    bool owns,arm_attempted=false;
    if(!request||!d)return EINVAL;
    r=*request;dma=*d;
    if(r.tag||r.group)return EINVAL;
    lck_mtx_lock(&camera_irq_lock);
    e=!cam_irq.prepared||cam_irq.epoch!=epoch||cam_irq.close_attempted?ESTALE:0;
    old=cam_irq_spin_take();
    if(!e&&(cam_irq.accepting||cam_irq.count||cam_irq.busy))e=EBUSY;
    if(!e)e=cam_irq.error;
    owns=cam_irq.owns;cam_irq_spin_drop(old);
    if(!e&&r.frame.sensor!=cam_sv.status.sensor)e=EINVAL;
    /* Refuse a new arm BEFORE changing S1 if the previous request has an
     * issued credit. Only the real future producer retirement can end it. */
    if(!e&&cam_sv.credit_issued)e=EBUSY;
    if(!e)e=cam_sv_environment();if(!e)e=cam_irq_mapping();
    if(!e)e=cam_irq_controller();if(!e)e=cam_irq_idle();
    if(!e&&owns)e=cam_irq_config(false);
    if(!e&&cam_irq.slot->state!=X9_CAMSV_EMPTY&&cam_irq.slot->state!=X9_CAMSV_RETIRED)e=EBUSY;
    if(!e) {
        arm_attempted=true;
        e=x9_camera_camsv_arm(cam_irq.slot,&r,&dma); /* real lease + per-page CIVAC */
    }
    /* Do not make a hardware source observable with stale address/cookie.
     * Both banks must consume ONLY this already-armed request's S1 IOVA
     * and FH_SPARE, then no-credit FBC_EN. No SOURCE/DMA/CQ/CMOS/VF enable
     * follows. The ONE credit command comes only after actual GIC publication;
     * unexpected counter changes are retained, never re-credited. */
    if(!e)e=cam_sv_request_prepare_locked(epoch,cam_irq.slot);
    if(!e&&!owns) {
        /* Capture all priors before any route/priority write. Never change
         * Group/ICFGR/CTLR, and no distributor-global initialization. */
        for(unsigned i=0;i<3;i++) {
            cam_irq.route_before[i]=cam_irq_route(i);
            cam_irq.priority_before[i]=cam_irq_priority(i);
        }
        cam_irq.route_expected=__builtin_arm_rsr64("MPIDR_EL1")&0xff00ffffffULL;
        for(unsigned i=0;i<3&&!e;i++) {
            e=cam_irq_mutation_guard();
            if(e)break;
            old=cam_irq_spin_take();cam_irq.owns=true;cam_irq_spin_drop(old);
            cam_irq.dirty_route|=1u<<i;cam_irq.uncertain_route|=1u<<i;
            *(volatile uint64_t *)(cam_irq.gic+0x6000+8*cam_irq.layout.intid[i])=cam_irq.route_expected;
            __asm__ volatile("dsb sy":::"memory");
            if(cam_irq_route(i)!=cam_irq.route_expected){e=EIO;break;}
            cam_irq.uncertain_route&=~(1u<<i);
            e=cam_irq_mutation_guard();if(e)break;
            cam_irq.dirty_priority|=1u<<i;cam_irq.uncertain_priority|=1u<<i;
            *(volatile uint8_t *)(cam_irq.gic+0x400+cam_irq.layout.intid[i])=CAM_IRQ_PRIORITY;
            __asm__ volatile("dsb sy":::"memory");
            if(cam_irq_priority(i)!=CAM_IRQ_PRIORITY){e=EIO;break;}
            cam_irq.uncertain_priority&=~(1u<<i);
        }
    }
    if(!e)e=dma.check(dma.context,dma.lease,&cam_irq.slot->request);
    if(!e)e=cam_sv_request_guard_locked(epoch,cam_irq.slot);
    if(!e)e=cam_irq_config(false);if(!e)e=cam_irq_power();
    if(!e) {
        old=cam_irq_spin_take();cam_irq.request=cam_irq.slot->request;cam_irq.sof_seen=false;
        memset(&cam_irq.sof_frame,0,sizeof(cam_irq.sof_frame));cam_irq.accepting=true;
        cam_irq_spin_drop(old);
        /* SOF first, then hybrid, then debug. Sources remain off; the real
         * controller must preserve this preparation-to-publication boundary. */
        for(unsigned n=0;n<3&&!e;n++) {
            /* Recheck the complete bound request and controller, not only
             * the current byte/line, before EACH publication store. */
            e=cam_sv_request_guard_locked(epoch,cam_irq.slot);
            if(!e)e=cam_irq_config(false);if(!e)e=cam_irq_power();
            if(!e)e=cam_irq_unmask(order[n]);
        }
    }
    if(!e)e=cam_sv_credit_issue_locked(epoch,cam_irq.slot);
    if(e&&arm_attempted&&cam_irq.slot->state!=X9_CAMSV_EMPTY&&cam_irq.slot->state!=X9_CAMSV_RETIRED) {
        e=cam_irq_fault(e);lck_mtx_lock(&camera_camsv_lock);
        cam_camsv_retain(cam_irq.slot,e);lck_mtx_unlock(&camera_camsv_lock);
    }
    lck_mtx_unlock(&camera_irq_lock);return e;
}
int x9_camera_camsv_irq_begin(uint64_t epoch,const struct x9_camera_camsv_request *r,
    const struct x9_camera_camsv_dma *d)
{
    int e;
    lck_mtx_lock(&camera_control_lock);e=cam_irq_begin_locked(epoch,r,d);
    lck_mtx_unlock(&camera_control_lock);return e;
}
int x9_camera_camsv_irq_state(uint64_t epoch,uint32_t *state,int *error)
{
    int e;
    boolean_t old;
    if(!state||!error)return EINVAL;
    lck_mtx_lock(&camera_irq_lock);e=cam_irq.prepared&&epoch==cam_irq.epoch?0:ESTALE;
    if(!e) {
        *state=cam_irq.slot->state;old=cam_irq_spin_take();*error=cam_irq.error;
        cam_irq_spin_drop(old);
    }
    lck_mtx_unlock(&camera_irq_lock);return e;
}
/* No caller-supplied drained assertion. Receivers invoke before ANY CAMSV/
 * OUTMUX/PHY/MM restoration. We drain queued+running worker pins and GIC,
 * not the RAW/AXI/SMMU namespace; an unfinished slot cannot release power. */
static int cam_irq_close_locked(uint64_t epoch)
{
    boolean_t old;
    bool owns;
    uint64_t span,end;
    int e=0;
    if(!cam_irq.prepared)return 0;
    lck_mtx_lock(&camera_irq_lock);
    if(epoch!=cam_irq.epoch||cam_irq.close_attempted){lck_mtx_unlock(&camera_irq_lock);return ESTALE;}
    cam_irq.close_attempted=true;
    old=cam_irq_spin_take();cam_irq.accepting=false;owns=cam_irq.owns;
    if(owns)cam_irq_mask_all();cam_irq_spin_drop(old);
    lck_mtx_unlock(&camera_irq_lock);
    nanoseconds_to_absolutetime(500000000,&span);end=mach_absolute_time()+span;
    for(;;) {
        bool pending;
        old=cam_irq_spin_take();pending=cam_irq.busy||cam_irq.count;
        if(!pending){cam_irq_spin_drop(old);break;}
        if(mach_absolute_time()>=end){cam_irq_spin_drop(old);e=ETIMEDOUT;break;}
        assert_wait_deadline((event_t)&cam_irq.busy,THREAD_UNINT,end);
        cam_irq_spin_drop(old);thread_block(THREAD_CONTINUE_NULL);
    }
    lck_mtx_lock(&camera_irq_lock);
    if(!e)e=cam_irq_mapping();if(!e)e=cam_irq_wait_rwp();
    if(!e)e=cam_irq_controller();
    /* EOI is performed by sleh AFTER filter return; don't infer it from a
     * queued callback. Two actual idle samples include PENDING/ACTIVE. Never
     * ICPENDR/ICACTIVER/EOI a source to erase unconsumed history here. */
    if(!e)e=cam_irq_wait_idle();
    if(!e&&cam_irq.slot->state!=X9_CAMSV_EMPTY&&cam_irq.slot->state!=X9_CAMSV_RETIRED)e=EBUSY;
    if(!e)e=cam_sv_environment();
    for(unsigned k=3;k&&!e;k--) {
        unsigned i=k-1;uint32_t bit=1u<<i;
        if(owns)e=cam_irq_mutation_guard();
        if(e)break;
        if(cam_irq.dirty_priority&bit) {
            uint8_t p=cam_irq_priority(i);
            if(p!=CAM_IRQ_PRIORITY&&(!(cam_irq.uncertain_priority&bit)||p!=cam_irq.priority_before[i])){e=ESTALE;break;}
            cam_irq.uncertain_priority|=bit;
            *(volatile uint8_t *)(cam_irq.gic+0x400+cam_irq.layout.intid[i])=cam_irq.priority_before[i];
            __asm__ volatile("dsb sy":::"memory");
            if(cam_irq_priority(i)!=cam_irq.priority_before[i]){e=EIO;break;}
            cam_irq.dirty_priority&=~bit;cam_irq.uncertain_priority&=~bit;
        }
        if(cam_irq.dirty_route&bit) {
            uint64_t r=cam_irq_route(i);
            e=cam_irq_mutation_guard();if(e)break;
            if(r!=cam_irq.route_expected&&(!(cam_irq.uncertain_route&bit)||r!=cam_irq.route_before[i])){e=ESTALE;break;}
            cam_irq.uncertain_route|=bit;
            *(volatile uint64_t *)(cam_irq.gic+0x6000+8*cam_irq.layout.intid[i])=cam_irq.route_before[i];
            __asm__ volatile("dsb sy":::"memory");
            if(cam_irq_route(i)!=cam_irq.route_before[i]){e=EIO;break;}
            cam_irq.dirty_route&=~bit;cam_irq.uncertain_route&=~bit;
        }
        e=cam_irq_controller();if(!e)e=cam_irq_idle();
    }
    if(!e) {
        old=cam_irq_spin_take();cam_irq.owns=false;cam_irq_spin_drop(old);
        cam_irq.prepared=false; /* thread/maps/history/S1 backing retained */
    } else cam_irq_fault(e);
    printf("X9 CAMERA IRQ CLOSE: epoch=%llu owns=%u state=%u restore=%d dirty=%x/%x "
        "uncertain=%x/%x (GIC/worker only, NOT RAW/AXI/map drain)\n",epoch,owns,
        cam_irq.slot->state,e,cam_irq.dirty_route,cam_irq.dirty_priority,
        cam_irq.uncertain_route,cam_irq.uncertain_priority);
    lck_mtx_unlock(&camera_irq_lock);return e;
}
