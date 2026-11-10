/* OPPO tfa98xx_ext_reset polarity0: LOW 1ms -> HIGH 1ms. MT6661 pinctrl
 * DIR0x8b, DO0xa3, DI0xa7 bits2/3; MODE0xb9 fields0/3; SW digital0xd5/d6.
 * GPIO10/11 only. No DIR/mux/key/drive/rail write, no arbitrary SPMI API.
 * Existing read-only PA probe is not the clock/rail/fresh-reset authority. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <vm/pmap.h>
#include "x9_audio_resources.h"
#include "x9_adsp_mt6993.h"
#include "x9_adsp_transport.h"
#include "x9_speaker_mt6993.h"
#include "x9_pa_reset.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t,addr64_t);
extern int X9PMIFAcquire(void);
extern void X9PMIFRelease(void);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *,int);
LCK_GRP_DECLARE(pa_reset_group,"x9-pa-reset");
LCK_MTX_DECLARE(pa_reset_lock,&pa_reset_group);
enum { PendingNone,PendingRead,PendingWrite,PendingClear };
static struct {
    struct x9_pa_reset_policy policy;
    vm_offset_t pmif;
    uint64_t child,epoch,serial,bus_child;
    uint32_t last_generation,error,pending;
    uint8_t expected,prior;
    bool active,captured,configured,uncertain,dirty,asserted;
} pa;
static uint32_t rr(unsigned off)
{
    uint32_t v=*(volatile uint32_t *)(pa.pmif+off);
    __asm__ volatile("dmb oshld":::"memory");return v;
}
static void wr(unsigned off,uint32_t v)
{
    __asm__ volatile("dmb oshst":::"memory");
    *(volatile uint32_t *)(pa.pmif+off)=v;
    __asm__ volatile("dsb sy":::"memory");
}
static int fault(int e)
{
    if(e==EAGAIN)e=EIO;
    if(!pa.error)pa.error=(uint32_t)e;
    x9_audio_resources_fault(X9AudioResourceSpeaker,pa.policy.ticket,e);return e;
}
static int parent(void)
{return x9_speaker_mt6993_clock_pa_validate(pa.policy.ticket,pa.child);}
static int mapping(void)
{
    if(PAGE_SIZE!=0x4000||!pa.pmif)return ENXIO;
    for(unsigned i=0;i<2;i++) {
        addr64_t a=(addr64_t)pa.pmif+(i?0x1fff:0);
        ppnum_t p=pmap_find_phys(kernel_pmap,a);
        if(!p||ptoa_64(p)+(a&PAGE_MASK)!=0x1c018000ULL+(i?0x1fff:0))return EFAULT;
    }
    return 0;
}
static int access(void)
{
    int e=parent();if(!e)e=mapping();
    if(!e&&!(rr(0)&1))e=ENXIO;
    return e;
}
static int power(void)
{
    int e=parent();if(!e)e=pa.policy.validate_power(pa.policy.context,pa.policy.ticket,pa.policy.generation);
    if(!e)e=parent();return e;
}
/* All helpers below require PA reset lock AND common PMIF mutex. Parent
 * validation takes no PMIF lock/callback, so there is no recursive rail read. */
