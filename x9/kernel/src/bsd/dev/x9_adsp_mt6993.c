/* Independent implementation of OPPO clk-mt6993-pd/hwccf_api/adsp_core v3.
 * Only core A is owned. Cold takeover requires TOP+INFRA globally OFF;
 * inherited AO votes are preserved. Never reset a running foreign system.
 * Secure RELOAD is the firmware source, NOT a made-up request_firmware/ELF
 * loader. Its return is not firmware-ready: transport still requires IPI13.
 * No runtime suspend IPI is issued; domains remain pinned until explicit stop.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <mach/vm_types.h>
#include <pexpert/device_tree.h>
#include "x9_adsp_mt6993.h"
#include "x9_adsp_transport.h"
#include "x9_audio_resources.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *,int);
LCK_GRP_DECLARE(board_group,"x9-adsp-board");
LCK_MTX_DECLARE(board_control_lock,&board_group);
LCK_MTX_DECLARE(board_lock,&board_group);
static volatile uint32_t *hwv,*cfg,*cfg2;
static bool registered,exclusive,reset_done,started,wake;
static uint64_t resource_ticket;
static uint64_t borrowed_ticket;
static uint32_t resource_owner=X9AudioResourceADSP;
static int access_error; /* reset only by a new explicit control operation */
static bool new_control;
static X9AudioBoardStatus status;
static struct x9_adsp_handoff layout;
enum { AudioSemIdle, AudioSemAcquireUncertain, AudioSemHeld,
       AudioSemReleaseUncertain, AudioSemLost };
static uint32_t audio_sem_state;
static uint64_t audio_sem_lease,audio_sem_serial;
static uint64_t shared_child_lease,shared_child_serial;
static uint64_t clock_child_lease,clock_child_serial;
static uint64_t irq_child_lease,irq_child_serial;
static bool irq_child_quiesced;
enum { AUDIO_SEM_REG=0x64,AUDIO_SEM_BIT=1u<<(4*3+2),AUDIO_SEM_RETRIES=5000 };
_Static_assert(sizeof(X9AudioBoardStatus)==160,"ADSP board status v3 size");
enum { PD_SET=0x20700,PD_CLR=0x20704,PD_EN=0x20708,PD_DONE=0x1132c,
       PD_ALL=0x13700,PD_BUSY=0x13704,TOP=2,INFRA=4,AO=8,CHAIN=14,
       SMC_CLEAR=3,SMC_START=4,SMC_STOP=5,SMC_RELOAD=7 };
