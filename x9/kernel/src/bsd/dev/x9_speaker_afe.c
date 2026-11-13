/* Independent IPM2P0/ETDM configuration from MT6993 ALSA descriptors.
 * This engine has NO board lease provider yet. It never registers Ready and
 * never unmutes a PA. A final audio backend must prefill before DMA start and
 * connect these hardware cursors, not a host-time simulation, to its timeline.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include "x9_speaker_afe.h"
#include "x9_audio_resources.h"
#include "x9_speaker_out4.h"
#include "x9_speaker_dl24.h"
#include "x9_speaker_memif.h"
extern int sweet_x9_arg(const char *,int);
extern int sweet_early_is_x9(void);
LCK_GRP_DECLARE(speaker_afe_group,"x9-speaker-afe");
LCK_MTX_DECLARE(speaker_afe_lock,&speaker_afe_group);
static struct x9_speaker_afe_platform hw;
static struct x9_speaker_afe_status state;
static struct x9_speaker_memif_layout memif;
static bool registered,leased,journaled;
static uint64_t last_poll,last_progress;
static uint64_t resource_ticket;
static bool ticket_owned;
static uint32_t last_output_generation;
/* Inherited O116/O117 mixer inputs other than our bank (LK may leave a route
 * whose source memif is off: it contributes silence). Captured at prepare,
 * must stay exactly unchanged; our own bank must start empty. */
static uint32_t route_base[2][7];
/* Per-operation accessibility failure: fault cleanup may mute/quiesce via
 * independently qualified callbacks, but must not retry AFE/DMA access.
 * A later explicit stop starts a new qualification attempt. */
static int access_error;
enum { STOP_MARGIN_FRAMES=480, WATCHDOG_MS=2 };
static void watchdog(void *,wait_result_t);
struct patch {uint32_t off,mask,value,saved,expected;};
/* OUT4 S16 in 32-bit I2S slots matches get_etdm_wlen(S16_LE)=31, valid
 * width=15. ETDM FS=5, relatch=10; memif FS=10. No IN4 feedback programming.
 * Pad/master/slave/format are AP-master I2S, synchronous sharing OFF.
 * Cowork OUT4->IN4 master choice=0 only configures the inactive selector. */
static struct patch config[]={
    X9_SPEAKER_OUT4_DISABLED_CONFIG,
    X9_SPEAKER_OUT4_COWORK_DISABLED,
    {0x0098,(1u<<5)|(1u<<20),1u<<5,0,0},
    {0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},
    {0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0}, /* explicit layout installed at register */
};
#define COUNT ((unsigned)(sizeof(config)/sizeof(config[0])))
/* Separate from shared five-word descriptors. saved is the lease baseline;
 * prior is the observed value immediately before the LAST attempted write. */
