/* Native PLJ110 I2S4 pads. OPPO pinctrl-mt6993.c FIELD_BASE 108..111,
 * pinctrl-paris.c Schmitt semantics and race-free SET/CLR/MODE MWR.
 * No cold diagnostic, inherited Linux state or user verified flag grants
 * access. Parent is the real native clock sublease, not probe-status bits. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/locks.h>
#include <vm/pmap.h>
#include "x9_audio_resources.h"
#include "x9_adsp_mt6993.h"
#include "x9_adsp_transport.h"
#include "x9_speaker_mt6993.h"
#include "x9_speaker_pads.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t,addr64_t);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *,int);
LCK_GRP_DECLARE(pads_group,"x9-speaker-pads");
LCK_MTX_DECLARE(pads_lock,&pads_group);
static struct {
    struct x9_speaker_pads_policy policy;
    vm_offset_t gpio,ioc;
    uint64_t child;
    uint32_t last_generation,error,dirty,uncertain;
    bool active,captured,configured,restored;
} pads;
struct pad_field { unsigned bank,off;uint32_t mask,target,saved,expected,prior; };
static struct pad_field fields[]={
    {0,0x4b0,0x0f0f0f0f,0x05050505,0,0,0}, /* MODE: bit4 is MWR enable */
    {0,0x030,0x0000f000,0,0,0,0},          /* DIR input */
    {1,0x0b0,0x170,0,0,0,0},              /* PU off */
    {1,0x0a0,0x170,0,0,0,0},              /* PD off */
    {1,0x0e0,0x170,0x170,0,0,0},          /* Schmitt on */
};
static struct pad_field observed[]={
    {0,0x130,0xf000,0,0,0,0},             /* DO never written */
    {1,0x070,0x170,0,0,0,0},              /* IES NOT changed by Schmitt */
    {1,0x000,0x07fc0000,0,0,0,0},         /* drive GPIO108/109/110 */
    {1,0x010,7,0,0,0,0},                  /* drive GPIO111 */
};
static uint32_t rd(unsigned bank,unsigned off)
{
    uint32_t v=*(volatile uint32_t *)((bank?pads.ioc:pads.gpio)+off);
    __asm__ volatile("dmb oshld":::"memory");return v;
}
static void wr(unsigned bank,unsigned off,uint32_t v)
{
    __asm__ volatile("dmb oshst":::"memory");
    *(volatile uint32_t *)((bank?pads.ioc:pads.gpio)+off)=v;
    __asm__ volatile("dsb sy":::"memory");
}
static int fault(int e)
{
    if(e==EAGAIN)e=EIO;
    if(!pads.error)pads.error=(uint32_t)e;
    x9_audio_resources_fault(X9AudioResourceSpeaker,pads.policy.ticket,e);return e;
}
static int mappings(void)
{
    if(PAGE_SIZE!=0x4000||!pads.gpio||!pads.ioc)return ENXIO;
    const vm_offset_t va[]={pads.gpio,pads.ioc};
    const uint64_t pa[]={0x1002d000ULL,0x12860000ULL};
    for(unsigned i=0;i<2;i++)for(unsigned n=0;n<2;n++) {
        addr64_t a=(addr64_t)va[i]+(n?0xfff:0);
        ppnum_t p=pmap_find_phys(kernel_pmap,a);
        /* GPIO is NOT 16K aligned. Compare the byte PA, not just ppnum. */
        if(!p||ptoa_64(p)+(a&PAGE_MASK)!=pa[i]+(n?0xfff:0))return EFAULT;
    }
    return 0;
}
static int access(void)
{
    int e=x9_speaker_mt6993_clock_pad_validate(pads.policy.ticket,pads.child);
    return e?e:mappings();
}
static int verify(bool saved,bool recovering)
{
    int e=access();if(e)return e;
    if(!pads.captured)return ENXIO;
    /* The 5th MODE bit is a write-modify strobe, not restorable data.
     * OPPO's stable readback compares full 5-bit field with mode<=15. */
    if(rd(0,0x4b0)&0x10101010)return EBUSY;
    for(unsigned i=0;i<5;i++) {
        struct pad_field *f=&fields[i];uint32_t v=rd(f->bank,f->off)&f->mask;
        if(v!=((saved?f->saved:f->expected)&f->mask)&&
           !(recovering&&!saved&&(pads.uncertain&(1u<<i))&&v==(f->prior&f->mask)))return EBUSY;
    }
    for(unsigned i=0;i<4;i++)if((rd(observed[i].bank,observed[i].off)&observed[i].mask)!=
       (observed[i].saved&observed[i].mask))return EBUSY;
    return 0;
}
static int change(unsigned i,uint32_t value)
{
    struct pad_field *f=&fields[i];uint32_t bit=1u<<i;
    /* Every single alias write has its own LAST observed prior; a failed
     * SET followed by CLR cannot fall back to the generation's old saved. */
    for(unsigned step=0;step<2;step++) {
        int e=verify(false,true);if(e)return e;
        uint32_t old=rd(f->bank,f->off),want=value&f->mask;
        if((old&f->mask)!=(f->expected&f->mask)&&
           !((pads.uncertain&bit)&&(old&f->mask)==(f->prior&f->mask)))return EBUSY;
        if((old&f->mask)==want){f->expected=old;pads.uncertain&=~bit;return 0;}
        uint32_t next=(old&~f->mask)|want,off=f->off,v;
        if(!i) {off+=12;v=want|0x10101010;} /* owned four-pin MODE MWR */
        else {
            uint32_t clear=(old&f->mask)&~want,set=want&~old;
            if(clear){off+=8;v=clear;next=old&~clear;}
            else {off+=4;v=set;next=old|set;}
        }
        f->prior=old;f->expected=next;pads.dirty|=bit;pads.uncertain|=bit;
        wr(f->bank,off,v);
        e=access();if(e)return e;
        if((rd(f->bank,f->off)&f->mask)!=(next&f->mask))return EIO;
        pads.uncertain&=~bit;
        if((next&f->mask)==want)return verify(false,false);
    }
    return EIO;
}
static int online(uint64_t ticket,uint32_t generation)
{
    X9AudioDSPStatus ds={0};int e=x9_adsp_mt6993_speaker_validate(ticket);
    if(!e)e=x9_adsp_status(0,&ds);
    if(!e&&(ds.state!=X9DSPOnline||ds.error||ds.generation!=generation))e=ESTALE;
    return e;
}
int x9_speaker_pads_acquire(const struct x9_speaker_pads_policy *p)
{
    if(!p||p->version!=X9_SPEAKER_PADS_POLICY_VERSION||!p->ticket||!p->generation||
       !p->mute||!p->validate_drained)return EINVAL;
    if(!sweet_early_is_x9()||sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    lck_mtx_lock(&pads_lock);
    int e=pads.active||p->generation<=pads.last_generation?EBUSY:online(p->ticket,p->generation);
    struct x9_adsp_handoff h={0};if(!e)e=x9_adsp_mt6993_copy_handoff(&h);
    if(!e&&(!x9_adsp_handoff_valid(&h)||!(h.flags&X9_ADSP_I2S4_VERIFIED)))e=ENXIO;
    uint64_t child=0;if(!e)e=x9_speaker_mt6993_clock_pad_hold(p->ticket,&child);
    if(!e) {
        pads.policy=*p;pads.child=child;pads.active=true;pads.last_generation=p->generation;
        pads.error=pads.dirty=pads.uncertain=0;pads.captured=pads.configured=pads.restored=false;
        /* No pin access/mutation before parent hold and real dual-PA mute. */
        e=online(p->ticket,p->generation);if(!e)e=p->mute(p->context,p->ticket);
        if(!e)e=online(p->ticket,p->generation);
        if(!e) {
            if(!pads.gpio)pads.gpio=ml_io_map(0x1002d000,0x1000);
            if(!pads.ioc)pads.ioc=ml_io_map(0x12860000,0x1000);
            e=access();
        }
        if(!e&&(rd(0,0x4b0)&0x1f1f1f1f))e=EBUSY; /* inherited alternate owner */
        if(!e&&(rd(0,0x30)&0xf000))e=EBUSY;        /* inherited output */
        if(!e) {
            for(unsigned i=0;i<5;i++)fields[i].prior=fields[i].saved=fields[i].expected=rd(fields[i].bank,fields[i].off);
            for(unsigned i=0;i<4;i++)observed[i].saved=rd(observed[i].bank,observed[i].off);
            pads.captured=true;
            /* The saved image itself must STILL be the safe baseline;
             * an alternate owner appearing between refusal and capture
             * cannot be silently converted into our restorable baseline. */
            if((fields[0].saved&0x1f1f1f1f)||(fields[1].saved&0xf000))e=EBUSY;
            if(!e)e=verify(false,false);
            /* Electrical state while in safe mode0; mux mode5 LAST. */
            for(unsigned i=1;!e&&i<5;i++)e=change(i,fields[i].target);
            if(!e)e=change(0,fields[0].target);
            if(!e)e=online(p->ticket,p->generation);
            if(!e)e=verify(false,false);
            if(!e)pads.configured=true;
        }
        if(e)e=fault(e); /* explicit cleanup only, never automatic unwind */
    }
    lck_mtx_unlock(&pads_lock);return e;
}
int x9_speaker_pads_validate(uint64_t ticket)
{
    lck_mtx_lock(&pads_lock);
    int e=!pads.active||!ticket||ticket!=pads.policy.ticket?EPERM:
        !pads.configured||pads.restored?ENXIO:verify(false,false);
    /* Remembered stream faults do not block safe physical teardown. */
    if(e&&pads.active&&ticket==pads.policy.ticket)e=fault(e);
    lck_mtx_unlock(&pads_lock);return e;
}
int x9_speaker_pads_held(uint64_t ticket)
{
    lck_mtx_lock(&pads_lock);int held=pads.active&&ticket&&ticket==pads.policy.ticket;
    lck_mtx_unlock(&pads_lock);return held;
}
int x9_speaker_pads_release(uint64_t ticket)
{
    lck_mtx_lock(&pads_lock);
    int e=!pads.active||!ticket||ticket!=pads.policy.ticket?EPERM:0;
    if(!e)e=pads.policy.mute(pads.policy.context,ticket);
    if(!e)e=x9_adsp_mt6993_speaker_halted_validate(ticket);
    if(!e)e=pads.policy.validate_drained(pads.policy.context,ticket,0x1a110000,0x10000);
    if(!e)pads.configured=false; /* partial teardown never validates as mode5 */
    if(!e&&pads.captured&&!pads.restored) {
        e=verify(false,true);
        /* Exit mode5 BEFORE electrical restoration. Retire only touched
         * fields; third values and uncertain failures retain the parent. */
        if(!e&&(pads.dirty&1)){e=change(0,fields[0].saved);if(!e)pads.dirty&=~1u;}
        for(unsigned i=5;!e&&i>1;i--)if(pads.dirty&(1u<<(i-1))) {
            e=change(i-1,fields[i-1].saved);if(!e)pads.dirty&=~(1u<<(i-1));
        }
        if(!e)e=verify(true,false);if(!e)pads.restored=true;
    }
    if(!e&&pads.captured)e=verify(true,false); /* fresh proof on drop retry */
    if(!e)e=x9_speaker_mt6993_clock_pad_drop(ticket,pads.child);
    if(!e){pads.child=0;pads.active=pads.configured=false;}
    else if(pads.active&&ticket==pads.policy.ticket)e=fault(e);
    lck_mtx_unlock(&pads_lock);return e;
}
