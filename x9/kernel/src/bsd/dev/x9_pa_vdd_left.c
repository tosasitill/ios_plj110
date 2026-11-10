/* OPPO tfa98xx.c: tfa98xx_vdd set_voltage(1800000), set_load(200000),
 * enable(). mt6661-regulator.{c,h}: SID5 LNC0_8 selector0x24b encoded
 * (vosel<<4)|vocal, EN0/LP1 at0x2087, DA EN0/LP2 at0x20a2,
 * HW_NORMAL_OP_EN=2 at0x20af; SET+1/CLR+2. LEFT only, no right inference. */
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
#include "x9_pa_vdd_left.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t,addr64_t);
extern int X9PMIFAcquire(void);
extern void X9PMIFRelease(void);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *,int);
LCK_GRP_DECLARE(left_vdd_group,"x9-pa-left-vdd");
LCK_MTX_DECLARE(left_vdd_lock,&left_vdd_group);
enum { PendingNone,PendingRead,PendingWrite,PendingClear };
enum { Selector,Control,Hardware,Fields };
static const unsigned registers[Fields]={X9_PA_LEFT_VDD_VSEL_REG,
    X9_PA_LEFT_VDD_SW_REG,X9_PA_LEFT_VDD_HW_REG};
static int online(void);
static struct {
    struct x9_pa_left_vdd_policy policy;
    vm_offset_t pmif;
    uint64_t child,lease,serial;
    uint32_t last_generation;
    uint8_t saved[Fields],expected[Fields],prior[Fields];
    X9AudioLeftVDDStatus status;
    bool active,captured,configured,teardown;
} vdd;
_Static_assert(sizeof(X9AudioLeftVDDStatus)==64,"LEFT VDD cached ABI");
static uint32_t rr(unsigned off)
{
    uint32_t value=*(volatile uint32_t *)(vdd.pmif+off);
    __asm__ volatile("dmb oshld":::"memory");return value;
}
static void wr(unsigned off,uint32_t value)
{
    __asm__ volatile("dmb oshst":::"memory");
    *(volatile uint32_t *)(vdd.pmif+off)=value;
    __asm__ volatile("dsb sy":::"memory");
}
static int fault(int e)
{
    if(e==EAGAIN)e=EIO;
    if(!vdd.status.error)vdd.status.error=(uint32_t)e;
    x9_audio_resources_fault(X9AudioResourceSpeaker,vdd.policy.ticket,e);return e;
}
static int parent(void)
{return x9_speaker_mt6993_clock_rail_validate(vdd.policy.ticket,vdd.child);}
static int exclusive(void)
{
    int e=parent();if(!e)e=vdd.policy.validate_exclusive(vdd.policy.context,
        vdd.policy.ticket,vdd.policy.generation);
    if(!e)e=parent();return e;
}
static int mapping(void)
{
    if(PAGE_SIZE!=0x4000||!vdd.pmif)return ENXIO;
    for(unsigned i=0;i<2;i++) {
        addr64_t a=(addr64_t)vdd.pmif+(i?0x1fff:0);ppnum_t p=pmap_find_phys(kernel_pmap,a);
        if(!p||ptoa_64(p)+(a&PAGE_MASK)!=0x1c018000ULL+(i?0x1fff:0))return EFAULT;
    }
    return 0;
}
static int access(void)
{
    int e=parent();if(!e)e=mapping();if(!e&&!(rr(0)&1))e=ENXIO;return e;
}
/* PMIF SWINF2, common PMIF mutex required. No generic clear/retry. */
static int wait_state(unsigned wanted)
{
    uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);
    for(unsigned i=0;i<1000;i++) {
        int e=access();if(e)return e;uint32_t state=rr(0x8a8);
        if(state&(1u<<18))return EIO;
        if(((state>>1)&7)==wanted)return 0;
        if(mach_absolute_time()>=deadline)break;delay(1);
    }
    return ETIMEDOUT;
}
static int read_byte(unsigned sid,unsigned reg,uint8_t *out)
{
    if(vdd.status.pending)return EBUSY;
    int e=wait_state(0);if(e)return e;
    vdd.status.pending=PendingRead;wr(0x880,(3u<<30)|(sid<<24)|reg);
    e=wait_state(6);if(e)return e;uint8_t value=(uint8_t)rr(0x894);
    vdd.status.pending=PendingClear;wr(0x8a4,1);e=wait_state(0);
    if(!e){vdd.status.pending=PendingNone;*out=value;}return e;
}
static int write_byte(unsigned reg,uint8_t value)
{
    if(vdd.status.pending)return EBUSY;
    int e=wait_state(0);if(e)return e;
    vdd.status.pending=PendingWrite;wr(0x884,value);
    wr(0x880,(3u<<30)|(1u<<29)|(5u<<24)|reg);
    e=wait_state(0);if(!e)vdd.status.pending=PendingNone;return e;
}
static int resolve_pending(void)
{
    if(!vdd.status.pending)return 0;
    int e=access();if(e)return e;uint32_t state=rr(0x8a8);
    if(state&(1u<<18))return EIO;unsigned fsm=(state>>1)&7;
    if(vdd.status.pending==PendingRead) {
        /* Idle cannot retire a read that might still produce late VALID.
         * Attribution is independently pinned, not derived from FSM alone. */
        if(fsm!=6)return EBUSY;
        (void)rr(0x894);vdd.status.pending=PendingClear;wr(0x8a4,1);
        e=wait_state(0);if(e)return e;
    } else if(fsm!=0)return EBUSY;
    vdd.status.pending=PendingNone;return 0;
}
static int reset_low(void)
{
    uint8_t dir=0,dout=0,din=0,mode=0,sw=0,digital=0;
    int e=read_byte(3,0x8b,&dir);if(!e)e=read_byte(3,0xa3,&dout);
    if(!e)e=read_byte(3,0xa7,&din);if(!e)e=read_byte(3,0xb9,&mode);
    if(!e)e=read_byte(3,0xd5,&sw);if(!e)e=read_byte(3,0xd6,&digital);
    if(!e&&((dir&12)!=12||((dout|din)&12)||(mode&63)||(sw&12)!=12||
       (digital&12)!=12))e=EBUSY;
    return e; /* Physical DI LOW as well as DO, no GPIO write. */
}
static int image(uint8_t values[Fields],uint8_t *da)
{
    int e=0;for(unsigned i=0;!e&&i<Fields;i++)e=read_byte(5,registers[i],&values[i]);
    if(!e)e=read_byte(5,X9_PA_LEFT_VDD_DA_REG,da);
    if(!e) {
        vdd.status.selector=values[Selector];vdd.status.control=values[Control];
        vdd.status.hw_op=values[Hardware];vdd.status.da=*da;
    }
    return e;
}
static int verify(bool recovering,uint8_t values[Fields],uint8_t *da)
{
    int e=image(values,da);if(e)return e;
    for(unsigned i=0;i<Fields;i++)if(values[i]!=vdd.expected[i]&&
        !(recovering&&(vdd.status.uncertain&(1u<<i))&&values[i]==vdd.prior[i]))return EBUSY;
    return 0; /* DA is not interpreted with regulator.c's ambiguous masks. */
}
static int change(unsigned field,uint8_t wanted)
{
    /* New programming must not continue through a same-generation transport
     * quarantine. Physical cleanup, in contrast, does not require Online. */
    int e=vdd.teardown?0:online();
    uint8_t values[Fields]={0},da=0;if(!e)e=reset_low();if(!e)e=verify(true,values,&da);
    if(e)return e;
    /* Recheck the actual OFF state at each VSEL write, not just at the
     * earlier cold image or settle. Never retune an enabled rail. */
    if(field==Selector&&((values[Control]&1)||values[Hardware]||(da&X9_PA_LEFT_VDD_DA_EN)))return EBUSY;
    /* A fresh full control image protects unmodified bits too. For an
     * uncertain action only its last observed prior or expected is accepted. */
    uint8_t old=values[field];if(old==wanted) {
        vdd.expected[field]=old;vdd.status.uncertain&=~(1u<<field);return 0;
    }
    unsigned reg=registers[field];uint8_t payload=wanted;
    if(field!=Selector) {
        uint8_t changed=old^wanted;
        if((field==Control&&(changed&~3u))||(field==Hardware&&(changed&~2u)))return EPERM;
        /* Only one SET or CLR transaction; no full-byte RMW over neighbors. */
        if((wanted&changed)&&((uint8_t)~wanted&changed))return EINVAL;
        reg+=(wanted&changed)?1:2;payload=changed;
    }
    vdd.prior[field]=old;vdd.expected[field]=wanted;
    vdd.status.dirty|=1u<<field;vdd.status.uncertain|=1u<<field;
    e=write_byte(reg,payload);if(!e)e=verify(false,values,&da);
    if(!e)vdd.status.uncertain&=~(1u<<field);return e;
}
static int settle(unsigned wanted)
{
    /* FDT ramp200us is a minimum delay, not an analogue measurement. Then
     * wait boundedly for actual DA EN(bit0)/LP(bit2) after control readback. */
    uint64_t minimum,deadline;clock_interval_to_deadline(200,NSEC_PER_USEC,&minimum);
    clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);
    for(unsigned i=0;i<1000;i++) {
        int e=access();if(e)return e;
        if(mach_absolute_time()>=minimum) {
            uint8_t values[Fields]={0},da=0;e=verify(false,values,&da);if(e)return e;
            if((da&5)==wanted)return 0;
        }
        if(mach_absolute_time()>=deadline)break;delay(5);
    }
    return ETIMEDOUT;
}
static int online(void)
{
    X9AudioDSPStatus s={0};int e=x9_adsp_status(0,&s);
    return e?e:s.state!=X9DSPOnline||s.error||s.generation!=vdd.policy.generation?ESTALE:0;
}
int x9_pa_vdd_left_acquire(const struct x9_pa_left_vdd_policy *p,uint64_t *lease)
{
    if(!lease)return EINVAL;*lease=0;
    if(!p||p->version!=X9_PA_LEFT_VDD_POLICY_VERSION||!p->ticket||!p->generation||
       !p->validate_exclusive||!p->validate_drained)return EINVAL;
    if(!sweet_early_is_x9()||sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    lck_mtx_lock(&left_vdd_lock);
    int e=vdd.active||p->generation<=vdd.last_generation?EBUSY:0;
    struct x9_adsp_handoff h={0};if(!e)e=x9_adsp_mt6993_copy_handoff(&h);
    if(!e&&!(h.flags&X9_ADSP_PA_LEFT_VDD_VERIFIED))e=ENXIO;
    if(!e&&vdd.serial==UINT64_MAX)e=EOVERFLOW;
    uint64_t child=0;if(!e)e=x9_speaker_mt6993_clock_rail_hold(p->ticket,&child);
    if(!e) {
        vdd.policy=*p;vdd.child=child;vdd.active=true;vdd.last_generation=p->generation;
        vdd.captured=vdd.configured=vdd.teardown=false;
        *lease=vdd.lease=++vdd.serial;
        vdd.status=(X9AudioLeftVDDStatus){.version=X9_PA_LEFT_VDD_STATUS_VERSION,
            .flags=X9LeftVDDHeld,.generation=p->generation,.ticket=p->ticket,.lease=*lease};
        e=online();if(!e)e=exclusive();if(!e)e=online();
        if(!e&&!vdd.pmif)vdd.pmif=ml_io_map(0x1c018000,0x2000);
        if(!e)e=mapping();bool locked=false;if(!e){e=X9PMIFAcquire()?EBUSY:0;locked=!e;}
        uint8_t values[Fields]={0},da=0;
        if(!e)e=reset_low();if(!e)e=image(values,&da);
        /* No takeover of a live rail or hardware operation. Off-state LP
         * may be inherited; only that owned bit is normalized/restored. */
        if(!e&&((values[Control]&1)||(da&5)||values[Hardware]))e=EBUSY;
        if(!e) {
            memcpy(vdd.saved,values,Fields);memcpy(vdd.expected,values,Fields);
            vdd.captured=true;vdd.status.flags|=X9LeftVDDCaptured;
            e=change(Selector,X9_PA_LEFT_VDD_VSEL);
            if(!e)e=change(Control,vdd.expected[Control]&~2u);
            if(!e)e=change(Hardware,X9_PA_LEFT_VDD_HW_NORMAL);
            if(!e)e=change(Control,vdd.expected[Control]|1u);
            if(!e)e=settle(1);if(!e)e=reset_low();
        }
        if(locked)X9PMIFRelease();if(!e)e=exclusive();if(!e)e=online();
        if(!e){vdd.configured=true;vdd.status.flags|=X9LeftVDDConfigured;}
        if(e)e=fault(e); /* Retain child/journal/token; no automatic unwind. */
    }
    lck_mtx_unlock(&left_vdd_lock);return e;
}
int x9_pa_vdd_left_validate(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&left_vdd_lock);
    int e=!vdd.active||!ticket||ticket!=vdd.policy.ticket||!lease||lease!=vdd.lease?EPERM:0;
    if(!e&&(!vdd.configured||vdd.teardown||vdd.status.uncertain||vdd.status.pending))e=ENXIO;
    if(!e)e=exclusive();bool locked=false;if(!e){e=X9PMIFAcquire()?EBUSY:0;locked=!e;}
    uint8_t values[Fields]={0},da=0;if(!e)e=verify(false,values,&da);
    if(!e&&(values[Selector]!=X9_PA_LEFT_VDD_VSEL||(values[Control]&3)!=1||
       values[Hardware]!=X9_PA_LEFT_VDD_HW_NORMAL||(da&5)!=1))e=EIO;
    if(locked)X9PMIFRelease();if(!e)e=exclusive();
    /* No Online requirement here: same-generation physical fault cleanup
     * must still be able to mute PAs before halting producer. */
    if(e&&vdd.active&&ticket==vdd.policy.ticket&&lease==vdd.lease)e=fault(e);
    lck_mtx_unlock(&left_vdd_lock);return e;
}
int x9_pa_vdd_left_release(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&left_vdd_lock);
    int e=!vdd.active||!ticket||ticket!=vdd.policy.ticket||!lease||lease!=vdd.lease?EPERM:0;
    if(!e)e=x9_speaker_mt6993_clock_rail_quiesced_validate(ticket,vdd.child);
    if(e==EBUSY){lck_mtx_unlock(&left_vdd_lock);return e;} /* PA still pins power. */
    if(!e)e=vdd.policy.validate_drained(vdd.policy.context,ticket,vdd.policy.generation,0x1a110000,0x10000);
    if(!e)e=exclusive();
    if(!e) {
        vdd.teardown=true;vdd.configured=false;vdd.status.flags|=X9LeftVDDTeardown;
        vdd.status.flags&=~X9LeftVDDConfigured;
        /* A pre-map failure has issued no rail operation. No off-domain or
         * missing-map read is needed to drop its untouched clock child. */
        bool locked=false;
        if(vdd.captured||vdd.status.pending){e=X9PMIFAcquire()?EBUSY:0;locked=!e;}
        if(!e)e=resolve_pending();
        if(!e&&vdd.captured) {
            uint8_t values[Fields]={0},da=0;e=reset_low();if(!e)e=verify(true,values,&da);
            /* Resolve only observed expected/prior. Then restore according
             * to the actual control image; never replay a timed-out action. */
            if(!e)for(unsigned i=0;i<Fields;i++)vdd.expected[i]=values[i];
            if(!e)vdd.status.uncertain=0;
            if(!e&&(vdd.status.dirty&(1u<<Control)))e=change(Control,vdd.expected[Control]&~1u);
            if(!e&&(vdd.status.dirty&(1u<<Hardware)))e=change(Hardware,vdd.saved[Hardware]);
            /* HW1 normal operation can still influence DA; prove OFF only
             * AFTER SW_EN and owned HW1 are retired, BEFORE lowering VSEL. */
            if(!e)e=settle(0);
            if(!e&&(vdd.status.dirty&(1u<<Control)))e=change(Control,vdd.saved[Control]);
            if(!e&&(vdd.status.dirty&(1u<<Selector)))e=change(Selector,vdd.saved[Selector]);
            if(!e)e=verify(false,values,&da);
            if(!e&&(memcmp(values,vdd.saved,Fields)||(da&5)))e=EBUSY;
            if(!e)e=reset_low();
        }
        if(locked)X9PMIFRelease();if(!e)e=exclusive();
    }
    if(!e)e=x9_speaker_mt6993_clock_rail_drop(ticket,vdd.child);
    vdd.status.restore_error=(uint32_t)e;
    if(!e) {
        vdd.active=false;vdd.child=vdd.lease=0;vdd.status.dirty=vdd.status.uncertain=0;
        vdd.status.flags&=~X9LeftVDDHeld;vdd.status.flags|=X9LeftVDDRestored;
        /* Historical error and old cached lease retained for attribution. */
    } else if(vdd.active&&ticket==vdd.policy.ticket&&lease==vdd.lease)e=fault(e);
    lck_mtx_unlock(&left_vdd_lock);return e;
}
int x9_pa_vdd_left_status(X9AudioLeftVDDStatus *out)
{
    if(!out)return EINVAL;lck_mtx_lock(&left_vdd_lock);*out=vdd.status;
    out->version=X9_PA_LEFT_VDD_STATUS_VERSION;lck_mtx_unlock(&left_vdd_lock);return 0;
}