static uint32_t prior[COUNT];
_Static_assert(COUNT==13, "memif journal index10 and route indices11/12");
_Static_assert(sizeof(struct x9_speaker_afe_status)==192,"AFE cached status v6 size");
static int guard_error(unsigned stage,int e,bool configuration,uint32_t off,
                       uint32_t expected,uint32_t actual)
{
    uint32_t *error=configuration?&state.config_error:&state.lease_error;
    if(!*error)*error=(uint32_t)e;
    if(!state.guard_stage) {
        state.guard_stage=stage;state.guard_reg=off;
        state.guard_expected=expected;state.guard_actual=actual;
        state.guard_index=UINT32_MAX;state.guard_prior_valid=0;
        state.guard_prior=state.guard_saved=0;
    }
    return e;
}
static int journal_error(unsigned stage,int e,unsigned i,uint32_t expected,uint32_t actual)
{
    bool first=!state.guard_stage;
    const struct patch *p=&config[i];
    guard_error(stage,e,true,p->off,expected,actual);
    if(first) {
        state.guard_index=i;state.guard_saved=p->saved&p->mask;
        state.guard_prior=prior[i]&p->mask;
        state.guard_prior_valid=(state.uncertain_mask&(1u<<i))!=0;
    }
    return e;
}
static bool known_value(unsigned i,uint32_t actual,bool recovering)
{
    const struct patch *p=&config[i];
    if(actual==(p->expected&p->mask))return true;
    if(recovering && (state.uncertain_mask&(1u<<i)) && actual==(prior[i]&p->mask)) {
        if(state.prior_observations!=UINT64_MAX)state.prior_observations++;
        return true;
    }
    return false;
}
static int check_lease(unsigned stage)
{
    if(access_error)return access_error;
    if(state.lease_checks!=UINT64_MAX)state.lease_checks++;
    X9AudioResourceStatus ticket;
    int e=x9_audio_resources_status(&ticket);
    if(!e && (!leased || !resource_ticket || ticket.owner!=X9AudioResourceSpeaker ||
              ticket.ticket!=resource_ticket))e=EIO;
    if(!e && !hw.held(hw.context))e=ENXIO;
    if(!e)e=hw.validate(hw.context);
    /* Unlike a transient CUR retry, failed ownership/accessibility is fatal. */
    if(e==EAGAIN)e=EIO;
    if(e)access_error=e;
    return e?guard_error(stage,e,false,UINT32_MAX,0,0):0;
}
static uint32_t rd(uint32_t off)
{
    uint32_t v=hw.afe[off/4];__asm__ volatile("dmb oshld":::"memory");
    if(off==memif.con)state.dl24=v;else if(off==0x1580)state.out4=v;
    return v;
}
static void wr(uint32_t off,uint32_t value)
{hw.afe[off/4]=value;__asm__ volatile("dsb sy":::"memory");}
static int change(unsigned i,uint32_t value,unsigned stage)
{
    int e=check_lease(stage);if(e)return e;
    struct patch *p=&config[i];uint32_t old=rd(p->off);
    uint32_t bit=1u<<i;
    /* A failed readback may mean this write never landed. Recover ONLY its
     * target or immediate prewrite value, not an older lease baseline. */
    bool recovering=stage==X9AFEGuardDisable || stage==X9AFEGuardRestore;
    if(!known_value(i,old&p->mask,recovering))
        return journal_error(stage,EBUSY,i,p->expected&p->mask,old&p->mask);
    if(p->off==memif.con && (old&(memif.inactive&~memif.enable)))
        return guard_error(stage,EBUSY,true,p->off,0,old&(memif.inactive&~memif.enable));
    uint32_t next=(old&~p->mask)|(value&p->mask);
    if(p->off==memif.con)next&=~memif.clear;
    if((next&p->mask)==(old&p->mask)) {
        if((state.uncertain_mask&bit) && state.no_write_reconciliations!=UINT64_MAX)
            state.no_write_reconciliations++;
        p->expected=next;state.uncertain_mask&=~bit;return 0;
    }
    /* Capture BEFORE publishing the intended value or issuing MMIO. If a
     * later cleanup changes it again, only that new transition is uncertain. */
    prior[i]=old;
    state.dirty_mask|=bit;state.uncertain_mask|=bit;
    p->expected=next;wr(p->off,next);
    e=check_lease(stage);if(e)return e; /* keep uncertain after revoked access */
    uint32_t actual=rd(p->off)&p->mask;
    if(actual!=(next&p->mask))return journal_error(stage,EIO,i,next&p->mask,actual);
    state.uncertain_mask&=~bit;return 0;
}
/* Recovery accepts only our expected value, or the immediate prior value for
 * an uncertain write. It must also validate untouched/already-restored words. */