_Static_assert(sizeof(struct x9_adsp_handoff)==X9_ADSP_HANDOFF_BYTES,"ADSP layout size");
static uint32_t get32(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t get64(const uint8_t *p)
{return (uint64_t)get32(p)|(uint64_t)get32(p+4)<<32;}
static void copy_layout(void)
{
    DTEntry chosen;const void *p=NULL;unsigned int bytes=0;
    struct x9_adsp_handoff h;
    if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess ||
       SecureDTGetProperty(chosen,"x9-adsp-handoff",&p,&bytes)!=kSuccess ||
       !p || bytes!=sizeof(h))return;
    memcpy(&h,p,sizeof(h));if(!x9_adsp_handoff_valid(&h))return;
    if(SecureDTGetProperty(chosen,"x9-ram-map",&p,&bytes)!=kSuccess ||
       !p || bytes!=2064)return;
    const uint8_t *map=p;uint32_t count=get32(map+4);
    if(get32(map)!=1 || !count || count>128)return;
    uint64_t ram_total=0,last=0;
    for(unsigned i=0;i<count;i++) {
        uint64_t lo=get64(map+16+i*16),hi=get64(map+24+i*16);
        if(lo<0x100000000ULL || hi>0x37fa00000ULL || lo>=hi || lo<last ||
           ((lo|hi)&0x3fff) || ram_total>UINT64_MAX-(hi-lo) ||
           (h.reserve_base<hi && lo<h.reserve_base+h.reserve_bytes))return;
        last=hi;ram_total+=hi-lo;
    }
    if(ram_total<(8ULL<<30) || ram_total!=get64(map+8))return;
    /* The fixed-size ADT tail must be zero, not a hidden second range list. */
    for(unsigned i=16+count*16;i<bytes;i++)if(map[i])return;
    lck_mtx_lock(&board_lock);layout=h;lck_mtx_unlock(&board_lock);
}
static uint32_t rd(volatile uint32_t *base,uint32_t off)
{ uint32_t v=base[off/4];__asm__ volatile("dmb oshld":::"memory");return v; }
static void wr(volatile uint32_t *base,uint32_t off,uint32_t v)
{ base[off/4]=v;__asm__ volatile("dsb sy":::"memory"); }
static int wait_bits(volatile uint32_t *p,uint32_t off,uint32_t mask,uint32_t val,uint32_t us)
{
    uint64_t ticks;nanoseconds_to_absolutetime((uint64_t)us*1000,&ticks);
    uint64_t end=mach_absolute_time()+ticks;
    for(uint32_t n=0;n<us/2+1;n++) {
        if((rd(p,off)&mask)==val)return 0;
        if(mach_absolute_time()>=end)break;delay(2);
    }
    status.timeouts++;return ETIMEDOUT;
}
static void snapshot(void)
{
    status.ap_vote=rd(hwv,PD_EN);status.pd_done=rd(hwv,PD_DONE);
    status.all_vote=rd(hwv,PD_ALL);status.pd_busy=rd(hwv,PD_BUSY);
    status.flags&=~X9BoardRegistersValid;
    status.pending=status.sleep=status.dpsw_req=status.dpsw_ack=UINT32_MAX;
    status.vlogic_ack=status.sram_ack=UINT32_MAX;
    X9AudioResourceStatus ticket;
    /* The global ON aggregate alone is not proof of our AP lease. Qualify
     * only using AO HWV/arbiter before touching either domain mapping. */
    bool held=exclusive && resource_ticket && !access_error &&
        !x9_audio_resources_status(&ticket) && ticket.owner==resource_owner &&
        ticket.ticket==resource_ticket && (status.owned_vote&(TOP|INFRA))==(TOP|INFRA) &&
        (status.ap_vote&CHAIN)==CHAIN && (status.pd_done&CHAIN)==CHAIN &&
        (status.all_vote&CHAIN)==CHAIN && !(status.pd_busy&CHAIN);
    if(held) {
        status.pending=rd(cfg,0x15c);status.sleep=rd(cfg,0x158);
        status.dpsw_req=rd(cfg,0x590);status.dpsw_ack=rd(cfg,0x594);
        status.vlogic_ack=rd(cfg2,0x50);status.sram_ack=rd(cfg2,0x54);
        status.flags|=X9BoardRegistersValid;
    }
}
static int guard_failure(uint32_t space,uint32_t reg,uint32_t expected,uint32_t actual,int e)
{
    if(!status.lease_error) {
        status.lease_error=(uint32_t)e;status.guard_space=space;status.guard_reg=reg;
        status.guard_expected=expected;status.guard_actual=actual;
    }
    access_error=e;return e;
}
static int qualify(void)
{
    if(access_error)return access_error;
    if(status.lease_checks!=UINT64_MAX)status.lease_checks++;
    snapshot();
    if(!(status.flags&X9BoardRegistersValid)) {
        X9AudioResourceStatus ticket;x9_audio_resources_status(&ticket);
        if(!exclusive || !resource_ticket || ticket.owner!=resource_owner || ticket.ticket!=resource_ticket)
            return guard_failure(X9BoardGuardLease,UINT32_MAX,resource_owner,ticket.owner,ENXIO);
        if((status.owned_vote&(TOP|INFRA))!=(TOP|INFRA))
            return guard_failure(X9BoardGuardLease,UINT32_MAX,TOP|INFRA,status.owned_vote&(TOP|INFRA),ENXIO);
        if((status.ap_vote&CHAIN)!=CHAIN)
            return guard_failure(X9BoardGuardHWV,PD_EN,CHAIN,status.ap_vote&CHAIN,ENXIO);
        if((status.pd_done&CHAIN)!=CHAIN)
            return guard_failure(X9BoardGuardHWV,PD_DONE,CHAIN,status.pd_done&CHAIN,ENXIO);
        if((status.all_vote&CHAIN)!=CHAIN)
            return guard_failure(X9BoardGuardHWV,PD_ALL,CHAIN,status.all_vote&CHAIN,ENXIO);
        return guard_failure(X9BoardGuardHWV,PD_BUSY,0,status.pd_busy&CHAIN,ENXIO);
    }
    /* WFI, firmware RSV, VLOGIC/SRAM diagnostic values are not guessed
     * wake semantics. The pinned v3 path never initiates runtime suspend. */
    if(status.dpsw_req&1)return guard_failure(X9BoardGuardCFG,0x590,0,status.dpsw_req&1,EBUSY);
    if(status.dpsw_ack&1)return guard_failure(X9BoardGuardCFG,0x594,0,status.dpsw_ack&1,EBUSY);
    return 0;
}
static int validate(void *unused)
{
    (void)unused;lck_mtx_lock(&board_lock);int e=qualify();
    if(e){status.error=(uint32_t)e;x9_audio_resources_fault(resource_owner,resource_ticket,e);}
    lck_mtx_unlock(&board_lock);return e;
}
/* board_lock held. Never use the cached global ON aggregate as admission. */
static int speaker_qualify(uint64_t ticket)
{
    if(!registered || !ticket || borrowed_ticket!=ticket ||
       resource_ticket!=ticket || resource_owner!=X9AudioResourceSpeaker)return EPERM;
    int e=qualify();
    if(e){status.error=(uint32_t)e;x9_audio_resources_fault(resource_owner,ticket,e);}
    return e;
}
int x9_adsp_mt6993_speaker_validate(uint64_t ticket)
{
    lck_mtx_lock(&board_lock);int e=speaker_qualify(ticket);
    lck_mtx_unlock(&board_lock);return e;
}
static int speaker_halted_qualify(uint64_t ticket)
{
    int e=speaker_qualify(ticket);
    if(!e && (!reset_done || started || wake || status.pending ||
       audio_sem_state!=AudioSemIdle))e=EBUSY;
    if(!e && (rd(cfg,AUDIO_SEM_REG)&AUDIO_SEM_BIT))e=EBUSY;
    return e;
}
int x9_adsp_mt6993_speaker_halted_validate(uint64_t ticket)
{
    lck_mtx_lock(&board_lock);int e=speaker_halted_qualify(ticket);
    lck_mtx_unlock(&board_lock);return e;
}
int x9_adsp_mt6993_shared_hold(uint64_t ticket,uint64_t *lease)
{
    if(!lease)return EINVAL;
    *lease=0;lck_mtx_lock(&board_lock);int e=speaker_qualify(ticket);
    if(!e && (!started || shared_child_lease))e=EBUSY;
    if(!e && shared_child_serial==UINT64_MAX)e=EOVERFLOW;
    if(!e){shared_child_lease=++shared_child_serial;*lease=shared_child_lease;}
    lck_mtx_unlock(&board_lock);return e;
}
int x9_adsp_mt6993_shared_drop(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&board_lock);int e=!lease || lease!=shared_child_lease?EPERM:0;
    if(!e)e=speaker_halted_qualify(ticket);
    if(!e)shared_child_lease=0;
    lck_mtx_unlock(&board_lock);return e;
}
int x9_adsp_mt6993_clock_hold(uint64_t ticket,uint64_t *lease)
{
    if(!lease)return EINVAL;
    *lease=0;lck_mtx_lock(&board_lock);int e=speaker_qualify(ticket);
    if(!e && (!started || clock_child_lease))e=EBUSY;
    if(!e && clock_child_serial==UINT64_MAX)e=EOVERFLOW;
    if(!e){clock_child_lease=++clock_child_serial;*lease=clock_child_lease;}
    lck_mtx_unlock(&board_lock);return e;
}
int x9_adsp_mt6993_clock_drop(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&board_lock);int e=!lease || lease!=clock_child_lease?EPERM:0;
    if(!e)e=speaker_halted_qualify(ticket);
    if(!e)clock_child_lease=0;
    lck_mtx_unlock(&board_lock);return e;
}
/* AUDIO IRQ pins parents until final route retirement. Active delivery
 * blocks reset; masked/drained quiescence permits halt but NOT restart/off. */
