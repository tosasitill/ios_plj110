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
extern int sweet_x9_arg(const char *,int);
extern int sweet_early_is_x9(void);
LCK_GRP_DECLARE(speaker_afe_group,"x9-speaker-afe");
LCK_MTX_DECLARE(speaker_afe_lock,&speaker_afe_group);
static struct x9_speaker_afe_platform hw;
static struct x9_speaker_afe_status state;
static bool registered,leased,journaled;
static uint64_t last_poll,last_progress;
static uint64_t resource_ticket;
enum { STOP_MARGIN_FRAMES=480, WATCHDOG_MS=2 };
static void watchdog(void *,wait_result_t);
struct patch {uint32_t off,mask,value,saved,expected;};
/* OUT4 S16 in 32-bit I2S slots matches get_etdm_wlen(S16_LE)=31, valid
 * width=15. ETDM FS=5, relatch=10; DL24 FS=10. No IN4 feedback programming.
 * Pad/master/slave/format are AP-master I2S, synchronous sharing OFF.
 * Cowork OUT4->IN4 master choice=0 only configures the inactive selector. */
static struct patch config[]={
    X9_SPEAKER_OUT4_DISABLED_CONFIG,
    X9_SPEAKER_OUT4_COWORK_DISABLED,
    {0x0098,(1u<<5)|(1u<<20),1u<<5,0,0},
    {0x46b0,UINT32_MAX,0,0,0},{0x46b4,UINT32_MAX,0,0,0},
    {0x46c0,UINT32_MAX,0,0,0},{0x46c4,UINT32_MAX,0,0,0},
    X9_SPEAKER_DL24_DISABLED_FORMAT,
    X9_SPEAKER_DL24_DISABLED_ROUTES,
};
#define COUNT ((unsigned)(sizeof(config)/sizeof(config[0])))
_Static_assert(COUNT==13, "DL24 journal index10 and route indices11/12");
static uint32_t rd(uint32_t off)
{
    uint32_t v=hw.afe[off/4];__asm__ volatile("dmb oshld":::"memory");
    if(off==0x46d0)state.dl24=v;else if(off==0x1580)state.out4=v;
    return v;
}
static void wr(uint32_t off,uint32_t value)
{hw.afe[off/4]=value;__asm__ volatile("dsb sy":::"memory");}
static int change(unsigned i,uint32_t value)
{
    struct patch *p=&config[i];uint32_t old=rd(p->off);
    uint32_t bit=1u<<i;
    /* A failed readback may mean the write never landed. Only the two
     * journaled values are recoverable; never adopt a third/foreign value. */
    if((old&p->mask)!=(p->expected&p->mask) &&
       !((state.uncertain_mask&bit) && (old&p->mask)==(p->saved&p->mask)))return EBUSY;
    if(p->off==X9_SPEAKER_DL24_CON0 && (old&(X9_SPEAKER_DL24_INACTIVE_MASK&~(1u<<28))))return EBUSY;
    uint32_t next=(old&~p->mask)|(value&p->mask);
    if(p->off==X9_SPEAKER_DL24_CON0)next&=~X9_SPEAKER_DL24_CLEAR_MASK;
    if((next&p->mask)==(old&p->mask)) {
        p->expected=next;state.uncertain_mask&=~bit;return 0;
    }
    state.dirty_mask|=bit;state.uncertain_mask|=bit;
    p->expected=next;wr(p->off,next);
    if((rd(p->off)&p->mask)!=(next&p->mask))return EIO;
    state.uncertain_mask&=~bit;return 0;
}
static int verify_config(bool saved)
{
    for(unsigned n=0;n<COUNT;n++) {
        const struct patch *p=&config[n];
        if((rd(p->off)&p->mask)!=((saved?p->saved:p->expected)&p->mask))return EBUSY;
    }
    return 0;
}
static int unowned_routes_idle(void)
{
    const unsigned banks[]={0,4,16,20,24,28};
    for(unsigned ch=0;ch<2;ch++)for(unsigned n=0;n<6;n++)
        if(rd(0x2f00+ch*32+banks[n]))return EBUSY;
    return rd(X9_SPEAKER_DL24_CON0)&X9_SPEAKER_DL24_INACTIVE_MASK?EBUSY:0;
}
static int restore(void)
{
    /* Ordinary masked RW only; CUR/MEM_UP/action bits are never replayed. */
    int e=unowned_routes_idle();if(e)return e;
    for(unsigned n=COUNT;n>0;n--) {
        if(!(state.dirty_mask&(1u<<(n-1))))continue;
        int e=change(n-1,config[n-1].saved);if(e)return e;
        state.dirty_mask&=~(1u<<(n-1));
    }
    e=verify_config(true);if(!e)e=unowned_routes_idle();if(e)return e;
    journaled=false;return 0;
}
static int disable(void)
{
    /* Do not RMW an untouched or previously restored register. */
    int e=0;
    if(state.dirty_mask&(1u<<10))e=change(10,config[10].expected&~(1u<<28));
    uint64_t ticks;nanoseconds_to_absolutetime(1000000,&ticks);
    uint64_t end=mach_absolute_time()+ticks;
    for(unsigned n=0;!e && (state.dirty_mask&(1u<<10)) && n<500;n++) {
        if(!(rd(0x46d0)&(1u<<28)))break;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}delay(2);
    }
    if(!e && (state.dirty_mask&(1u<<10)) && (rd(0x46d0)&(1u<<28)))e=ETIMEDOUT;
    int out=0;
    if(state.dirty_mask&1)out=change(0,config[0].expected&~1u);
    if(!e)e=out;
    if(!e)state.running=0;
    return e;
}
static int stop_locked(void)
{
    if(!leased)return 0;
    state.prepared=0; /* retained resources never authorize another start */
    if(!hw.held(hw.context)) {
        /* No off-domain mute/AFE read after successful or empty unwind.
         * Losing the physical lease BEFORE restoring our AFE journal is
         * inconsistent and must keep the transition ticket quarantined. */
        int e=journaled?EIO:x9_audio_resources_release(X9AudioResourceSpeaker,resource_ticket);
        state.restore_error=(uint32_t)e;
        if(!e){resource_ticket=0;leased=false;state.poisoned=0;}
        else {state.poisoned=1;x9_audio_resources_fault(X9AudioResourceSpeaker,resource_ticket,e);}
        return e;
    }
    /* Still stop DMA if mute fails. A failed mute never authorizes releasing
     * clocks/I2C/rails or declaring the PA safely off. */
    int muted=hw.mute(hw.context);state.mute_error=(uint32_t)muted;
    int disabled=journaled?disable():0;state.disable_error=(uint32_t)disabled;
    int quiet=journaled?hw.quiesce(hw.context):0;state.quiesce_error=(uint32_t)quiet;
    int e=disabled?disabled:quiet?quiet:muted;
    if(!e && journaled)e=restore();
    if(!e)e=hw.release(hw.context);
    if(!e && hw.held(hw.context))e=EIO; /* callback success alone is not released ownership */
    if(!e)e=x9_audio_resources_release(X9AudioResourceSpeaker,resource_ticket);
    state.restore_error=(uint32_t)e;
    if(!e){resource_ticket=0;leased=false;state.poisoned=0;}
    else {state.poisoned=1;x9_audio_resources_fault(X9AudioResourceSpeaker,resource_ticket,e);}
    return e;
}
int x9_speaker_afe_register(const struct x9_speaker_afe_platform *p)
{
    if(!sweet_early_is_x9() || sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    if(!p || p->version!=X9_SPEAKER_AFE_PLATFORM_VERSION || p->verified!=X9_SPEAKER_AFE_RESOURCES || !p->afe ||
       !p->dma || !p->acquire || !p->release || !p->mute || !p->quiesce || !p->sync_device || !p->held ||
       ((uintptr_t)p->afe&3) || ((uintptr_t)p->dma&63) || (p->dma_bus&63) ||
       !p->dma_bus || p->dma_bytes<65536 || p->dma_bytes>131072 || (p->dma_bytes&63) ||
       p->dma_bus>(1ULL<<41)-p->dma_bytes)return EINVAL;
    lck_mtx_lock(&speaker_afe_lock);int e=registered?EBUSY:0;
    if(!e) {
        hw=*p;
        /* Independent boot-lifetime watcher, not user/HAL position polling.
         * Scheduler latency is not a hardware deadline guarantee. No PA is
         * unmuted by this engine; a final provider still needs that proof. */
        thread_t thread;
        if(kernel_thread_start(watchdog,NULL,&thread)!=KERN_SUCCESS)e=ENOMEM;
        else {thread_deallocate(thread);registered=true;state.version=X9_SPEAKER_AFE_STATUS_VERSION;}
        if(e)bzero(&hw,sizeof(hw));
    }
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_prepare(void)
{
    lck_mtx_lock(&speaker_afe_lock);int e=registered?0:ENXIO;
    if(!e && (leased || state.poisoned))e=EBUSY;
    if(e){lck_mtx_unlock(&speaker_afe_lock);return e;}
    e=x9_audio_resources_claim(X9AudioResourceSpeaker,&resource_ticket);
    if(e){lck_mtx_unlock(&speaker_afe_lock);return e;}
    leased=true;state.error=state.restore_error=state.copy_pending=0;
    state.mute_error=state.disable_error=state.quiesce_error=0;
    e=hw.acquire(hw.context);
    if(!e && !hw.held(hw.context))e=EIO;
    if(!e) {
        e=hw.mute(hw.context);
        if(!e && ((rd(X9_SPEAKER_DL24_CON0)&X9_SPEAKER_DL24_INACTIVE_MASK) || (rd(0x1580)&1)))e=EBUSY;
        /* Reject other enabled mixer sources. There is no CONN116_3 in this
         * SoC table. Do not clear a foreign route to make our source work. */
        const unsigned banks[]={0,4,8,16,20,24,28};
        for(unsigned ch=0;!e && ch<2;ch++)for(unsigned b=0;b<7;b++)
            if(rd(0x2f00+ch*32+banks[b])){e=EBUSY;break;}
        if(!e) {
            uint64_t base=hw.dma_bus,end=base+hw.dma_bytes-1;
            config[6].value=(uint32_t)(base>>32);config[7].value=(uint32_t)base;
            config[8].value=(uint32_t)(end>>32);config[9].value=(uint32_t)end;
            for(unsigned i=0;i<COUNT;i++)config[i].saved=config[i].expected=rd(config[i].off);
            state.dirty_mask=state.uncertain_mask=0;
            journaled=true;
            for(unsigned i=0;!e && i<COUNT;i++)e=change(i,config[i].value);
        }
        if(!e)e=verify_config(false);
        if(!e)e=unowned_routes_idle();
        if(!e){state.prepared=1;state.dl24=rd(0x46d0);state.out4=rd(0x1580);}
    }
    if(e){int off=stop_locked();if(off)state.restore_error=(uint32_t)off;}
    /* Admission errors returned above did not alter the prior stream.
     * Retained physical or transition ownership prevents a new prepare. */
    if(!leased || !state.running)state.error=(uint32_t)e;
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
static int copy_frames(const int16_t *pcm,uint32_t frames)
{
    if(!pcm || !frames || frames>hw.dma_bytes/4)return EINVAL;
    if(state.copy_pending || state.played>state.submitted || state.submitted>UINT64_MAX-frames)return EIO;
    if(state.submitted-state.played+frames>hw.dma_bytes/4-32)return EAGAIN;
    uint32_t offset=(uint32_t)(state.submitted%(hw.dma_bytes/4))*4;
    uint32_t bytes=frames*4,first=MIN(bytes,hw.dma_bytes-offset);
    state.copy_pending=frames; /* even sync failure may have exposed PCM */
    memcpy((uint8_t *)hw.dma+offset,pcm,first);
    int e=hw.sync_device(hw.context,offset,first);
    if(!e && first<bytes) {
        memcpy(hw.dma,(const uint8_t *)pcm+first,bytes-first);
        e=hw.sync_device(hw.context,0,bytes-first);
    }
    if(!e){state.submitted+=frames;state.copy_pending=0;}
    else {state.poisoned=1;if(e==EAGAIN)e=EIO;} /* no "consumed nothing" lie */
    return e;
}
int x9_speaker_afe_start(const int16_t *pcm,uint32_t frames,uint64_t *anchor)
{
    if(!anchor)return EINVAL;
    *anchor=0;
    if(!pcm || frames<960)return EINVAL;
    lck_mtx_lock(&speaker_afe_lock);int e=state.prepared&&!state.running&&!state.error?0:ENXIO;
    if(!e) {
        if(frames>hw.dma_bytes/4-32) {
            lck_mtx_unlock(&speaker_afe_lock);return EINVAL;
        }
        state.played=state.submitted=state.cursor=0;
        bzero(hw.dma,hw.dma_bytes);e=hw.sync_device(hw.context,0,hw.dma_bytes);
        if(!e)e=copy_frames(pcm,frames);
        if(!e)e=change(0,config[0].expected|1);
        if(!e) {
            state.anchor=mach_absolute_time();e=change(10,config[10].expected|(1u<<28));
            if(!e) {
                state.running=1;last_poll=last_progress=state.anchor;*anchor=state.anchor;
                thread_wakeup((event_t)&state.running);
            }
        }
        if(e){state.error=(uint32_t)e;state.poisoned=1;(void)stop_locked();}
    }
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
static int position_locked(void)
{
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
    /* Half-ring ambiguity must fail; never infer number of wraps by time. */
    if(elapsed>=(uint64_t)hw.dma_bytes/8*1000000000/48000 || stalled>=250000000)return ETIMEDOUT;
    uint32_t high=0,low=0;bool stable=false;
    for(unsigned n=0;n<3;n++) {
        high=rd(0x46b8);low=rd(0x46bc);if(high==rd(0x46b8)){stable=true;break;}
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
        int e=position_locked();if(e && e!=EAGAIN)fault_locked(e);
        lck_mtx_unlock(&speaker_afe_lock);
        delay_for_interval(WATCHDOG_MS,1000*NSEC_PER_USEC);
    }
}
int x9_speaker_afe_position(uint64_t *played)
{
    if(!played)return EINVAL;
    lck_mtx_lock(&speaker_afe_lock);int e=state.error?(int)state.error:state.running?position_locked():ENXIO;
    if(!e)*played=state.played;
    else if(e!=EAGAIN && state.running && !state.error)fault_locked(e);
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_submit(const int16_t *pcm,uint32_t frames)
{
    if(!pcm || !frames)return EINVAL;
    lck_mtx_lock(&speaker_afe_lock);int e=state.error?(int)state.error:state.running?position_locked():ENXIO;
    if(!e)e=copy_frames(pcm,frames);
    if(e && e!=EAGAIN && state.running && !state.error)fault_locked(e);
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_stop(void)
{
    lck_mtx_lock(&speaker_afe_lock);int e=stop_locked();
    if(e && !state.error)state.error=(uint32_t)e;
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_status(struct x9_speaker_afe_status *s)
{
    if(!s)return EINVAL;lck_mtx_lock(&speaker_afe_lock);*s=state;
    s->version=X9_SPEAKER_AFE_STATUS_VERSION;s->registered=registered;s->leased=leased;
    lck_mtx_unlock(&speaker_afe_lock);return 0;
}
