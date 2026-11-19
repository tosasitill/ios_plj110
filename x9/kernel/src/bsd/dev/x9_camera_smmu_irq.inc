/* MM SMMU combined edge IRQ. Hard IRQ masks only its owned GIC SPI;
 * all SMMU registers and the device-written EVTQ are serviced by a worker. */
#include <machine/machine_routines.h>
LCK_SPIN_DECLARE(camera_smmu_irq_spin,&camera_group);
enum { SMMU_IRQ_ID=0x1df+32, SMMU_IRQ_BIT=1u<<(SMMU_IRQ_ID%32),
    SMMU_IRQ_ENABLE=0x100+4*(SMMU_IRQ_ID/32),
    SMMU_IRQ_DISABLE=0x180+4*(SMMU_IRQ_ID/32),
    SMMU_IRQ_PENDING=0x200+4*(SMMU_IRQ_ID/32),
    SMMU_IRQ_ACTIVE=0x300+4*(SMMU_IRQ_ID/32),
    SMMU_IRQ_CONFIG=0xc00+4*(SMMU_IRQ_ID/16),
    SMMU_IRQ_SHIFT=(SMMU_IRQ_ID%16)*2,
    SMMU_IRQ_PRIORITY=0x400+SMMU_IRQ_ID, SMMU_IRQ_ROUTE=0x6000+8*SMMU_IRQ_ID };
static struct {
    vm_offset_t gic;
    struct x9_camera_smmu *owner;
    uint64_t saved_route,route,deliveries;
    uint32_t saved_config,dirty;
    uint8_t saved_priority;
    bool owns,armed,pending,busy,thread_started;
} cam_smmu_irq;
static boolean_t cam_smmu_irq_spin_take(void)
{
    boolean_t old=ml_set_interrupts_enabled(FALSE);
    lck_spin_lock(&camera_smmu_irq_spin);return old;
}
static void cam_smmu_irq_spin_drop(boolean_t old)
{lck_spin_unlock(&camera_smmu_irq_spin);ml_set_interrupts_enabled(old);}
static uint32_t cam_smmu_irq_read(unsigned off)
{
    uint32_t v=*(volatile uint32_t *)(cam_smmu_irq.gic+off);
    __asm__ volatile("dmb oshld":::"memory");return v;
}
static void cam_smmu_irq_write(unsigned off,uint32_t v)
{
    *(volatile uint32_t *)(cam_smmu_irq.gic+off)=v;
    __asm__ volatile("dsb sy":::"memory");
}
static uint64_t cam_smmu_irq_route(void)
{
    uint64_t v=*(volatile uint64_t *)(cam_smmu_irq.gic+SMMU_IRQ_ROUTE);
    __asm__ volatile("dmb oshld":::"memory");return v;
}
static int cam_smmu_irq_wait_rwp(void)
{
    uint64_t span,end;nanoseconds_to_absolutetime(1000000,&span);end=mach_absolute_time()+span;
    for(unsigned i=0;i<500;i++) {
        if(!(cam_smmu_irq_read(0)&(1u<<31)))return 0;
        if(mach_absolute_time()>=end)return ETIMEDOUT;
        delay(2);
    }
    return ETIMEDOUT;
}
static int cam_smmu_irq_topology(void)
{
    DTEntry chosen,gic;const void *p;unsigned n;
    struct x9_camera_smmu_irq_layout q;uint64_t reg[4];
    if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess ||
       SecureDTGetProperty(chosen,"x9-camera-smmu-irq",&p,&n)!=kSuccess || !p || n!=sizeof(q))return ENXIO;
    memcpy(&q,p,sizeof(q));if(!x9_camera_smmu_irq_layout_valid(&q))return EINVAL;
    if(SecureDTLookupEntry(NULL,"/arm-io/gic",&gic)!=kSuccess ||
       SecureDTGetProperty(gic,"reg",&p,&n)!=kSuccess || !p || n!=sizeof(reg))return ENXIO;
    memcpy(reg,p,sizeof(reg));
    if(reg[0]!=q.gicd_base-0x400000ULL || reg[1]!=q.gicd_bytes ||
       reg[2]!=0x06880000-0x400000ULL || reg[3]!=0x200000)return ENXIO;
    if(!cam_smmu_irq.gic)cam_smmu_irq.gic=ml_io_map(q.gicd_base,q.gicd_bytes);
    if(!cam_smmu_irq.gic || PAGE_SIZE!=0x4000)return ENXIO;
    for(unsigned off=0;off<0x40000;off+=0x4000) {
        addr64_t a=(addr64_t)cam_smmu_irq.gic+off;ppnum_t page=pmap_find_phys(kernel_pmap,a);
        if(!page || ptoa_64(page)+(a&PAGE_MASK)!=0x06800000ULL+off)return EFAULT;
    }
    if((cam_smmu_irq_read(0)&0x12)!=0x12 || (cam_smmu_irq_read(0)&(1u<<31)) ||
       ((cam_smmu_irq_read(4)&31u)+1u)*32u<=SMMU_IRQ_ID)return EBUSY;
    return 0;
}
/* Same full-bank comparison as CR0 enable; IRQ_CTRLACK is allowed to move
 * only from the old snapshot to the requested value during this operation. */