int x9_adsp_mt6993_audio_irq_hold(uint64_t ticket,uint64_t *lease)
{
    if(!lease)return EINVAL;*lease=0;
    lck_mtx_lock(&board_lock);int e=speaker_qualify(ticket);
    if(!e && (!started || irq_child_lease))e=EBUSY;
    if(!e && irq_child_serial==UINT64_MAX)e=EOVERFLOW;
    if(!e){irq_child_lease=++irq_child_serial;irq_child_quiesced=false;*lease=irq_child_lease;}
    lck_mtx_unlock(&board_lock);return e;
}
int x9_adsp_mt6993_audio_irq_quiesce(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&board_lock);int e=!lease || lease!=irq_child_lease?EPERM:0;
    if(!e)e=speaker_qualify(ticket);
    if(!e)irq_child_quiesced=true;
    lck_mtx_unlock(&board_lock);return e;
}
int x9_adsp_mt6993_audio_irq_drop(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&board_lock);int e=!lease || lease!=irq_child_lease?EPERM:0;
    if(!e && !irq_child_quiesced)e=EBUSY;
    if(!e)e=speaker_halted_qualify(ticket);
    if(!e){irq_child_lease=0;irq_child_quiesced=false;}
    lck_mtx_unlock(&board_lock);return e;
}
int x9_adsp_mt6993_audio_irq_ack(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&board_lock);int e=!lease || lease!=irq_child_lease?EPERM:0;
    if(!e)e=speaker_qualify(ticket);
    if(!e && (!started || irq_child_quiesced))e=EBUSY;
    /* Vendor v3 top half clears core-A SPM wake bit0, then exact AUDIO
     * CLR bit2. CLR is write-only action, NEVER a pending/ownership read. */
    if(!e) {
        uint32_t old=rd(cfg,0x5c);
        e=speaker_qualify(ticket);
        if(!e && (old&1)) {
            wr(cfg,0x5c,old&~1u);e=speaker_qualify(ticket);
            if(!e && rd(cfg,0x5c)!=(old&~1u))e=EIO;
        }
    }
    if(!e){e=speaker_qualify(ticket);if(!e)wr(cfg,0x38,1u<<2);}
    if(!e)e=speaker_qualify(ticket);
    if(e)x9_audio_resources_fault(X9AudioResourceSpeaker,ticket,e);
    lck_mtx_unlock(&board_lock);return e;
}
int x9_adsp_mt6993_audio_sem_acquire(uint64_t ticket,uint64_t *lease)
{
    if(!lease)return EINVAL;
    *lease=0;
    lck_mtx_lock(&board_control_lock);lck_mtx_lock(&board_lock);
    int e=speaker_qualify(ticket);
    if(!e && (!started || audio_sem_state!=AudioSemIdle))e=EBUSY;
    /* An inherited AP bit is foreign ownership, not recursive success. */
    if(!e && (rd(cfg,AUDIO_SEM_REG)&AUDIO_SEM_BIT))e=EBUSY;
    if(!e && audio_sem_serial==UINT64_MAX)e=EOVERFLOW;
    if(!e) {
        audio_sem_lease=++audio_sem_serial;*lease=audio_sem_lease;
        for(unsigned n=0;n<AUDIO_SEM_RETRIES;n++) {
            e=speaker_qualify(ticket);if(e)break;
            /* Fresh zero read permits exactly one attempt. Mark uncertain
             * BEFORE the toggle; never blindly replay an unresolved write. */
            if(rd(cfg,AUDIO_SEM_REG)&AUDIO_SEM_BIT) {
                audio_sem_state=AudioSemLost;e=EBUSY;break;
            }
            audio_sem_state=AudioSemAcquireUncertain;
            wr(cfg,AUDIO_SEM_REG,AUDIO_SEM_BIT);
            e=speaker_qualify(ticket);if(e)break;
            if(rd(cfg,AUDIO_SEM_REG)&AUDIO_SEM_BIT) {
                audio_sem_state=AudioSemHeld;break;
            }
            /* Completed write + qualified readback proves this attempt
             * did not acquire. DSP may still own one of its two ways. */
            audio_sem_state=AudioSemIdle;
            if(n+1==AUDIO_SEM_RETRIES){status.timeouts++;e=ETIMEDOUT;}
            else delay(2);
        }
        if(audio_sem_state==AudioSemIdle){audio_sem_lease=0;*lease=0;}
    }
    if(e)x9_audio_resources_fault(X9AudioResourceSpeaker,ticket,e);
    lck_mtx_unlock(&board_lock);lck_mtx_unlock(&board_control_lock);return e;
}
int x9_adsp_mt6993_audio_sem_validate(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&board_lock);
    int e=!lease || lease!=audio_sem_lease || audio_sem_state!=AudioSemHeld?EPERM:0;
    if(!e)e=speaker_qualify(ticket);
    if(!e && !(rd(cfg,AUDIO_SEM_REG)&AUDIO_SEM_BIT)) {
        audio_sem_state=AudioSemLost;e=ESTALE;
    }
    if(e)x9_audio_resources_fault(X9AudioResourceSpeaker,ticket,e);
    lck_mtx_unlock(&board_lock);return e;
}
int x9_adsp_mt6993_audio_sem_release(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&board_lock);
    int e=!lease || lease!=audio_sem_lease || audio_sem_state==AudioSemIdle?EPERM:0;
    if(!e)e=speaker_qualify(ticket);
    if(!e) {
        uint32_t bit=rd(cfg,AUDIO_SEM_REG)&AUDIO_SEM_BIT;
        if(audio_sem_state==AudioSemLost) {
            /* Explicit cleanup may retire LOST only on a qualified zero
             * AP bit. A one bit could be foreign: never toggle it. This
             * does not clear the caller's historical shared/PCM fault. */
            if(bit)e=ESTALE;
            else {audio_sem_state=AudioSemIdle;audio_sem_lease=0;}
        } else if(audio_sem_state==AudioSemHeld && !bit) {
            audio_sem_state=AudioSemLost;e=ESTALE;
        } else if(audio_sem_state==AudioSemReleaseUncertain && bit) {
            /* A previous release might not have settled. No second toggle:
             * toggling again could reacquire instead of releasing. */
            e=EIO;
        } else {
            if(bit) {
                audio_sem_state=AudioSemReleaseUncertain;
                wr(cfg,AUDIO_SEM_REG,AUDIO_SEM_BIT);
                e=speaker_qualify(ticket);
                if(!e && (rd(cfg,AUDIO_SEM_REG)&AUDIO_SEM_BIT))e=EIO;
            }
            if(!e){audio_sem_state=AudioSemIdle;audio_sem_lease=0;}
        }
    }
    if(e)x9_audio_resources_fault(X9AudioResourceSpeaker,ticket,e);
    lck_mtx_unlock(&board_lock);return e;
}
static int wait_pending_idle(void)
{
    uint64_t ticks;nanoseconds_to_absolutetime(10000000,&ticks);
    uint64_t end=mach_absolute_time()+ticks;
    for(unsigned n=0;n<5001;n++) {
        int e=qualify();if(e)return e;
        if(!status.pending)return 0;
        if(mach_absolute_time()>=end)break;delay(2);
    }
    status.timeouts++;return ETIMEDOUT;
}
/* CCF_XPU(0)=0x20000. DONE/global regs have NO XPU offset. Own a SET
 * before sending it (timeout is ambiguous), and never clear inherited bits. */
