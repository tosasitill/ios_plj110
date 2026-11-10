/* Native Group1NS SPI570 -> mask -> thread -> qualified v3 ACK -> SEMA_AUDIO
 * DTOA. No global GIC reinitialization, generic interrupt replacement, host
 * polling, mailbox-to-core notification substitution or output inference. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>
#include <vm/pmap.h>
#include "x9_adsp_audio_irq.h"
#include "x9_adsp_mt6993.h"
#include "x9_dsp_shared.h"
#include "x9_audio_resources.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t,addr64_t);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *,int);
LCK_GRP_DECLARE(audio_irq_group,"x9-adsp-audio-irq");
LCK_MTX_DECLARE(audio_irq_lock,&audio_irq_group);
LCK_SPIN_DECLARE(audio_irq_spin,&audio_irq_group);
static struct {
    vm_offset_t gic;
    struct x9_adsp_audio_irq_layout layout;
    uint64_t ticket,child,route_saved,route_expected,deliveries;
    uint32_t generation,last_generation,error,dirty,uncertain;
    uint8_t priority_saved;
    struct x9_dsp_pcm_session *session;
    lck_mtx_t *session_lock;
    bool attempted,captured,quiesced,thread_started;
    /* spin protected; filter never dereferences a session or board/RAM. */
    bool owns,armed,pending,busy;
} irq;
enum { IRQ_BIT=1u<<(X9_ADSP_AUDIO_INTID%32), IRQ_BANK=X9_ADSP_AUDIO_INTID/32,
       GROUP=0x80+4*IRQ_BANK, ENABLE=0x100+4*IRQ_BANK,
       DISABLE=0x180+4*IRQ_BANK, PENDING=0x200+4*IRQ_BANK,
       CLEAR_PENDING=0x280+4*IRQ_BANK, ACTIVE=0x300+4*IRQ_BANK,
       CONFIG=0xc00+4*(X9_ADSP_AUDIO_INTID/16),
       PRIORITY=0x400+X9_ADSP_AUDIO_INTID, ROUTE=0x6000+8*X9_ADSP_AUDIO_INTID,
       DIRTY_ROUTE=1,DIRTY_PRIORITY=2 };
_Static_assert(sizeof(struct x9_adsp_audio_irq_layout)==32,"AUDIO IRQ metadata size");
static uint32_t rd(unsigned off)
{
    uint32_t v=*(volatile uint32_t *)(irq.gic+off);
    __asm__ volatile("dmb oshld":::"memory");return v;
}
static void wr(unsigned off,uint32_t value)
{
    *(volatile uint32_t *)(irq.gic+off)=value;
    __asm__ volatile("dsb sy":::"memory");
}
static uint64_t route(void)
{
    uint64_t v=*(volatile uint64_t *)(irq.gic+ROUTE);
    __asm__ volatile("dmb oshld":::"memory");return v;
}
static uint8_t priority(void){return *(volatile uint8_t *)(irq.gic+PRIORITY);}
static boolean_t spin_lock(void)
{
    boolean_t old=ml_set_interrupts_enabled(FALSE);lck_spin_lock(&audio_irq_spin);return old;
}
static void spin_unlock(boolean_t old)
{lck_spin_unlock(&audio_irq_spin);ml_set_interrupts_enabled(old);}
static int fault(int e)
{
    if(e==EAGAIN)e=EIO;
    if(!irq.error)irq.error=(uint32_t)e;
    if(irq.ticket)x9_audio_resources_fault(X9AudioResourceSpeaker,irq.ticket,e);
    return e;
}
static int mapping(void)
{
    if(PAGE_SIZE!=0x4000 || !irq.gic)return ENXIO;
    /* Same physical/Device mapping as the real pe_init_fiq GICD, not an
     * unrelated controller. Permanent; no callback may unmap this alias. */
    for(unsigned off=0;off<0x40000;off+=0x4000) {
        addr64_t a=(addr64_t)irq.gic+off;
        ppnum_t p=pmap_find_phys(kernel_pmap,a);
        if(!p || ptoa_64(p)+(a&PAGE_MASK)!=0x06800000ULL+off)return EFAULT;
    }
    return 0;
}
static int controller(void)
{
    int e=mapping();if(e)return e;
    /* NS CTLR: ARE_NS bit4 and EnableGrp1 bit0. Do not change security group,
     * trigger, distributor, redistributors or the timer/IPI configuration. */
    if((rd(0)&0x11)!=0x11 || (rd(0)&(1u<<31)) ||
       (((rd(4)&31)+1)*32)<=X9_ADSP_AUDIO_INTID || !(rd(GROUP)&IRQ_BIT) ||
       (rd(CONFIG)&(3u<<((X9_ADSP_AUDIO_INTID%16)*2))))return EBUSY;
    return 0;
}
static int wait_rwp(void)
{
    uint64_t ticks;nanoseconds_to_absolutetime(1000000,&ticks);
    uint64_t end=mach_absolute_time()+ticks;
    for(unsigned i=0;i<500;i++) {
        if(!(rd(0)&(1u<<31)))return 0;
        if(mach_absolute_time()>=end)break;delay(2);
    }
    return ETIMEDOUT;
}
/* Readback may already be 0 because a NEW real IAR masked the SPI. Require
 * either enabled=1 or that independently observed delivery, never busy/time. */
