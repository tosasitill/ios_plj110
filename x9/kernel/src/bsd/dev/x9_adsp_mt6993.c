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
#include "x9_adsp_mt6993.h"
#include "x9_adsp_transport.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *,int);
LCK_GRP_DECLARE(board_group,"x9-adsp-board");
LCK_MTX_DECLARE(board_control_lock,&board_group);
LCK_MTX_DECLARE(board_lock,&board_group);
static volatile uint32_t *hwv,*cfg,*cfg2;
static bool registered,exclusive,reset_done,started,wake;
static X9AudioBoardStatus status;
enum { PD_SET=0x20700,PD_CLR=0x20704,PD_EN=0x20708,PD_DONE=0x1132c,
       PD_ALL=0x13700,PD_BUSY=0x13704,TOP=2,INFRA=4,AO=8,CHAIN=14,
       SMC_CLEAR=3,SMC_START=4,SMC_STOP=5,SMC_RELOAD=7 };
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
    if(exclusive && (status.all_vote&CHAIN)==CHAIN && !(status.pd_busy&CHAIN)) {
        status.pending=rd(cfg,0x15c);status.sleep=rd(cfg,0x158);
        status.dpsw_req=rd(cfg,0x590);status.dpsw_ack=rd(cfg,0x594);
        status.vlogic_ack=rd(cfg2,0x50);status.sram_ack=rd(cfg2,0x54);
    }
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
    /* Silicon sw_ver is not yet proven. Keep an owned AO vote for the boot
     * lifetime, covering the vendor A0 erratum without guessing chip tags. */
    const uint32_t bits[]={TOP,INFRA};int e=0;
    uint32_t owned=status.owned_vote;
    for(unsigned i=0;i<2 && !e;i++)if(owned&bits[i])e=vote(bits[i],false);
    if(e) {
        /* Restore accessibility after a partial withdraw. Do not release any
         * remaining resource on error. Both primary and rollback errors live
         * in status. Transport v2 retains this lease for explicit recovery. */
        int restore=chain_on();status.restore_error=(uint32_t)restore;
        status.owned_vote|=owned & rd(hwv,PD_EN);
    }
    return e;
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
{ (void)unused;lck_mtx_lock(&board_lock);bool held=exclusive;lck_mtx_unlock(&board_lock);return held; }
static int power_on(void *unused)
{
    (void)unused;lck_mtx_lock(&board_lock);int e=0;
    if(exclusive)e=EBUSY;
    else {
        snapshot();
        e=wait_bits(hwv,PD_DONE,CHAIN,CHAIN,10000);
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
    }
    status.error=(uint32_t)e;snapshot();lck_mtx_unlock(&board_lock);return e;
}
static int reset(void *unused)
{
    (void)unused;lck_mtx_lock(&board_lock);int e=exclusive?0:ENXIO;
    if(!e)e=chain_on(); /* recover a partially revoked power lease first */
    if(!e)e=smc(SMC_STOP,0);
    if(!e)started=false;
    if(!e)e=wait_bits(cfg,0x15c,UINT32_MAX,0,10000);
    if(!e)e=smc(SMC_CLEAR,0xad5bad5b);
    if(!e) {reset_done=true;status.flags&=~X9BoardStarted;status.resets++;}
    else reset_done=false;
    status.error=(uint32_t)e;snapshot();lck_mtx_unlock(&board_lock);return e;
}
static int core_start(void *unused)
{
    (void)unused;lck_mtx_lock(&board_lock);int e=exclusive&&reset_done&&!started?0:ENXIO;
    if(!e)e=smc(SMC_RELOAD,0xad5bad5b);
    if(!e)e=smc(SMC_START,0);
    reset_done=false;
    if(!e){started=true;status.flags|=X9BoardStarted;}
    status.error=(uint32_t)e;snapshot();lck_mtx_unlock(&board_lock);return e;
}
static int power_off(void *unused)
{
    (void)unused;lck_mtx_lock(&board_lock);int e=0;
    if(started || wake || !reset_done)e=EBUSY;
    else if(exclusive) {
        e=chain_off();
        if(!e){exclusive=reset_done=false;status.flags&=~(X9BoardPowered|X9BoardExclusive);}
    }
    status.error=(uint32_t)e;snapshot();lck_mtx_unlock(&board_lock);return e;
}
static int wake_lock(void *unused)
{
    (void)unused;lck_mtx_lock(&board_lock);int e=exclusive&&started&&!wake?0:ENXIO;
    if(!e) {
        snapshot();
        /* v3 registers no pre_lock/check_core_active callbacks. Do NOT use
         * old infracfg/IPIC/semaphore protocol or interpret firmware RSV as
         * Linux state enum. Pinned, nonsuspended domain + DPSW off is required.
         * A sleeping/changed domain is a hard error, not fake wake success. */
        if((status.all_vote&CHAIN)!=CHAIN || (status.pd_busy&CHAIN) ||
           (status.dpsw_req&1) || (status.dpsw_ack&1))e=ENXIO;
    }
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
                struct x9_adsp_platform p={.version=2,.core=0,.tx=tx,.rx=rx,
                    .power_on=power_on,.power_off=power_off,.stop_and_reset=reset,
                    .core_start=core_start,.wake_lock=wake_lock,.wake_unlock=wake_unlock,
                    .power_held=power_held};
                e=x9_adsp_register_transport(&p);
                if(!e){registered=true;status.version=1;status.flags=X9BoardRegistered;}
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
    int e=registered?(op?x9_adsp_shutdown(0):x9_adsp_boot(0)):ENXIO;
    lck_mtx_unlock(&board_control_lock);return e;
}
int x9_adsp_mt6993_status(X9AudioBoardStatus *s)
{
    if(!s)return EINVAL;
    lck_mtx_lock(&board_lock);*s=status;s->version=1;
    lck_mtx_unlock(&board_lock);return 0; /* cached only: no off-domain reads */
}