static int vote(uint32_t bit,bool on)
{
    int e=wait_bits(hwv,PD_DONE,bit,bit,10000);if(e)return e;
    if(on) {
        if(rd(hwv,PD_EN)&bit)return EBUSY;
        status.owned_vote|=bit;
    } else if(!(status.owned_vote&bit))return 0;
    wr(hwv,on?PD_SET:PD_CLR,bit);
    e=wait_bits(hwv,PD_EN,bit,on?bit:0,200);
    if(!e)e=wait_bits(hwv,PD_DONE,bit,bit,10000);
    if(!e && !on)status.owned_vote&=~bit;
    return e;
}
static int chain_on(void)
{
    const uint32_t bits[]={AO,INFRA,TOP};int e=0;
    for(unsigned i=0;i<3 && !e;i++) {
        uint32_t bit=bits[i];
        if(!(rd(hwv,PD_EN)&bit))e=vote(bit,true);
        if(!e)e=wait_bits(hwv,PD_ALL,bit,bit,10000);
        if(!e)e=wait_bits(hwv,PD_BUSY,bit,0,10000);
    }
    return e;
}
static int chain_off(void)
{
    status.rollback_error=0;
    /* Silicon sw_ver is not yet proven. Keep an owned AO vote for the boot
     * lifetime, covering the vendor A0 erratum without guessing chip tags. */
    const uint32_t bits[]={TOP,INFRA};int e=0;
    uint32_t owned=status.owned_vote;
    for(unsigned i=0;i<2 && !e;i++)if(owned&bits[i])e=vote(bits[i],false);
    if(e) {
        /* Restore accessibility after a partial withdraw. Do not release any
         * remaining resource on error. Both primary and rollback errors live
         * in status. Transport v3 retains this lease for explicit recovery. */
        int restore=chain_on();status.rollback_error=(uint32_t)restore;
        status.owned_vote|=owned & rd(hwv,PD_EN);
    }
    status.restore_error=(uint32_t)e;return e;
}
static int smc(uint32_t op,uint32_t value)
{
    register uint64_t x0 __asm__("x0")=0xc200052bULL;
    register uint64_t x1 __asm__("x1")=op,x2 __asm__("x2")=value,x3 __asm__("x3")=0;
    register uint64_t x4 __asm__("x4")=0,x5 __asm__("x5")=0,x6 __asm__("x6")=0,x7 __asm__("x7")=0;
    __asm__ volatile("smc #0":"+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),
        "+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7)::"memory");
    status.smc_op=op;status.smc_calls++;status.smc_result[0]=x0;
    status.smc_result[1]=x1;status.smc_result[2]=x2;status.smc_result[3]=x3;
    printf("X9 ADSP SMC: op=%u a0=%llx a1=%llx a2=%llx a3=%llx\n",op,
        (unsigned long long)x0,(unsigned long long)x1,(unsigned long long)x2,(unsigned long long)x3);
    /* No positive undocumented result is silently interpreted as success. */
    return x0==0?0:ENXIO;
}
static bool power_held(void *unused)
{ (void)unused;lck_mtx_lock(&board_lock);bool held=exclusive || resource_ticket;lck_mtx_unlock(&board_lock);return held; }
static void begin_control(void)
{
    /* Called only from power_on/reset under the transport core mutex.
     * A waiting admin operation must not clear a running poll's failure. */
    if(new_control){new_control=false;access_error=0;}
}
static int power_on(void *unused)
{
    (void)unused;lck_mtx_lock(&board_lock);int e=0;
    begin_control();
    if(exclusive || resource_ticket)e=EBUSY;
    else {
        status.lease_error=0;status.guard_space=X9BoardGuardLease;
        status.guard_reg=UINT32_MAX;status.guard_expected=status.guard_actual=0;
        /* Claim before even observing a cold baseline; retain until all
         * owned parent transitions settle, not merely until start returns. */
        if(borrowed_ticket) {
            X9AudioResourceStatus ticket;e=x9_audio_resources_status(&ticket);
            if(!e && (ticket.owner!=X9AudioResourceSpeaker || ticket.ticket!=borrowed_ticket))e=EPERM;
            if(!e)resource_ticket=borrowed_ticket;
        } else e=x9_audio_resources_claim(X9AudioResourceADSP,&resource_ticket);
        if(e){status.error=(uint32_t)e;lck_mtx_unlock(&board_lock);return e;}
        snapshot();e=wait_bits(hwv,PD_DONE,CHAIN,CHAIN,10000);
        if(!e && ((rd(hwv,PD_EN)|rd(hwv,PD_ALL)|rd(hwv,PD_BUSY))&(TOP|INFRA)))e=EBUSY;
        if(!e) {
            /* Proven off TOP+INFRA means neither core is executing. The
             * built-in provider owns both reset domains but starts only A. */
            exclusive=true;status.flags|=X9BoardExclusive;
            e=chain_on();
            if(e) {
                int off=chain_off();status.restore_error=(uint32_t)off;
                if(!off){exclusive=false;status.flags&=~X9BoardExclusive;}
                else status.flags|=X9BoardPowered;
            }
        }
        if(!e)status.flags|=X9BoardPowered;
        if(e && !exclusive && resource_ticket) {
            int released=borrowed_ticket?0:x9_audio_resources_release(resource_owner,resource_ticket);
            if(!released)resource_ticket=0;
            else status.restore_error=(uint32_t)released;
        }
        if(e && resource_ticket)x9_audio_resources_fault(resource_owner,resource_ticket,e);
    }
    status.error=(uint32_t)e;snapshot();lck_mtx_unlock(&board_lock);return e;
}
static int reset(void *unused)
{
    (void)unused;lck_mtx_lock(&board_lock);begin_control();int e=exclusive?0:ENXIO;
    if(!e && (audio_sem_state!=AudioSemIdle || (irq_child_lease && !irq_child_quiesced)))e=EBUSY;
    if(!e && access_error)e=access_error;
    if(!e) {
        X9AudioResourceStatus ticket;x9_audio_resources_status(&ticket);
        if(!resource_ticket || ticket.owner!=resource_owner || ticket.ticket!=resource_ticket)
            e=guard_failure(X9BoardGuardLease,UINT32_MAX,resource_owner,ticket.owner,ENXIO);
    }
    if(!e)e=chain_on(); /* recover a partially revoked power lease first */
    if(!e)e=qualify();
    if(!e && (rd(cfg,AUDIO_SEM_REG)&AUDIO_SEM_BIT))e=EBUSY;
    if(!e)e=smc(SMC_STOP,0);
    if(!e)started=false;
    if(!e)e=qualify(); /* secure monitor may have changed domain state */
    if(!e)e=wait_pending_idle();
    if(!e)e=qualify();
    if(!e)e=smc(SMC_CLEAR,0xad5bad5b);
    if(!e) {reset_done=true;status.flags&=~X9BoardStarted;status.resets++;}
    else reset_done=false;
    status.error=(uint32_t)e;snapshot();lck_mtx_unlock(&board_lock);return e;
}
static int core_start(void *unused)
{
    (void)unused;lck_mtx_lock(&board_lock);int e=exclusive&&reset_done&&!started?0:ENXIO;
    if(!e && (audio_sem_state!=AudioSemIdle || shared_child_lease || clock_child_lease || irq_child_lease))e=EBUSY;
    if(!e)e=qualify();
    if(!e && (rd(cfg,AUDIO_SEM_REG)&AUDIO_SEM_BIT))e=EBUSY;
    if(!e)e=smc(SMC_RELOAD,0xad5bad5b);
    if(!e)e=qualify();
    if(!e)e=smc(SMC_START,0);
    reset_done=false;
    if(!e){started=true;status.flags|=X9BoardStarted;}
    status.error=(uint32_t)e;snapshot();lck_mtx_unlock(&board_lock);return e;
}
static int power_off(void *unused)
{
    (void)unused;lck_mtx_lock(&board_lock);int e=0;
    if(started || wake || !reset_done || audio_sem_state!=AudioSemIdle || shared_child_lease || clock_child_lease || irq_child_lease)e=EBUSY;
    else if(exclusive) {
        e=qualify();
        if(!e && (rd(cfg,AUDIO_SEM_REG)&AUDIO_SEM_BIT))e=EBUSY;
        if(!e)e=chain_off();
        if(!e){exclusive=reset_done=false;status.flags&=~(X9BoardPowered|X9BoardExclusive);}
    }
    if(!e && !exclusive && resource_ticket) {
        /* The combined caller owns other children. Clearing our association
         * is not permission to release its AP ticket here. */
        e=borrowed_ticket?0:x9_audio_resources_release(resource_owner,resource_ticket);
        if(!e)resource_ticket=0;
    }
    if(e && resource_ticket)x9_audio_resources_fault(resource_owner,resource_ticket,e);
    status.error=(uint32_t)e;snapshot();lck_mtx_unlock(&board_lock);return e;
}
static int wake_lock(void *unused)
{
    (void)unused;lck_mtx_lock(&board_lock);int e=exclusive&&started&&!wake?0:ENXIO;
    if(!e)e=qualify();
    if(!e){wake=true;status.flags|=X9BoardWake;}
    status.error=(uint32_t)e;lck_mtx_unlock(&board_lock);return e;
}
static int wake_unlock(void *unused)
{
    (void)unused;lck_mtx_lock(&board_lock);int e=wake?0:EINVAL;
    wake=false;status.flags&=~X9BoardWake;lck_mtx_unlock(&board_lock);return e;
}
int x9_adsp_mt6993_init(void)
{
    if(!sweet_early_is_x9() || sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    lck_mtx_lock(&board_control_lock);int e=0;
    if(!registered) {
        copy_layout(); /* metadata only; never use an old Android address */
        /* Device mappings are permanent, created once, and not dereferenced
         * until the explicit admin operation. Never map firmware as Device. */
        if(!hwv)hwv=(volatile uint32_t *)ml_io_map(0x1c040000,0x74900);
        if(!cfg)cfg=(volatile uint32_t *)ml_io_map(0x1a000000,0x5000);
        if(!cfg2)cfg2=(volatile uint32_t *)ml_io_map(0x1a00a000,0x8000);
        static volatile uint32_t *tx,*rx;
        if(!tx)tx=(volatile uint32_t *)ml_io_map(0x1a350000,0x1000);
        if(!rx)rx=(volatile uint32_t *)ml_io_map(0x1a360000,0x1000);
        if(!hwv || !cfg || !cfg2 || !tx || !rx)e=ENOMEM;
        else {
            X9AudioDSPStatus other;x9_adsp_status(1,&other);
            if(other.state!=X9DSPUnconfigured)e=EBUSY;
            else {
                struct x9_adsp_platform p={.version=X9_ADSP_PLATFORM_VERSION,.core=0,.tx=tx,.rx=rx,
                    .power_on=power_on,.power_off=power_off,.stop_and_reset=reset,
                    .core_start=core_start,.wake_lock=wake_lock,.wake_unlock=wake_unlock,
                    .power_held=power_held,.validate=validate};
                e=x9_adsp_register_transport(&p);
                if(!e){registered=true;status.version=X9_AUDIO_BOARD_STATUS_VERSION;status.flags=X9BoardRegistered;}
            }
        }
    }
    lck_mtx_unlock(&board_control_lock);return e;
}
int x9_adsp_mt6993_control(uint32_t op)
{
    if(sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    if(op>1)return EINVAL;
    lck_mtx_lock(&board_control_lock);
    lck_mtx_lock(&board_lock);
    int e=!registered?ENXIO:borrowed_ticket?EBUSY:0;
    if(!e)new_control=true;
    lck_mtx_unlock(&board_lock);
    if(!e)e=op?x9_adsp_shutdown(0):x9_adsp_boot(0);
    lck_mtx_unlock(&board_control_lock);return e;
}
int x9_adsp_mt6993_speaker_control(uint32_t op,uint64_t ticket)
{
    if(sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    if(op>2 || !ticket)return EINVAL;
    lck_mtx_lock(&board_control_lock);lck_mtx_lock(&board_lock);
    X9AudioResourceStatus t;int e=!registered?ENXIO:x9_audio_resources_status(&t);
    if(!e && (t.owner!=X9AudioResourceSpeaker || t.ticket!=ticket))e=EPERM;
    if(!e && !op && (borrowed_ticket || resource_ticket || exclusive))e=EBUSY;
    if(!e && op && borrowed_ticket!=ticket)e=EPERM;
    if(!e) {
        if(!op){borrowed_ticket=ticket;resource_owner=X9AudioResourceSpeaker;}
        /* Only boot/halt can clear the board's per-operation access gate,
         * once serialized by the transport. release does not repair it. */
        if(op!=2)new_control=true;
    }
    lck_mtx_unlock(&board_lock);
    bool attempted=!e;
    if(!e)e=!op?x9_adsp_boot(0):op==1?x9_adsp_halt(0):x9_adsp_release(0);
    if(!e && op==2) {
        lck_mtx_lock(&board_lock);
        if(exclusive || resource_ticket || started || wake || audio_sem_state!=AudioSemIdle || shared_child_lease || clock_child_lease || irq_child_lease)e=EIO;
        else {borrowed_ticket=0;resource_owner=X9AudioResourceADSP;}
        lck_mtx_unlock(&board_lock);
    }
    if(e && attempted)x9_audio_resources_fault(X9AudioResourceSpeaker,ticket,e);
    lck_mtx_unlock(&board_control_lock);return e;
}
int x9_adsp_mt6993_copy_handoff(struct x9_adsp_handoff *h)
{
    if(!h)return EINVAL;
    lck_mtx_lock(&board_lock);int e=x9_adsp_handoff_valid(&layout)?0:ENXIO;
    if(!e)*h=layout;
    lck_mtx_unlock(&board_lock);return e;
}
int x9_adsp_mt6993_status(X9AudioBoardStatus *s)
{
    if(!s)return EINVAL;
    lck_mtx_lock(&board_lock);*s=status;s->version=X9_AUDIO_BOARD_STATUS_VERSION;
    lck_mtx_unlock(&board_lock);return 0; /* cached only: no off-domain reads */
}