static int cam_smmu_irq_control_locked(struct x9_camera_smmu *m,struct cam_native_snapshot *s,uint32_t value)
{
    uint64_t span,end;uint32_t before;int e=cam_native_match(m,s);if(e)return e;
    before=s->word[CAM_N_IRQACK];m->native_irq_ctrl=value;
    cam_smmu_rw(m,0x50,value);s->word[CAM_N_IRQ]=value;
    nanoseconds_to_absolutetime(10000000,&span);end=mach_absolute_time()+span;
    for(unsigned i=0;i<10000;i++) {
        struct cam_native_snapshot now;uint32_t ack;
        e=x9_mm_power_validate(m->native_power,X9_MM_POWER_CAMERA);if(e)return e;
        cam_native_read(m,&now);ack=now.word[CAM_N_IRQACK];
        if(ack!=before && ack!=value)return ESTALE;
        now.word[CAM_N_IRQACK]=s->word[CAM_N_IRQACK];
        if(memcmp(&now,s,sizeof(now)))return ESTALE;
        if(ack==value){s->word[CAM_N_IRQACK]=value;return 0;}
        if(mach_absolute_time()>=end)return ETIMEDOUT;
        delay(1);
    }
    return ETIMEDOUT;
}
static bool cam_smmu_irq_filter(uint32_t intid)
{
    boolean_t old;bool ours,wake=false;
    if(intid!=SMMU_IRQ_ID)return false;
    old=cam_smmu_irq_spin_take();ours=cam_smmu_irq.owns;
    if(ours) {
        cam_smmu_irq_write(SMMU_IRQ_DISABLE,SMMU_IRQ_BIT);
        if(cam_smmu_irq.armed) {
            if(cam_smmu_irq.deliveries!=UINT64_MAX)cam_smmu_irq.deliveries++;
            cam_smmu_irq.pending=true;wake=true;
        }
    }
    cam_smmu_irq_spin_drop(old);
    if(wake)thread_wakeup((event_t)&cam_smmu_irq.pending);
    return ours;
}
static void __attribute__((noreturn)) cam_smmu_irq_worker(void *unused,wait_result_t result)
{
    (void)unused;(void)result;
    for(;;) {
        boolean_t old=cam_smmu_irq_spin_take();
        struct x9_camera_smmu *m;bool armed;int e=0;
        uint64_t deliveries,events=0;uint32_t gerror=0;
        if(!cam_smmu_irq.pending) {
            assert_wait((event_t)&cam_smmu_irq.pending,THREAD_UNINT);
            cam_smmu_irq_spin_drop(old);thread_block(THREAD_CONTINUE_NULL);continue;
        }
        cam_smmu_irq.pending=false;cam_smmu_irq.busy=true;
        m=cam_smmu_irq.owner;deliveries=cam_smmu_irq.deliveries;
        armed=cam_smmu_irq.armed;cam_smmu_irq_spin_drop(old);
        if(armed) {
            lck_mtx_lock(&camera_smmu_lock);
            e=cam_smmu_identity(m);
            if(!e)e=cam_smmu_storage(m);
            if(!e)e=cam_smmu_live(m);
            if(e && m && m==cam_smmu_namespace && m->exported)e=cam_smmu_latch(m,e);
            if(m && m==cam_smmu_namespace){events=m->evt_events;gerror=m->gerror^m->gerrorn;}
            lck_mtx_unlock(&camera_smmu_lock);
        }
        if(e || deliveries<=8)
            printf("X9 CAMERA SMMU IRQ: IAR deliveries=%llu events=%llu gerror=%x armed=%u errno=%d\n",
                deliveries,events,gerror,armed,e);
        if(e)cam_smmu_irq_notify_fault(e);
        old=cam_smmu_irq_spin_take();
        if(e)cam_smmu_irq.armed=false;
        if(cam_smmu_irq.armed && cam_smmu_irq.owner==m)
            cam_smmu_irq_write(SMMU_IRQ_ENABLE,SMMU_IRQ_BIT);
        cam_smmu_irq.busy=false;cam_smmu_irq_spin_drop(old);
        thread_wakeup((event_t)&cam_smmu_irq.busy);
    }
}
/* Recheck the sole SPI's prior/installed state before every setup store.
 * These observations are necessary ownership checks, not an atomic lease. */