static int unmask(void)
{
    boolean_t old=spin_lock();
    if(!irq.armed || irq.deliveries==UINT64_MAX){spin_unlock(old);return EOVERFLOW;}
    uint64_t before=irq.deliveries;wr(ENABLE,IRQ_BIT);spin_unlock(old);
    int e=wait_rwp();
    if(!e) {
        old=spin_lock();bool delivered=irq.deliveries!=before;
        bool enabled=(rd(ENABLE)&IRQ_BIT)!=0;spin_unlock(old);
        if(!enabled && !delivered)e=EIO;
    }
    return e;
}
static int verify_config(bool recovering)
{
    int e=controller();if(e)return e;
    if(!irq.captured)return ENXIO;
    uint64_t r=route();uint8_t p=priority();
    if(r!=irq.route_expected && !(recovering && (irq.uncertain&DIRTY_ROUTE) && r==irq.route_saved))return EBUSY;
    if(p!=0xa0 && !(recovering && (irq.uncertain&DIRTY_PRIORITY) && p==irq.priority_saved))return EBUSY;
    return 0;
}
static int ready(void)
{
    X9AudioDSPStatus s;int e=x9_adsp_status(0,&s);
    if(!e && (s.state!=X9DSPOnline || s.generation!=irq.generation || s.error))e=ENXIO;
    if(!e)e=x9_adsp_mt6993_speaker_validate(irq.ticket);
    if(!e)e=x9_dsp_shared_irq_validate(irq.session,irq.ticket,irq.generation);
    return e;
}
bool x9_adsp_audio_irq_filter(uint32_t intid)
{
    if(intid!=X9_ADSP_AUDIO_INTID)return false;
    boolean_t old=spin_lock();bool ours=irq.owns,wake=false;
    if(ours) {
        /* IAR is the event's provenance. Disable only this SPI before EOI.
         * Deferred ACK is safe because it stays masked throughout the thread. */
        wr(DISABLE,IRQ_BIT);
        if(irq.armed){if(irq.deliveries!=UINT64_MAX)irq.deliveries++;irq.pending=true;wake=true;}
    }
    spin_unlock(old);
    if(wake)thread_wakeup((event_t)&irq.pending);
    return ours;
}
static void worker_idle(void)
{
    boolean_t old=spin_lock();irq.busy=false;spin_unlock(old);
    thread_wakeup((event_t)&irq.busy);
}
static void worker(void *unused,wait_result_t result)
{
    (void)unused;(void)result;
    for(;;) {
        boolean_t old=spin_lock();
        if(!irq.pending) {
            assert_wait((event_t)&irq.pending,THREAD_UNINT);
            spin_unlock(old);thread_block(THREAD_CONTINUE_NULL);continue;
        }
        irq.pending=false;irq.busy=true;spin_unlock(old);
        /* Backend holds this SAME lock for PCM mutation and teardown. The
         * pointer remains pinned until release; a queued wake after quiesce
         * is discarded without reading board or shared memory. */
        lck_mtx_lock(&audio_irq_lock);lck_mtx_t *lock=irq.session_lock;
        lck_mtx_unlock(&audio_irq_lock);
        if(!lock){worker_idle();continue;}
        lck_mtx_lock(lock);lck_mtx_lock(&audio_irq_lock);
        old=spin_lock();bool armed=irq.armed;spin_unlock(old);
        if(armed && !irq.error) {
            int e=wait_rwp();
            if(!e)e=verify_config(false);
            if(!e)e=ready();
            if(!e)e=x9_adsp_mt6993_audio_irq_ack(irq.ticket,irq.child);
            if(!e)e=x9_dsp_shared_dtoa(irq.session);
            if(!e)e=ready();
            if(!e)e=verify_config(false);
            if(!e)e=unmask();
            if(e) {
                old=spin_lock();irq.armed=false;wr(DISABLE,IRQ_BIT);spin_unlock(old);
                fault(e);
            }
        }
        lck_mtx_unlock(&audio_irq_lock);lck_mtx_unlock(lock);worker_idle();
    }
}
static int topology(void)
{
    DTEntry chosen,gic;const void *p;unsigned n;
    struct x9_adsp_handoff h;
    if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess ||
       SecureDTGetProperty(chosen,"x9-adsp-audio-irq",&p,&n)!=kSuccess || !p || n!=32)return ENXIO;
    struct x9_adsp_audio_irq_layout q;memcpy(&q,p,sizeof(q));
    if(!x9_adsp_audio_irq_layout_valid(&q) || x9_adsp_mt6993_copy_handoff(&h) ||
       q.cfg_base!=h.cfg_base[0])return ENXIO;
    /* ADT uses pe_arm_get_soc_base_phys=0x400000 and relative reg addresses,
     * exactly the input consumed by pe_init_fiq, not old captured PA fallback. */
    if(SecureDTLookupEntry(NULL,"/arm-io/gic",&gic)!=kSuccess ||
       SecureDTGetProperty(gic,"reg",&p,&n)!=kSuccess || !p || n!=32)return ENXIO;
    const uint64_t *reg=p;
    if(reg[0]!=q.gicd_base-0x400000ULL || reg[1]!=q.gicd_bytes ||
       reg[2]!=0x06880000-0x400000ULL || reg[3]!=0x200000)return ENXIO;
    irq.layout=q;
    if(!irq.gic)irq.gic=ml_io_map(q.gicd_base,q.gicd_bytes);
    return mapping();
}
int x9_adsp_audio_irq_bind(uint64_t ticket,uint32_t generation,
    struct x9_dsp_pcm_session *session,lck_mtx_t *lock)
{
    if(!sweet_early_is_x9() || sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    if(!ticket || !generation || !session || !lock || lock==&audio_irq_lock)return EINVAL;
    lck_mtx_lock(lock);lck_mtx_lock(&audio_irq_lock);
    int e=irq.attempted || generation<=irq.last_generation?EBUSY:0;
    if(!e) {
        irq.ticket=ticket;irq.generation=generation;irq.last_generation=generation;
        irq.session=session;irq.session_lock=lock;irq.attempted=true;
        irq.error=0;irq.quiesced=false;
        e=ready();if(!e)e=topology();if(!e)e=controller();
        if(!e && ((rd(ENABLE)|rd(ACTIVE)|rd(PENDING))&IRQ_BIT))e=EBUSY;
        if(!e)e=x9_adsp_mt6993_audio_irq_hold(ticket,&irq.child);
        if(!e && !irq.thread_started) {
            thread_t thread;
            if(kernel_thread_start(worker,NULL,&thread)!=KERN_SUCCESS)e=ENOMEM;
            else {thread_deallocate(thread);irq.thread_started=true;}
        }
        if(!e)e=ready();
        if(!e)e=controller();
        if(!e && ((rd(ENABLE)|rd(ACTIVE)|rd(PENDING))&IRQ_BIT))e=EBUSY;
        if(!e) {
            irq.route_saved=route();irq.priority_saved=priority();irq.captured=true;
            /* Single PE affinity, IRM=0. Include AFF3, not guessed CPU0. */
            uint64_t mpidr=__builtin_arm_rsr64("MPIDR_EL1");
            irq.route_expected=mpidr&0xff00ffffffULL;
            irq.dirty|=DIRTY_ROUTE;irq.uncertain|=DIRTY_ROUTE;
            *(volatile uint64_t *)(irq.gic+ROUTE)=irq.route_expected;
            __asm__ volatile("dsb sy":::"memory");
            e=ready();if(!e)e=controller();if(!e && route()!=irq.route_expected)e=EIO;
            if(!e)irq.uncertain&=~DIRTY_ROUTE;
        }
        if(!e)e=ready();
        if(!e)e=controller();
        if(!e && (route()!=irq.route_expected || priority()!=irq.priority_saved))e=EBUSY;
        if(!e) {
            irq.dirty|=DIRTY_PRIORITY;irq.uncertain|=DIRTY_PRIORITY;
            *(volatile uint8_t *)(irq.gic+PRIORITY)=0xa0;
            __asm__ volatile("dsb sy":::"memory");
            e=ready();if(!e)e=controller();if(!e && priority()!=0xa0)e=EIO;
            if(!e)irq.uncertain&=~DIRTY_PRIORITY;
        }
        if(!e)e=ready();
        if(!e)e=verify_config(false);
        if(!e) {
            boolean_t old=spin_lock();irq.owns=true;irq.armed=true;spin_unlock(old);
            e=unmask();
        }
        if(e) {
            boolean_t old=spin_lock();irq.armed=false;
            if(irq.captured) {irq.owns=true;wr(DISABLE,IRQ_BIT);}spin_unlock(old);
            fault(e);
        }
    }
    lck_mtx_unlock(&audio_irq_lock);lck_mtx_unlock(lock);return e;
}
static bool binding_matches(uint64_t ticket,uint32_t generation,struct x9_dsp_pcm_session *session)
{return irq.attempted&&ticket&&ticket==irq.ticket&&generation&&generation==irq.generation&&session&&session==irq.session;}
int x9_adsp_audio_irq_validate(uint64_t ticket,uint32_t generation,struct x9_dsp_pcm_session *session)
{
    lck_mtx_lock(&audio_irq_lock);int e=binding_matches(ticket,generation,session)?0:EPERM;
    if(e){lck_mtx_unlock(&audio_irq_lock);return e;}
    boolean_t old=spin_lock();bool armed=irq.armed;spin_unlock(old);
    if(irq.error)e=(int)irq.error;
    if(!e&&(!armed||!irq.child||irq.quiesced))e=ENXIO;
    if(!e)e=ready();
    if(!e)e=verify_config(false);
    if(e)e=fault(e);
    lck_mtx_unlock(&audio_irq_lock);return e;
}
bool x9_adsp_audio_irq_binding_held(uint64_t ticket,uint32_t generation,struct x9_dsp_pcm_session *session)
{
    lck_mtx_lock(&audio_irq_lock);bool held=binding_matches(ticket,generation,session);
    lck_mtx_unlock(&audio_irq_lock);return held;
}
int x9_adsp_audio_irq_quiesce(uint64_t ticket,uint32_t generation,struct x9_dsp_pcm_session *session)
{
    /* First stop new delivery, WITHOUT holding the PCM lock the worker may
     * be queued on. Then drain its pin, not merely the running callback. */
    lck_mtx_lock(&audio_irq_lock);
    int e=binding_matches(ticket,generation,session)?0:EPERM;
    if(e){lck_mtx_unlock(&audio_irq_lock);return e;} /* No foreign mask. */
    if(!e) {
        boolean_t old=spin_lock();irq.armed=false;irq.pending=false;spin_unlock(old);
        if(irq.captured)e=mapping();
        if(!e && irq.captured) {old=spin_lock();wr(DISABLE,IRQ_BIT);spin_unlock(old);}
    }
    if(e && irq.attempted)e=fault(e);
    lck_mtx_unlock(&audio_irq_lock);
    if(e)return e;
    uint64_t ticks;nanoseconds_to_absolutetime(500000000,&ticks);
    uint64_t deadline=mach_absolute_time()+ticks;
    for(;;) {
        boolean_t old=spin_lock();
        if(!irq.busy){spin_unlock(old);break;}
        if(mach_absolute_time()>=deadline){spin_unlock(old);e=ETIMEDOUT;break;}
        assert_wait_deadline((event_t)&irq.busy,THREAD_UNINT,deadline);
        spin_unlock(old);thread_block(THREAD_CONTINUE_NULL);
    }
    lck_mtx_lock(&audio_irq_lock);
    if(!binding_matches(ticket,generation,session)) {
        lck_mtx_unlock(&audio_irq_lock);return EPERM;
    }
    if(e){e=fault(e);lck_mtx_unlock(&audio_irq_lock);return e;}
    if(irq.captured) {
        e=mapping(); /* Requalify after sleeping for a queued worker pin. */
        if(!e)e=wait_rwp();
        if(!e)e=controller();
        if(!e && (rd(ENABLE)&IRQ_BIT))e=EIO;
        if(!e) {
            for(unsigned i=0;i<500 && (rd(ACTIVE)&IRQ_BIT);i++)delay(2);
            if(rd(ACTIVE)&IRQ_BIT)e=ETIMEDOUT;
        }
    }
    if(!e && irq.child) {
        e=x9_adsp_mt6993_audio_irq_quiesce(irq.ticket,irq.child);
    }
    if(!e)irq.quiesced=true;
    if(e)e=fault(e);
    lck_mtx_unlock(&audio_irq_lock);return e;
}
int x9_adsp_audio_irq_release(uint64_t ticket,uint32_t generation,struct x9_dsp_pcm_session *session)
{
    lck_mtx_lock(&audio_irq_lock);
    boolean_t saved=spin_lock();bool busy=irq.busy;spin_unlock(saved);
    int e=binding_matches(ticket,generation,session)?0:EPERM;
    if(e){lck_mtx_unlock(&audio_irq_lock);return e;} /* No foreign restore/drop. */
    if(!irq.quiesced||busy)e=EBUSY;
    if(!e)e=x9_adsp_mt6993_speaker_halted_validate(irq.ticket);
    if(!e && irq.captured) {
        e=controller();
        if(!e && ((rd(ENABLE)|rd(ACTIVE))&IRQ_BIT))e=EBUSY;
        if(!e) {wr(CLEAR_PENDING,IRQ_BIT);if(rd(PENDING)&IRQ_BIT)e=EIO;}
        if(!e && (irq.dirty&DIRTY_PRIORITY)) {
            uint8_t p=priority();
            if(p!=0xa0 && p!=irq.priority_saved)e=EBUSY;
            else {
                irq.uncertain|=DIRTY_PRIORITY;
                *(volatile uint8_t *)(irq.gic+PRIORITY)=irq.priority_saved;
                __asm__ volatile("dsb sy":::"memory");
                e=controller();if(!e && priority()!=irq.priority_saved)e=EIO;
                if(!e){irq.dirty&=~DIRTY_PRIORITY;irq.uncertain&=~DIRTY_PRIORITY;}
            }
        }
        if(!e && (irq.dirty&DIRTY_ROUTE)) {
            uint64_t r=route();
            if(r!=irq.route_expected && r!=irq.route_saved)e=EBUSY;
            else {
                irq.uncertain|=DIRTY_ROUTE;
                *(volatile uint64_t *)(irq.gic+ROUTE)=irq.route_saved;
                __asm__ volatile("dsb sy":::"memory");
                e=controller();if(!e && route()!=irq.route_saved)e=EIO;
                if(!e){irq.dirty&=~DIRTY_ROUTE;irq.uncertain&=~DIRTY_ROUTE;}
            }
        }
    }
    if(!e && (irq.dirty || irq.uncertain))e=EBUSY;
    if(!e && irq.child) {
        e=x9_adsp_mt6993_audio_irq_drop(irq.ticket,irq.child);
        if(!e)irq.child=0;
    }
    if(!e) {
        boolean_t old=spin_lock();irq.owns=false;irq.armed=false;irq.pending=false;spin_unlock(old);
        irq.attempted=false;irq.captured=false;irq.session=NULL;irq.session_lock=NULL;
        /* mapping/thread/last generation and first fault remain retained. */
    }
    if(e && irq.attempted)e=fault(e);
    lck_mtx_unlock(&audio_irq_lock);return e;
}