static int wait_state(unsigned wanted)
{
    uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);
    for(unsigned i=0;i<1000;i++) {
        int e=access();if(e)return e;
        uint32_t s=rr(0x8a8);if(s&(1u<<18))return EIO;
        if(((s>>1)&7)==wanted)return 0;
        if(mach_absolute_time()>=deadline)break;delay(1);
    }
    return ETIMEDOUT;
}
static int read_byte(unsigned reg,uint8_t *out)
{
    if(pa.pending)return EBUSY;
    int e=wait_state(0);if(e)return e;
    pa.pending=PendingRead;wr(0x880,(3u<<30)|(3u<<24)|reg);
    e=wait_state(6);if(e)return e;
    uint8_t v=(uint8_t)rr(0x894);
    pa.pending=PendingClear;wr(0x8a4,1);e=wait_state(0);
    if(!e){pa.pending=PendingNone;*out=v;}return e;
}
static int write_do(uint8_t value)
{
    if(pa.pending)return EBUSY;
    int e=wait_state(0);if(e)return e;
    /* MT6661 FLAG_MT66XX uses regmap_update_bits, not SoC SET/CLR aliases.
     * Caller observed full DO under this PMIF mutex and preserves neighbors. */
    pa.pending=PendingWrite;wr(0x884,value);
    wr(0x880,(3u<<30)|(1u<<29)|(3u<<24)|0xa3);
    e=wait_state(0);if(!e)pa.pending=PendingNone;return e;
}
static int resolve_pending(void)
{
    if(!pa.pending)return 0;
    int e=access();if(e)return e;
    uint32_t s=rr(0x8a8);if(s&(1u<<18))return EIO;
    unsigned fsm=(s>>1)&7;
    if(pa.pending==PendingRead) {
        /* Only THIS owner's actual late read VALID may be drained. Idle
         * cannot prove that a timed-out read won't produce a future result. */
        if(fsm!=6)return EBUSY;
        (void)rr(0x894);pa.pending=PendingClear;wr(0x8a4,1);
        e=wait_state(0);if(e)return e;
    } else if(fsm!=0)return EBUSY; /* no timeout write/CLEAR replay */
    pa.pending=PendingNone;return 0;
}
struct reset_image {uint8_t dir,dout,din,mode,sw,digital;};
static int image(struct reset_image *s)
{
    int e=read_byte(0x8b,&s->dir);
    if(!e)e=read_byte(0xa3,&s->dout);if(!e)e=read_byte(0xa7,&s->din);
    if(!e)e=read_byte(0xb9,&s->mode);if(!e)e=read_byte(0xd5,&s->sw);
    if(!e)e=read_byte(0xd6,&s->digital);return e;
}
static int ownership(const struct reset_image *s)
{
    return (s->dir&12)!=12||(s->mode&63)||(s->sw&12)!=12||
        (s->digital&12)!=12?EBUSY:0;
}
static int verify(bool recovering,struct reset_image *s)
{
    int e=image(s);if(!e)e=ownership(s);if(e)return e;
    uint8_t v=s->dout&12;
    if(v!=(pa.expected&12)&&!(recovering&&pa.uncertain&&v==(pa.prior&12)))return EBUSY;
    if((s->din&12)!=v)return EBUSY; /* actual pad, not just software DO */
    return 0;
}
static int set_reset(uint8_t wanted)
{
    struct reset_image s={0};int e=verify(true,&s);if(e)return e;
    /* Re-read the RMW source immediately before this write. Unknown PA bits
     * cannot be overwritten even if the earlier six-register image passed. */
    uint8_t old;e=read_byte(0xa3,&old);if(e)return e;
    if((old&12)!=(pa.expected&12)&&!(pa.uncertain&&(old&12)==(pa.prior&12)))return EBUSY;
    uint8_t next=(old&~12u)|(wanted&12);
    if((old&12)==(next&12)){pa.expected=old;pa.uncertain=false;return 0;}
    pa.prior=old;pa.expected=next;pa.uncertain=pa.dirty=true;
    e=write_do(next);if(e)return e;
    e=verify(false,&s);if(!e)pa.uncertain=false;return e;
}
static int dwell(uint8_t level)
{
    /* Preserve an actual minimum 1ms low/high interval, not a reset flag.
     * Every loop freshly qualifies parent before any PMIF MMIO. */
    uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);
    unsigned i=0;
    while(mach_absolute_time()<deadline) {
        if(++i>1000)return ETIMEDOUT;
        int e=access();if(e)return e;delay(25);
    }
    struct reset_image s={0};int e=verify(false,&s);
    return e?e:((s.dout&12)!=level||(s.din&12)!=level?EIO:0);
}
static int online(void)
{
    X9AudioDSPStatus s={0};int e=x9_adsp_status(0,&s);
    return e?e:s.state!=X9DSPOnline||s.error||s.generation!=pa.policy.generation?ESTALE:0;
}
int x9_pa_reset_acquire(const struct x9_pa_reset_policy *p,uint64_t *epoch)
{
    if(!epoch)return EINVAL;*epoch=0;
    if(!p||p->version!=X9_PA_RESET_POLICY_VERSION||!p->ticket||!p->generation||
       !p->validate_power||!p->validate_drained)return EINVAL;
    if(!sweet_early_is_x9()||sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    lck_mtx_lock(&pa_reset_lock);
    int e=pa.active||p->generation<=pa.last_generation?EBUSY:0;
    X9AudioDSPStatus ds={0};if(!e)e=x9_adsp_mt6993_speaker_validate(p->ticket);
    if(!e)e=x9_adsp_status(0,&ds);
    if(!e&&(ds.state!=X9DSPOnline||ds.error||ds.generation!=p->generation))e=ESTALE;
    struct x9_adsp_handoff h={0};if(!e)e=x9_adsp_mt6993_copy_handoff(&h);
    if(!e&&!(h.flags&X9_ADSP_PA_RESET_VERIFIED))e=ENXIO;
    if(!e&&pa.serial==UINT64_MAX)e=EOVERFLOW;
    uint64_t child=0;if(!e)e=x9_speaker_mt6993_clock_pa_hold(p->ticket,&child);
    if(!e) {
        pa.policy=*p;pa.child=child;pa.active=true;pa.epoch=0;pa.last_generation=p->generation;
        pa.error=pa.pending=0;pa.captured=pa.configured=pa.uncertain=pa.dirty=pa.asserted=false;
        e=online();if(!e)e=power();if(!e)e=online();
        if(!e&&!pa.pmif)pa.pmif=ml_io_map(0x1c018000,0x2000);
        if(!e)e=mapping();
        bool locked=false;if(!e){e=X9PMIFAcquire()?EBUSY:0;locked=!e;}
        struct reset_image s={0};if(!e)e=image(&s);if(!e)e=ownership(&s);
        if(!e&&((s.dout|s.din)&12))e=EBUSY; /* inherited high is not fresh reset */
        if(!e) {
            pa.captured=true;pa.prior=pa.expected=s.dout;
            e=dwell(0);if(!e)e=online();
            if(!e)e=set_reset(12);if(!e)e=dwell(12);
        }
        if(locked)X9PMIFRelease();
        if(!e)e=power();if(!e)e=online();
        if(!e){pa.configured=true;*epoch=pa.epoch=++pa.serial;}
        if(e)e=fault(e); /* no automatic reset replay/unwind */
    }
    lck_mtx_unlock(&pa_reset_lock);return e;
}
static int validate_locked(uint64_t ticket,uint64_t epoch)
{
    if(!pa.active||!ticket||ticket!=pa.policy.ticket||!epoch||epoch!=pa.epoch)return EPERM;
    int e=!pa.configured||pa.uncertain||pa.pending?ENXIO:power();
    bool locked=false;if(!e){e=X9PMIFAcquire()?EBUSY:0;locked=!e;}
    struct reset_image s={0};if(!e)e=verify(false,&s);
    if(!e&&((s.dout&12)!=12||(s.din&12)!=12))e=EIO;
    if(locked)X9PMIFRelease();if(!e)e=power();
    /* Historical stream/transport fault alone does not revoke physical PA
     * accessibility for mute; Online is required again by new-stream caller. */
    if(e&&pa.active&&ticket==pa.policy.ticket)e=fault(e);
    return e;
}
int x9_pa_reset_validate(uint64_t ticket,uint64_t epoch)
{
    lck_mtx_lock(&pa_reset_lock);int e=validate_locked(ticket,epoch);
    lck_mtx_unlock(&pa_reset_lock);return e;
}
int x9_pa_reset_bus_hold(uint64_t ticket,uint64_t epoch,uint64_t *lease)
{
    if(!lease)return EINVAL;*lease=0;
    lck_mtx_lock(&pa_reset_lock);
    int e=pa.bus_child?EBUSY:validate_locked(ticket,epoch);
    if(!e)e=online();if(!e&&pa.error)e=EIO;
    if(!e&&pa.serial==UINT64_MAX)e=EOVERFLOW;
    if(!e)*lease=pa.bus_child=++pa.serial;
    lck_mtx_unlock(&pa_reset_lock);return e;
}
int x9_pa_reset_bus_validate(uint64_t ticket,uint64_t epoch,uint64_t lease)
{
    lck_mtx_lock(&pa_reset_lock);
    int e=!lease||lease!=pa.bus_child?EPERM:validate_locked(ticket,epoch);
    lck_mtx_unlock(&pa_reset_lock);return e;
}
static int bus_quiesced_locked(uint64_t ticket,uint64_t epoch,uint64_t lease)
{
    int e=!lease||lease!=pa.bus_child?EPERM:validate_locked(ticket,epoch);
    if(!e)e=x9_adsp_mt6993_speaker_halted_validate(ticket);
    if(!e)e=pa.policy.validate_drained(pa.policy.context,ticket,pa.policy.generation,0x1a110000,0x10000);
    return e;
}
int x9_pa_reset_bus_quiesced_validate(uint64_t ticket,uint64_t epoch,uint64_t lease)
{
    lck_mtx_lock(&pa_reset_lock);int e=bus_quiesced_locked(ticket,epoch,lease);
    lck_mtx_unlock(&pa_reset_lock);return e;
}
int x9_pa_reset_bus_drop(uint64_t ticket,uint64_t epoch,uint64_t lease)
{
    lck_mtx_lock(&pa_reset_lock);int e=bus_quiesced_locked(ticket,epoch,lease);
    if(!e)pa.bus_child=0;
    lck_mtx_unlock(&pa_reset_lock);return e;
}
int x9_pa_reset_resolve_pmif(uint64_t ticket)
{
    lck_mtx_lock(&pa_reset_lock);
    int e=!pa.active||!ticket||ticket!=pa.policy.ticket?EPERM:0;
    if(!e)e=x9_adsp_mt6993_speaker_halted_validate(ticket);
    if(!e)e=pa.policy.validate_drained(pa.policy.context,ticket,pa.policy.generation,0x1a110000,0x10000);
    if(!e)e=power();bool locked=false;
    if(!e&&pa.pending){e=X9PMIFAcquire()?EBUSY:0;locked=!e;}
    if(!e)e=resolve_pending();if(locked)X9PMIFRelease();
    if(!e)e=power();if(e&&pa.active&&ticket==pa.policy.ticket)e=fault(e);
    lck_mtx_unlock(&pa_reset_lock);return e;
}
int x9_pa_reset_held(uint64_t ticket)
{
    lck_mtx_lock(&pa_reset_lock);int held=pa.active&&ticket&&ticket==pa.policy.ticket;
    lck_mtx_unlock(&pa_reset_lock);return held;
}
int x9_pa_reset_release(uint64_t ticket)
{
    lck_mtx_lock(&pa_reset_lock);
    int e=!pa.active||!ticket||ticket!=pa.policy.ticket?EPERM:0;
    if(!e&&pa.bus_child){lck_mtx_unlock(&pa_reset_lock);return EBUSY;}
    if(!e)e=x9_adsp_mt6993_speaker_halted_validate(ticket);
    if(!e)e=pa.policy.validate_drained(pa.policy.context,ticket,pa.policy.generation,0x1a110000,0x10000);
    if(!e)pa.configured=false;
    if(!e&&(pa.captured||pa.pending)) {
        e=power();bool locked=false;if(!e){e=X9PMIFAcquire()?EBUSY:0;locked=!e;}
        if(!e)e=resolve_pending();
        if(!e&&pa.captured) {
            if(!pa.asserted){e=set_reset(0);if(!e)e=dwell(0);if(!e)pa.asserted=true;}
            struct reset_image s={0};if(!e)e=verify(false,&s);
            if(!e&&((s.dout|s.din)&12))e=EBUSY;
        }
        if(locked)X9PMIFRelease();if(!e)e=power();
    }
    if(!e)e=x9_speaker_mt6993_clock_pa_drop(ticket,pa.child);
    if(!e){pa.child=0;pa.epoch=0;pa.active=false;pa.dirty=false;}
    else if(pa.active&&ticket==pa.policy.ticket)e=fault(e);
    lck_mtx_unlock(&pa_reset_lock);return e;
}