static int cam_smmu_irq_setup_guard_locked(struct x9_camera_smmu *m,struct cam_native_snapshot *s)
{
    int e=cam_native_match(m,s);if(e)return e;
    if((cam_smmu_irq_read(0)&0x12)!=0x12 || (cam_smmu_irq_read(0)&(1u<<31)) ||
       ((cam_smmu_irq_read(SMMU_IRQ_ENABLE)|cam_smmu_irq_read(SMMU_IRQ_PENDING)|
         cam_smmu_irq_read(SMMU_IRQ_ACTIVE))&SMMU_IRQ_BIT))return EBUSY;
    if((cam_smmu_irq_read(SMMU_IRQ_CONFIG)&(3u<<SMMU_IRQ_SHIFT))!=
       ((cam_smmu_irq.dirty&1)?(2u<<SMMU_IRQ_SHIFT):cam_smmu_irq.saved_config) ||
       cam_smmu_irq_route()!=((cam_smmu_irq.dirty&2)?cam_smmu_irq.route:cam_smmu_irq.saved_route) ||
       *(volatile uint8_t *)(cam_smmu_irq.gic+SMMU_IRQ_PRIORITY)!=
       ((cam_smmu_irq.dirty&4)?0xa0:cam_smmu_irq.saved_priority))return ESTALE;
    return 0;
}
/* Called with camera_smmu_lock held after EVTQ is ready, before SMMUEN. */
static int cam_smmu_irq_bind_impl_locked(struct x9_camera_smmu *m,struct cam_native_snapshot *s,const char **step)
{
    int e;boolean_t old;uint32_t config;uint64_t before;
    *step="topology";e=cam_smmu_irq_topology();
    if(!e)e=cam_native_match(m,s);
    if(e)return e;
    *step="admission";old=cam_smmu_irq_spin_take();
    if(cam_smmu_irq.owns || cam_smmu_irq.pending || cam_smmu_irq.busy)e=EBUSY;
    cam_smmu_irq_spin_drop(old);if(e)return e;
    if((cam_smmu_irq_read(SMMU_IRQ_ENABLE)|cam_smmu_irq_read(SMMU_IRQ_PENDING)|
        cam_smmu_irq_read(SMMU_IRQ_ACTIVE))&SMMU_IRQ_BIT)return EBUSY;
    *step="worker";
    if(!cam_smmu_irq.thread_started) {
        thread_t thread;
        if(kernel_thread_start(cam_smmu_irq_worker,NULL,&thread)!=KERN_SUCCESS)return ENOMEM;
        thread_deallocate(thread);cam_smmu_irq.thread_started=true;
    }
    cam_smmu_irq.saved_route=cam_smmu_irq_route();
    cam_smmu_irq.saved_priority=*(volatile uint8_t *)(cam_smmu_irq.gic+SMMU_IRQ_PRIORITY);
    cam_smmu_irq.saved_config=cam_smmu_irq_read(SMMU_IRQ_CONFIG)&(3u<<SMMU_IRQ_SHIFT);
    cam_smmu_irq.route=__builtin_arm_rsr64("MPIDR_EL1")&0xff00ffffffULL;
    old=cam_smmu_irq_spin_take();cam_smmu_irq.owner=m;cam_smmu_irq.owns=true;
    cam_smmu_irq.armed=false;cam_smmu_irq.pending=false;cam_smmu_irq.dirty=0;
    cam_smmu_irq_spin_drop(old);
    /* Linux programs IRQ_TYPE_EDGE_RISING from the live DT. Change only this
     * SPI's ICFGR bits and retain the previous route/priority/config. */
    *step="edge-config";e=cam_smmu_irq_setup_guard_locked(m,s);if(e)return e;
    config=cam_smmu_irq_read(SMMU_IRQ_CONFIG);
    cam_smmu_irq.dirty|=1;
    cam_smmu_irq_write(SMMU_IRQ_CONFIG,(config&~(3u<<SMMU_IRQ_SHIFT))|(2u<<SMMU_IRQ_SHIFT));
    e=cam_smmu_irq_wait_rwp();if(e)return e;
    if((cam_smmu_irq_read(SMMU_IRQ_CONFIG)&(3u<<SMMU_IRQ_SHIFT))!=(2u<<SMMU_IRQ_SHIFT))return EIO;
    *step="route";e=cam_smmu_irq_setup_guard_locked(m,s);if(e)return e;
    cam_smmu_irq.dirty|=2;
    *(volatile uint64_t *)(cam_smmu_irq.gic+SMMU_IRQ_ROUTE)=cam_smmu_irq.route;
    __asm__ volatile("dsb sy":::"memory");
    if(cam_smmu_irq_route()!=cam_smmu_irq.route)return EIO;
    *step="priority";e=cam_smmu_irq_setup_guard_locked(m,s);if(e)return e;
    cam_smmu_irq.dirty|=4;
    *(volatile uint8_t *)(cam_smmu_irq.gic+SMMU_IRQ_PRIORITY)=0xa0;
    __asm__ volatile("dsb sy":::"memory");
    if(*(volatile uint8_t *)(cam_smmu_irq.gic+SMMU_IRQ_PRIORITY)!=0xa0)return EIO;
    *step="source-ack";e=cam_smmu_irq_setup_guard_locked(m,s);if(e)return e;
    e=cam_smmu_irq_control_locked(m,s,5);if(e)return e; /* EVTQ + GERROR */
    *step="gic-enable";old=cam_smmu_irq_spin_take();
    if(cam_smmu_irq.deliveries==UINT64_MAX) {
        cam_smmu_irq_spin_drop(old);return EOVERFLOW;
    }
    cam_smmu_irq.armed=true;before=cam_smmu_irq.deliveries;
    cam_smmu_irq_write(SMMU_IRQ_ENABLE,SMMU_IRQ_BIT);cam_smmu_irq_spin_drop(old);
    e=cam_smmu_irq_wait_rwp();
    if(!e) {
        old=cam_smmu_irq_spin_take();
        if(!(cam_smmu_irq_read(SMMU_IRQ_ENABLE)&SMMU_IRQ_BIT) &&
           cam_smmu_irq.deliveries==before)e=EIO;
        cam_smmu_irq_spin_drop(old);
    }
    return e;
}
static int cam_smmu_irq_bind_locked(struct x9_camera_smmu *m,struct cam_native_snapshot *s)
{
    const char *step="entry";
    int e=cam_smmu_irq_bind_impl_locked(m,s,&step);
    printf("X9 CAMERA SMMU IRQ: bind step=%s intid=%u edge=1 route=%llx dirty=%x errno=%d\n",
        step,SMMU_IRQ_ID,cam_smmu_irq.route,cam_smmu_irq.dirty,e);
    return e;
}
/* Source off before GIC retirement. This runs without camera_smmu_lock so a
 * worker that already observed the IAR can finish; never clear pending bits. */