static int verify_journal(bool saved,bool recovering,unsigned stage)
{
    int e=check_lease(stage);if(e)return e;
    if(state.config_checks!=UINT64_MAX)state.config_checks++;
    for(unsigned n=0;n<COUNT;n++) {
        const struct patch *p=&config[n];
        uint32_t expected=(saved?p->saved:p->expected)&p->mask,actual=rd(p->off)&p->mask;
        if(saved) {
            if(actual!=expected)return journal_error(stage,EBUSY,n,expected,actual);
        } else if(!known_value(n,actual,recovering))return journal_error(stage,EBUSY,n,expected,actual);
    }
    return 0;
}
static int verify_config(bool saved,unsigned stage)
{return verify_journal(saved,false,stage);}
static int unowned_routes_idle(bool running,unsigned stage)
{
    int e=check_lease(stage);if(e)return e;
    const unsigned banks[]={0,4,8,16,20,24,28};
    for(unsigned ch=0;ch<2;ch++)for(unsigned n=0;n<7;n++) {
        unsigned off=0x2f00+ch*32+banks[n];uint32_t value=rd(off);
        if(off==memif.route[ch])continue; /* selected bank verified by journal */
        if(value!=route_base[ch][n])return guard_error(stage,EBUSY,true,off,route_base[ch][n],value);
    }
    uint32_t other=memif.id==X9_SPEAKER_DL24_MEMIF?0x4640:X9_SPEAKER_DL24_CON0;
    uint32_t other_on=memif.id==X9_SPEAKER_DL24_MEMIF?1u<<31:1u<<28;
    uint32_t value=rd(other)&other_on;
    if(value)return guard_error(stage,EBUSY,true,other,0,value);
    uint32_t mask=memif.inactive;
    if(running)mask&=~memif.enable;
    value=rd(memif.con)&mask;
    return value?guard_error(stage,EBUSY,true,memif.con,0,value):0;
}
static int restore(void)
{
    /* Ordinary masked RW only; CUR/MEM_UP/action bits are never replayed. */
    int e=verify_journal(false,true,X9AFEGuardRestore);if(e)return e;
    e=unowned_routes_idle(false,X9AFEGuardRestore);if(e)return e;
    for(unsigned n=COUNT;n>0;n--) {
        if(!(state.dirty_mask&(1u<<(n-1))))continue;
        int e=change(n-1,config[n-1].saved,X9AFEGuardRestore);if(e)return e;
        state.dirty_mask&=~(1u<<(n-1));
    }
    e=verify_config(true,X9AFEGuardRestore);if(!e)e=unowned_routes_idle(false,X9AFEGuardRestore);if(e)return e;
    journaled=false;return 0;
}
static int disable(void)
{
    /* Do not RMW an untouched or previously restored register. */
    int e=check_lease(X9AFEGuardDisable);if(e)return e;
    if(state.dirty_mask&(1u<<10))e=change(10,config[10].expected&~memif.enable,X9AFEGuardDisable);
    if(e)return e; /* no second write/qualification after a failed disable */
    uint64_t ticks;nanoseconds_to_absolutetime(1000000,&ticks);
    uint64_t end=mach_absolute_time()+ticks;
    for(unsigned n=0;!e && (state.dirty_mask&(1u<<10)) && n<500;n++) {
        e=check_lease(X9AFEGuardDisable);if(e)break;
        if(!(rd(memif.con)&memif.enable))break;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}delay(2);
    }
    if(!e && (state.dirty_mask&(1u<<10))) {
        e=check_lease(X9AFEGuardDisable);
        if(!e && (rd(memif.con)&memif.enable))e=ETIMEDOUT;
    }
    if(e)return e;
    if(state.dirty_mask&1)e=change(0,config[0].expected&~1u,X9AFEGuardDisable);
    if(!e)state.running=0;
    return e;
}
static int drop_ticket(void)
{
    X9AudioResourceStatus t;int e=x9_audio_resources_status(&t);
    if(!e && (!resource_ticket || t.owner!=X9AudioResourceSpeaker || t.ticket!=resource_ticket))e=EPERM;
    if(!e && ticket_owned)e=x9_audio_resources_release(X9AudioResourceSpeaker,resource_ticket);
    if(!e){resource_ticket=0;ticket_owned=false;leased=false;state.poisoned=0;}
    return e;
}
static int quiesce_locked(void)
{
    if(!leased)return 0;
    state.prepared=0;
    if(!hw.held(hw.context))return EIO; /* never touch a lost physical lease */
    /* Mute failure still stops DMA/producer; no failed step permits restoring
     * a mapping or dropping a dependency. Watchdog uses this same sequence. */
    int muted=hw.mute(hw.context);state.mute_error=(uint32_t)muted;
    int disabled=journaled?disable():0;state.disable_error=(uint32_t)disabled;
    int quiet=journaled?hw.quiesce(hw.context):0;state.quiesce_error=(uint32_t)quiet;
    int halted=0;
    if(!disabled && !quiet && hw.halt_producer)halted=hw.halt_producer(hw.context);
    return disabled?disabled:quiet?quiet:muted?muted:halted;
}
static int stop_locked(void)
{
    if(!leased)return 0;
    state.prepared=0; /* retained resources never authorize another start */
    if(!hw.held(hw.context)) {
        /* No off-domain mute/AFE read after successful or empty unwind.
         * Losing the physical lease BEFORE restoring our AFE journal is
         * inconsistent and must keep the transition ticket quarantined. */
        int e=journaled?EIO:drop_ticket();
        state.restore_error=(uint32_t)e;
        if(e){state.poisoned=1;x9_audio_resources_fault(X9AudioResourceSpeaker,resource_ticket,e);}
        return e;
    }
    int e=quiesce_locked();
    if(!e && journaled)e=restore();
    if(!e)e=hw.release(hw.context);
    if(!e && hw.held(hw.context))e=EIO; /* callback success alone is not released ownership */
    if(!e)e=drop_ticket();
    state.restore_error=(uint32_t)e;
    if(e){state.poisoned=1;x9_audio_resources_fault(X9AudioResourceSpeaker,resource_ticket,e);}
    return e;
}
static int platform_layout(const struct x9_speaker_afe_platform *p,
                           struct x9_speaker_memif_layout *layout)
{
    if(!sweet_early_is_x9() || sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    if(!p || p->version!=X9_SPEAKER_AFE_PLATFORM_VERSION || p->verified!=X9_SPEAKER_AFE_RESOURCES || !p->afe ||
       !p->dma || !p->acquire || !p->release || !p->mute || !p->quiesce || !p->held || !p->validate ||
       ((uintptr_t)p->afe&3) || ((uintptr_t)p->dma&63) || (p->dma_bus&63) ||
       !p->dma_bus || p->dma_bytes<65536 || p->dma_bytes>131072 || (p->dma_bytes&63) ||
       p->dma_bus>(1ULL<<41)-p->dma_bytes)return EINVAL;
    if(p->producer==X9AFEProducerCPU) {
        if(p->generation || p->output_committed || !p->sync_device)return EINVAL;
    } else if(p->producer==X9AFEProducerDSP) {
        if(!p->generation || !p->output_committed || !p->halt_producer)return EINVAL;
    } else return EINVAL;
    return x9_speaker_memif_layout(p->memif,layout)?0:EINVAL;
}
static void install_platform(const struct x9_speaker_afe_platform *p,
                             const struct x9_speaker_memif_layout *layout)
{
    hw=*p;memif=*layout;
    config[6]=(struct patch){memif.base_msb,X9_SPEAKER_MEMIF_ADDR_MSB_MASK,0,0,0};
    config[7]=(struct patch){memif.base,X9_SPEAKER_MEMIF_ADDR_LOW_MASK,0,0,0};
    config[8]=(struct patch){memif.end_msb,X9_SPEAKER_MEMIF_ADDR_MSB_MASK,0,0,0};
    config[9]=(struct patch){memif.end,X9_SPEAKER_MEMIF_ADDR_LOW_MASK,0,0,0};
    config[10]=(struct patch){memif.con,memif.format_mask,memif.format_value,0,0};
    for(unsigned i=0;i<2;i++)config[11+i]=(struct patch){memif.route[i],UINT32_MAX,memif.source[i],0,0};
    state.version=X9_SPEAKER_AFE_STATUS_VERSION;state.memif=memif.id;
    state.producer=hw.producer;state.generation=hw.generation;
}
int x9_speaker_afe_register(const struct x9_speaker_afe_platform *p)
{
    struct x9_speaker_memif_layout layout;
    int e=platform_layout(p,&layout);if(e)return e;
    lck_mtx_lock(&speaker_afe_lock);e=registered?EBUSY:0;
    if(!e) {
        install_platform(p,&layout);
        /* Independent boot-lifetime watcher, not user/HAL position polling.
         * Scheduler latency is not a hardware deadline guarantee. No PA is
         * unmuted by this engine; a final provider still needs that proof. */
        thread_t thread;
        if(kernel_thread_start(watchdog,NULL,&thread)!=KERN_SUCCESS)e=ENOMEM;
        else {thread_deallocate(thread);registered=true;}
        if(e)bzero(&hw,sizeof(hw));
    }
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_rebind_dsp(const struct x9_speaker_afe_platform *p)
{
    struct x9_speaker_memif_layout layout;
    int e=platform_layout(p,&layout);if(e)return e;
    if(p->producer!=X9AFEProducerDSP)return EINVAL;
    lck_mtx_lock(&speaker_afe_lock);e=registered?0:ENXIO;
    if(!e && (hw.producer!=X9AFEProducerDSP || leased || journaled || resource_ticket ||
       state.running || state.prepared || state.poisoned || state.dirty_mask ||
       state.uncertain_mask || state.copy_pending || hw.held(hw.context)))e=EBUSY;
    if(!e && (p->generation<=hw.generation || p->generation<=last_output_generation))e=ESTALE;
    if(!e)install_platform(p,&layout);
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
static int prepare(uint64_t borrowed)
{
    lck_mtx_lock(&speaker_afe_lock);int e=registered?0:ENXIO;
    if(!e && (leased || state.poisoned))e=EBUSY;
    if(!e && borrowed && !hw.halt_producer)e=ENXIO;
    if(!e && hw.producer==X9AFEProducerDSP &&
       (!borrowed || hw.generation<=last_output_generation))e=EBUSY;
    if(e){lck_mtx_unlock(&speaker_afe_lock);return e;}
    if(borrowed) {
        X9AudioResourceStatus t;e=x9_audio_resources_status(&t);
        if(!e && (t.owner!=X9AudioResourceSpeaker || t.ticket!=borrowed))e=EPERM;
        if(!e)resource_ticket=borrowed;
    } else e=x9_audio_resources_claim(X9AudioResourceSpeaker,&resource_ticket);
    if(e){lck_mtx_unlock(&speaker_afe_lock);return e;}
    ticket_owned=!borrowed;
    if(hw.producer==X9AFEProducerDSP)last_output_generation=hw.generation;
    leased=true;state.error=state.restore_error=state.copy_pending=0;
    access_error=0;
    state.mute_error=state.disable_error=state.quiesce_error=0;
    state.lease_error=state.config_error=state.guard_stage=0;
    state.guard_reg=UINT32_MAX;state.guard_expected=state.guard_actual=0;
    state.guard_index=UINT32_MAX;state.guard_prior_valid=0;state.guard_prior=state.guard_saved=0;
    e=hw.acquire(hw.context);
    if(!e)e=check_lease(X9AFEGuardPrepare);
    if(!e) {
        e=hw.mute(hw.context);
        if(!e)e=check_lease(X9AFEGuardPrepare);
        if(!e) {
            uint32_t value=rd(memif.con)&memif.inactive;
            if(value)e=guard_error(X9AFEGuardPrepare,EBUSY,true,memif.con,0,value);
            else if((value=rd(0x1580)&1))e=guard_error(X9AFEGuardPrepare,EBUSY,true,0x1580,0,value);
        }
        /* There is no CONN116_3 in this SoC table. Never clear a foreign
         * route; our own bank must start empty, others are baselined. */
        const unsigned banks[]={0,4,8,16,20,24,28};
        bool any=false;
        for(unsigned ch=0;!e && ch<2;ch++)for(unsigned b=0;b<7;b++) {
            unsigned off=0x2f00+ch*32+banks[b];uint32_t value=rd(off);
            route_base[ch][b]=off==memif.route[ch]?0:value;if(value)any=true;
            if(off==memif.route[ch] && value){e=guard_error(X9AFEGuardPrepare,EBUSY,true,off,0,value);break;}
        }
        if(any)printf("X9 SPK AFE: inherited O116 %x/%x/%x/%x/%x/%x/%x O117 %x/%x/%x/%x/%x/%x/%x\n",
            route_base[0][0],route_base[0][1],route_base[0][2],route_base[0][3],route_base[0][4],route_base[0][5],route_base[0][6],
            route_base[1][0],route_base[1][1],route_base[1][2],route_base[1][3],route_base[1][4],route_base[1][5],route_base[1][6]);
        if(!e)e=unowned_routes_idle(false,X9AFEGuardPrepare);
        if(!e) {
            uint64_t base=hw.dma_bus,end=base+hw.dma_bytes-1;
            config[6].value=(uint32_t)(base>>32);config[7].value=(uint32_t)base;
            config[8].value=(uint32_t)(end>>32);config[9].value=(uint32_t)end;
            for(unsigned i=0;i<COUNT;i++) {
                config[i].saved=config[i].expected=rd(config[i].off);prior[i]=config[i].saved;
            }
            state.dirty_mask=state.uncertain_mask=0;
            journaled=true;
            for(unsigned i=0;!e && i<COUNT;i++)e=change(i,config[i].value,X9AFEGuardPrepare);
        }
        if(!e)e=verify_config(false,X9AFEGuardPrepare);
        if(!e)e=unowned_routes_idle(false,X9AFEGuardPrepare);
        if(!e){
            if(hw.producer==X9AFEProducerDSP) {
                state.played=state.submitted=state.anchor=0;state.cursor=0;
            }
            state.prepared=1;state.dl24=rd(memif.con);state.out4=rd(0x1580);
        }
    }
    if(e){int off=stop_locked();if(off)state.restore_error=(uint32_t)off;}
    /* Admission errors returned above did not alter the prior stream.
     * Retained physical or transition ownership prevents a new prepare. */
    if(!leased || !state.running)state.error=(uint32_t)e;
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_prepare(void){return prepare(0);}
int x9_speaker_afe_prepare_on_ticket(uint64_t ticket)
{return ticket?prepare(ticket):EINVAL;}
static int copy_frames(const int16_t *pcm,uint32_t frames,unsigned stage)
{
    if(hw.producer!=X9AFEProducerCPU)return ENOTSUP;
    if(!pcm || !frames || frames>hw.dma_bytes/4)return EINVAL;
    int e=verify_config(false,stage);if(e)return e;
    e=unowned_routes_idle(state.running!=0,stage);if(e)return e;
    if(state.copy_pending || state.played>state.submitted || state.submitted>UINT64_MAX-frames)return EIO;
    if(state.submitted-state.played+frames>hw.dma_bytes/4-32)return EAGAIN;
    uint32_t offset=(uint32_t)(state.submitted%(hw.dma_bytes/4))*4;
    uint32_t bytes=frames*4,first=MIN(bytes,hw.dma_bytes-offset);
    state.copy_pending=frames; /* even sync failure may have exposed PCM */
    memcpy((uint8_t *)hw.dma+offset,pcm,first);
    e=hw.sync_device(hw.context,offset,first);
    if(!e)e=verify_config(false,stage);
    if(!e)e=unowned_routes_idle(state.running!=0,stage);
    if(!e && first<bytes) {
        memcpy(hw.dma,(const uint8_t *)pcm+first,bytes-first);
        e=hw.sync_device(hw.context,0,bytes-first);
        if(!e)e=verify_config(false,stage);
        if(!e)e=unowned_routes_idle(state.running!=0,stage);
    }
    if(!e){state.submitted+=frames;state.copy_pending=0;}
    else {state.poisoned=1;if(e==EAGAIN)e=EIO;} /* no "consumed nothing" lie */
    return e;
}
static int refresh_output(unsigned stage)
{
    if(hw.producer!=X9AFEProducerDSP)return ENOTSUP;
    int e=verify_config(false,stage);if(e)return e;
    e=unowned_routes_idle(state.running!=0,stage);if(e)return e;
    uint64_t candidate=0;
    e=hw.output_committed(hw.context,hw.generation,memif.id,
                           hw.dma_bus,hw.dma_bytes,&candidate);
    if(e)return e==EAGAIN?EIO:e; /* invalid authority, not insufficient prefill */
    e=verify_config(false,stage);if(e)return e;
    e=unowned_routes_idle(state.running!=0,stage);if(e)return e;
    if(state.played>state.submitted || candidate<state.submitted ||
       candidate-state.played>hw.dma_bytes/4-32)return EPROTO;
    /* No AP DMA memory read, copy, clean or invalidate. The producer's
     * publication proof includes DSP->AFE visibility of this exact prefix. */
    state.submitted=candidate;return 0;
}
static int enable_locked(uint64_t *anchor)
{
    int e=change(0,config[0].expected|1,X9AFEGuardStart);
    if(!e)e=verify_config(false,X9AFEGuardStart);
    if(!e)e=unowned_routes_idle(false,X9AFEGuardStart);
    if(!e && hw.producer==X9AFEProducerDSP)e=refresh_output(X9AFEGuardStart);
    if(!e) {
        uint64_t enabled_at=mach_absolute_time();
        e=change(10,config[10].expected|memif.enable,X9AFEGuardStart);
        if(!e)e=verify_config(false,X9AFEGuardStart);
        if(!e)e=unowned_routes_idle(true,X9AFEGuardStart);
        if(!e) {
            state.anchor=enabled_at;
            state.running=1;last_poll=last_progress=state.anchor;*anchor=state.anchor;
            thread_wakeup((event_t)&state.running);
        }
    }
    return e;
}
int x9_speaker_afe_start(const int16_t *pcm,uint32_t frames,uint64_t *anchor)
{
    if(!anchor)return EINVAL;
    *anchor=0;
    if(!pcm || frames<960)return EINVAL;
    lck_mtx_lock(&speaker_afe_lock);int e=state.prepared&&!state.running&&!state.error?0:ENXIO;
    if(hw.producer!=X9AFEProducerCPU){lck_mtx_unlock(&speaker_afe_lock);return ENOTSUP;}
    if(!e) {
        if(frames>hw.dma_bytes/4-32) {
            lck_mtx_unlock(&speaker_afe_lock);return EINVAL;
        }
        access_error=0;
        /* A prepared cached journal is not proof the addresses/clock/pads
         * still belong to us. Check before even touching the CPU DMA VA. */
        e=verify_config(false,X9AFEGuardStart);
        if(!e)e=unowned_routes_idle(false,X9AFEGuardStart);
        if(!e) {
            state.played=state.submitted=state.cursor=0;state.anchor=0;
            state.copy_pending=hw.dma_bytes/4;
            bzero(hw.dma,hw.dma_bytes);e=hw.sync_device(hw.context,0,hw.dma_bytes);
            if(e==EAGAIN)e=EIO;
            if(!e)e=verify_config(false,X9AFEGuardStart);
            if(!e)e=unowned_routes_idle(false,X9AFEGuardStart);
            if(!e)state.copy_pending=0;
        }
        if(!e)e=copy_frames(pcm,frames,X9AFEGuardStart);
        if(!e)e=enable_locked(anchor);
        if(e){state.error=(uint32_t)e;state.poisoned=1;(void)stop_locked();}
    }
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_start_dsp(uint64_t *anchor)
{
    if(!anchor)return EINVAL;*anchor=0;
    lck_mtx_lock(&speaker_afe_lock);
    if(hw.producer!=X9AFEProducerDSP){lck_mtx_unlock(&speaker_afe_lock);return ENOTSUP;}
    int e=state.error?(int)state.error:state.prepared&&!state.running?0:ENXIO;
    if(!e) {
        access_error=0;e=refresh_output(X9AFEGuardStart);
        /* No enable writes and no fault for a proved but short prefix. */
        if(!e && state.submitted<960){lck_mtx_unlock(&speaker_afe_lock);return EAGAIN;}
        if(!e)e=enable_locked(anchor);
        if(e){state.error=(uint32_t)e;state.poisoned=1;(void)stop_locked();}
    }
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
static int position_locked(void)
{
    int e=verify_config(false,X9AFEGuardPoll);if(e)return e;
    e=unowned_routes_idle(true,X9AFEGuardPoll);if(e)return e;
    uint64_t now=mach_absolute_time(),elapsed,stalled;
    absolutetime_to_nanoseconds(now-last_poll,&elapsed);
    absolutetime_to_nanoseconds(now-last_progress,&stalled);
    if(state.played>state.submitted)return EIO;
    uint64_t pending=state.submitted-state.played;
    if(pending<=STOP_MARGIN_FRAMES){state.low_water_stops++;return EPIPE;}
    /* A delayed AP poll cannot prove the DMA did not run out between reads.
     * Fail closed instead of extending the deadline from a stale cursor. */
    uint64_t budget=(pending-STOP_MARGIN_FRAMES)*1000000000/48000;
    if(elapsed>=budget || stalled>=MIN(budget,10000000ULL)) {
        state.late_polls++;return ETIMEDOUT;
    }
    if(hw.producer==X9AFEProducerDSP) {
        e=refresh_output(X9AFEGuardPoll);if(e)return e;
        now=mach_absolute_time();
        absolutetime_to_nanoseconds(now-last_poll,&elapsed);
        absolutetime_to_nanoseconds(now-last_progress,&stalled);
        /* A late publication cannot retroactively prove old PCM survived.
         * Keep the OLD confirmed-prefix deadline across supplier latency. */
        if(elapsed>=budget || stalled>=MIN(budget,10000000ULL)) {
            state.late_polls++;return ETIMEDOUT;
        }
    }
    /* Half-ring ambiguity must fail; never infer number of wraps by time. */
    if(elapsed>=(uint64_t)hw.dma_bytes/8*1000000000/48000 || stalled>=250000000)return ETIMEDOUT;
    uint32_t high=0,low=0;bool stable=false;
    e=check_lease(X9AFEGuardPoll);if(e)return e;
    for(unsigned n=0;n<3;n++) {
        high=rd(memif.cur_msb)&X9_SPEAKER_MEMIF_ADDR_MSB_MASK;
        low=rd(memif.cur);
        if(high==(rd(memif.cur_msb)&X9_SPEAKER_MEMIF_ADDR_MSB_MASK)){stable=true;break;}
    }
    if(!stable)return EAGAIN;
    uint64_t ptr=((uint64_t)high<<32)|low;
    if(!ptr)return stalled<10000000?EAGAIN:ETIMEDOUT;
    if(ptr<hw.dma_bus || ptr>=hw.dma_bus+hw.dma_bytes || (ptr&3))return EIO;
    uint32_t next=(uint32_t)(ptr-hw.dma_bus);
    uint32_t bytes=(next+hw.dma_bytes-state.cursor)%hw.dma_bytes;
    if(bytes>hw.dma_bytes/2 || bytes/4>state.submitted-state.played)return EIO;
    if(bytes){state.played+=bytes/4;state.cursor=next;last_progress=now;}
    last_poll=now;
    if(state.submitted-state.played<=STOP_MARGIN_FRAMES){state.low_water_stops++;return EPIPE;}
    return 0;
}
static void fault_locked(int e)
{
    if(!state.error)state.error=(uint32_t)e;
    state.poisoned=1;(void)stop_locked();
}
static void watchdog(void *unused,wait_result_t result)
{
    (void)unused;(void)result;
    for(;;) {
        lck_mtx_lock(&speaker_afe_lock);
        if(!state.running || state.error) {
            /* Arm while locked so a successful start cannot lose the wake. */
            assert_wait((event_t)&state.running,THREAD_UNINT);
            lck_mtx_unlock(&speaker_afe_lock);
            thread_block(THREAD_CONTINUE_NULL);continue;
        }
        access_error=0;
        int e=position_locked();if(e && e!=EAGAIN)fault_locked(e);
        lck_mtx_unlock(&speaker_afe_lock);
        delay_for_interval(WATCHDOG_MS,1000*NSEC_PER_USEC);
    }
}
int x9_speaker_afe_position(uint64_t *played)
{
    if(!played)return EINVAL;
    lck_mtx_lock(&speaker_afe_lock);access_error=0;
    int e=state.error?(int)state.error:state.running?position_locked():ENXIO;
    if(!e)*played=state.played;
    else if(e!=EAGAIN && state.running && !state.error)fault_locked(e);
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_submit(const int16_t *pcm,uint32_t frames)
{
    if(!pcm || !frames)return EINVAL;
    lck_mtx_lock(&speaker_afe_lock);access_error=0;
    if(hw.producer!=X9AFEProducerCPU){lck_mtx_unlock(&speaker_afe_lock);return ENOTSUP;}
    int e=state.error?(int)state.error:state.running?position_locked():ENXIO;
    if(!e)e=copy_frames(pcm,frames,X9AFEGuardCopy);
    if(e && e!=EAGAIN && state.running && !state.error)fault_locked(e);
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_stop(void)
{
    lck_mtx_lock(&speaker_afe_lock);access_error=0;int e=stop_locked();
    if(e && !state.error)state.error=(uint32_t)e;
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_quiesce(void)
{
    lck_mtx_lock(&speaker_afe_lock);access_error=0;int e=quiesce_locked();
    if(e && leased) {
        if(!state.error)state.error=(uint32_t)e;
        state.poisoned=1;state.restore_error=(uint32_t)e;
        x9_audio_resources_fault(X9AudioResourceSpeaker,resource_ticket,e);
    }
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_status(struct x9_speaker_afe_status *s)
{
    if(!s)return EINVAL;lck_mtx_lock(&speaker_afe_lock);*s=state;
    s->version=X9_SPEAKER_AFE_STATUS_VERSION;s->registered=registered;s->leased=leased;
    lck_mtx_unlock(&speaker_afe_lock);return 0;
}