static int cam_smmu_irq_shutdown(struct x9_camera_smmu *m)
{
    boolean_t old=cam_smmu_irq_spin_take();bool ours=cam_smmu_irq.owns;
    bool match=cam_smmu_irq.owner==m;int e;uint64_t span,end;
    cam_smmu_irq_spin_drop(old);
    if(!ours)return 0;if(!match)return ESTALE;
    lck_mtx_lock(&camera_smmu_lock);
    e=x9_mm_power_validate(m->native_power,X9_MM_POWER_CAMERA);
    if(!e) {
        struct cam_native_snapshot now;cam_native_read(m,&now);
        if(now.word[CAM_N_IRQ] || now.word[CAM_N_IRQACK])
            e=cam_smmu_irq_control_locked(m,&now,0);
    }
    lck_mtx_unlock(&camera_smmu_lock);if(e)return e;
    nanoseconds_to_absolutetime(100000000,&span);end=mach_absolute_time()+span;
    for(;;) {
        bool busy,hardware_pending;
        old=cam_smmu_irq_spin_take();
        busy=cam_smmu_irq.busy || cam_smmu_irq.pending;
        hardware_pending=((cam_smmu_irq_read(SMMU_IRQ_PENDING)|cam_smmu_irq_read(SMMU_IRQ_ACTIVE))&SMMU_IRQ_BIT)!=0;
        if(!busy && !hardware_pending) {
            cam_smmu_irq.armed=false;cam_smmu_irq_write(SMMU_IRQ_DISABLE,SMMU_IRQ_BIT);
            cam_smmu_irq_spin_drop(old);break;
        }
        cam_smmu_irq_spin_drop(old);
        if(mach_absolute_time()>=end)return ETIMEDOUT;
        delay(10);
    }
    e=cam_smmu_irq_wait_rwp();if(e)return e;
    old=cam_smmu_irq_spin_take();
    if(cam_smmu_irq.busy || cam_smmu_irq.pending ||
       ((cam_smmu_irq_read(SMMU_IRQ_ENABLE)|cam_smmu_irq_read(SMMU_IRQ_PENDING)|
         cam_smmu_irq_read(SMMU_IRQ_ACTIVE))&SMMU_IRQ_BIT))e=EBUSY;
    cam_smmu_irq_spin_drop(old);if(e)return e;
    if(cam_smmu_irq.dirty&4) {
        uint8_t current=*(volatile uint8_t *)(cam_smmu_irq.gic+SMMU_IRQ_PRIORITY);
        if(current!=0xa0 && current!=cam_smmu_irq.saved_priority)return ESTALE;
        *(volatile uint8_t *)(cam_smmu_irq.gic+SMMU_IRQ_PRIORITY)=cam_smmu_irq.saved_priority;
        __asm__ volatile("dsb sy":::"memory");
        if(*(volatile uint8_t *)(cam_smmu_irq.gic+SMMU_IRQ_PRIORITY)!=cam_smmu_irq.saved_priority)return EIO;
    }
    if(cam_smmu_irq.dirty&2) {
        uint64_t current=cam_smmu_irq_route();
        if(current!=cam_smmu_irq.route && current!=cam_smmu_irq.saved_route)return ESTALE;
        *(volatile uint64_t *)(cam_smmu_irq.gic+SMMU_IRQ_ROUTE)=cam_smmu_irq.saved_route;
        __asm__ volatile("dsb sy":::"memory");
        if(cam_smmu_irq_route()!=cam_smmu_irq.saved_route)return EIO;
    }
    if(cam_smmu_irq.dirty&1) {
        uint32_t config=cam_smmu_irq_read(SMMU_IRQ_CONFIG);
        uint32_t current=config&(3u<<SMMU_IRQ_SHIFT);
        if(current!=(2u<<SMMU_IRQ_SHIFT) && current!=cam_smmu_irq.saved_config)return ESTALE;
        cam_smmu_irq_write(SMMU_IRQ_CONFIG,(config&~(3u<<SMMU_IRQ_SHIFT))|cam_smmu_irq.saved_config);
        e=cam_smmu_irq_wait_rwp();if(e)return e;
        if((cam_smmu_irq_read(SMMU_IRQ_CONFIG)&(3u<<SMMU_IRQ_SHIFT))!=cam_smmu_irq.saved_config)return EIO;
    }
    old=cam_smmu_irq_spin_take();cam_smmu_irq.owns=false;cam_smmu_irq.owner=NULL;
    cam_smmu_irq.dirty=0;cam_smmu_irq_spin_drop(old);
    printf("X9 CAMERA SMMU IRQ: retired intid=%u deliveries=%llu\n",SMMU_IRQ_ID,cam_smmu_irq.deliveries);
    return 0;
}
