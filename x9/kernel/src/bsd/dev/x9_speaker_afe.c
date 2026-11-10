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
#include "x9_speaker_afe.h"
extern int sweet_x9_arg(const char *,int);
extern int sweet_early_is_x9(void);
LCK_GRP_DECLARE(speaker_afe_group,"x9-speaker-afe");
LCK_MTX_DECLARE(speaker_afe_lock,&speaker_afe_group);
static struct x9_speaker_afe_platform hw;
static struct x9_speaker_afe_status state;
static bool registered,leased,journaled;
static uint64_t last_poll,last_progress;
struct patch {uint32_t off,mask,value,saved,expected;};
/* OUT4 S16 in 32-bit I2S slots matches get_etdm_wlen(S16_LE)=31, valid
 * width=15. ETDM FS=5, relatch=10; DL24 FS=10. No IN4 feedback programming.
 * Pad/master/slave/format are AP-master I2S, synchronous sharing OFF.
 * Cowork OUT4->IN4 master choice=0 only configures the inactive selector. */
static struct patch config[]={
    {0x1580,1u|2u|8u|32u|(7u<<6)|(1u<<10)|(31u<<11)|(31u<<16)|(31u<<23)|(7u<<28),
            (15u<<11)|(31u<<16)|(1u<<23)|(1u<<28),0,0},
    {0x1584,31u|(31u<<5)|(1u<<15),5u|(6u<<5)|(1u<<15),0,0},
    {0x1588,31u,6u,0,0},
    {0x1590,31u|(7u<<6)|(31u<<24),5u|(1u<<6)|(10u<<24),0,0},
    {0x1690,15u<<8,0,0,0},
    {0x0098,(1u<<5)|(1u<<20),1u<<5,0,0},
    {0x46b0,UINT32_MAX,0,0,0},{0x46b4,UINT32_MAX,0,0,0},
    {0x46c0,UINT32_MAX,0,0,0},{0x46c4,UINT32_MAX,0,0,0},
    {0x46d0,(1u<<28)|(3u<<20)|(3u<<16)|(31u<<8)|(3u<<5)|(1u<<4)|(1u<<3)|(1u<<2)|3u,
        (10u<<8)|(1u<<3),0,0}, /* stereo S16, normal SRAM, pbuf32/min0 (10ms) */
    {0x2f08,UINT32_MAX,1u<<16,0,0}, /* I_DL24_CH1 = 80-64 */
    {0x2f28,UINT32_MAX,1u<<17,0,0}, /* I_DL24_CH2 = 81-64 */
};
#define COUNT ((unsigned)(sizeof(config)/sizeof(config[0])))
static uint32_t rd(uint32_t off)
{uint32_t v=hw.afe[off/4];__asm__ volatile("dmb oshld":::"memory");return v;}
static void wr(uint32_t off,uint32_t value)
{hw.afe[off/4]=value;__asm__ volatile("dsb sy":::"memory");}
static int change(unsigned i,uint32_t value)
{
    struct patch *p=&config[i];uint32_t old=rd(p->off);
    if((old&p->mask)!=(p->expected&p->mask))return EBUSY;
    p->expected=(old&~p->mask)|(value&p->mask);wr(p->off,p->expected);
    return (rd(p->off)&p->mask)==(p->expected&p->mask)?0:EIO;
}
static int restore(void)
{
    /* Ordinary masked RW only; CUR and MEM_UP triggers are NEVER replayed. */
    for(unsigned n=COUNT;n>0;n--) {
        int e=change(n-1,config[n-1].saved);if(e)return e;
    }
    journaled=false;return 0;
}
static int disable(void)
{
    wr(0x46d0,rd(0x46d0)&~(1u<<28));config[10].expected&=~(1u<<28);
    uint64_t ticks;nanoseconds_to_absolutetime(1000000,&ticks);
    uint64_t end=mach_absolute_time()+ticks;
    for(unsigned n=0;n<500;n++) {
        if(!(rd(0x46d0)&(1u<<28)))break;
        if(mach_absolute_time()>=end)return ETIMEDOUT;delay(2);
    }
    if(rd(0x46d0)&(1u<<28))return ETIMEDOUT;
    wr(0x1580,rd(0x1580)&~1u);config[0].expected&=~1u;
    if(rd(0x1580)&1)return EIO;
    state.running=0;return 0;
}
static int stop_locked(void)
{
    if(!leased)return 0;
    /* Still stop DMA if mute fails. A failed mute never authorizes releasing
     * clocks/I2C/rails or declaring the PA safely off. */
    int muted=hw.mute(hw.context);
    int e=journaled?disable():0;
    if(!e && journaled)e=hw.quiesce(hw.context);
    if(!e && muted)e=muted;
    if(!e && journaled)e=restore();
    if(!e)e=hw.release(hw.context);
    if(!e){leased=false;state.prepared=0;}
    else state.restore_error=(uint32_t)e;
    return e;
}
int x9_speaker_afe_register(const struct x9_speaker_afe_platform *p)
{
    if(!sweet_early_is_x9() || sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    if(!p || p->version!=1 || p->verified!=X9_SPEAKER_AFE_RESOURCES || !p->afe ||
       !p->dma || !p->acquire || !p->release || !p->mute || !p->quiesce || !p->sync_device ||
       ((uintptr_t)p->afe&3) || ((uintptr_t)p->dma&63) || (p->dma_bus&63) ||
       !p->dma_bus || p->dma_bytes<65536 || p->dma_bytes>131072 || (p->dma_bytes&63) ||
       p->dma_bus>(1ULL<<41)-p->dma_bytes)return EINVAL;
    lck_mtx_lock(&speaker_afe_lock);int e=registered?EBUSY:0;
    if(!e){hw=*p;registered=true;state.version=1;}
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_prepare(void)
{
    lck_mtx_lock(&speaker_afe_lock);int e=registered?0:ENXIO;
    if(!e && (leased || state.error))e=EBUSY;
    if(!e)e=hw.acquire(hw.context);
    if(!e) {
        leased=true;state.restore_error=0;e=hw.mute(hw.context);
        if(!e && ((rd(0x46d0)&(1u<<28)) || (rd(0x1580)&1)))e=EBUSY;
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
            journaled=true;
            for(unsigned i=0;!e && i<COUNT;i++)e=change(i,config[i].value);
        }
        if(!e){state.prepared=1;state.dl24=rd(0x46d0);state.out4=rd(0x1580);}
        else {int off=stop_locked();if(off)state.restore_error=(uint32_t)off;}
    }
    state.error=(uint32_t)e;lck_mtx_unlock(&speaker_afe_lock);return e;
}
static int copy_frames(const int16_t *pcm,uint32_t frames)
{
    if(!pcm || !frames || frames>hw.dma_bytes/4)return EINVAL;
    if(state.submitted-state.played+frames>hw.dma_bytes/4-32)return EAGAIN;
    uint32_t offset=(uint32_t)(state.submitted%(hw.dma_bytes/4))*4;
    uint32_t bytes=frames*4,first=MIN(bytes,hw.dma_bytes-offset);
    memcpy((uint8_t *)hw.dma+offset,pcm,first);
    int e=hw.sync_device(hw.context,offset,first);
    if(!e && first<bytes) {
        memcpy(hw.dma,(const uint8_t *)pcm+first,bytes-first);
        e=hw.sync_device(hw.context,0,bytes-first);
    }
    if(!e)state.submitted+=frames;return e;
}
int x9_speaker_afe_start(const int16_t *pcm,uint32_t frames,uint64_t *anchor)
{
    if(!anchor || frames<960)return EINVAL;
    lck_mtx_lock(&speaker_afe_lock);int e=state.prepared&&!state.running&&!state.error?0:ENXIO;
    if(!e) {
        state.played=state.submitted=state.cursor=0;
        bzero(hw.dma,hw.dma_bytes);e=hw.sync_device(hw.context,0,hw.dma_bytes);
        if(!e)e=copy_frames(pcm,frames);
        if(!e)e=change(0,config[0].expected|1);
        if(!e) {
            state.anchor=mach_absolute_time();e=change(10,config[10].expected|(1u<<28));
            if(!e){state.running=1;last_poll=last_progress=state.anchor;*anchor=state.anchor;}
        }
        if(e){state.error=(uint32_t)e;int off=stop_locked();if(off)state.restore_error=(uint32_t)off;}
    }
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
static int position_locked(void)
{
    uint64_t now=mach_absolute_time(),elapsed,stalled;
    absolutetime_to_nanoseconds(now-last_poll,&elapsed);
    absolutetime_to_nanoseconds(now-last_progress,&stalled);
    /* Half-ring ambiguity must fail; never infer number of wraps by time. */
    if(elapsed>=(uint64_t)hw.dma_bytes/8*1000000000/48000 || stalled>=250000000)return ETIMEDOUT;
    uint32_t high=0,low=0;bool stable=false;
    for(unsigned n=0;n<3;n++) {
        high=rd(0x46b8);low=rd(0x46bc);if(high==rd(0x46b8)){stable=true;break;}
    }
    if(!stable)return EAGAIN;
    uint64_t ptr=((uint64_t)high<<32)|low;
    if(!ptr && stalled<100000000)return EAGAIN;
    if(ptr<hw.dma_bus || ptr>=hw.dma_bus+hw.dma_bytes || (ptr&3))return EIO;
    uint32_t next=(uint32_t)(ptr-hw.dma_bus);
    uint32_t bytes=(next+hw.dma_bytes-state.cursor)%hw.dma_bytes;
    if(bytes>hw.dma_bytes/2 || state.played+bytes/4>state.submitted)return EIO;
    if(bytes){state.played+=bytes/4;state.cursor=next;last_progress=now;}
    last_poll=now;return 0;
}
int x9_speaker_afe_position(uint64_t *played)
{
    if(!played)return EINVAL;
    lck_mtx_lock(&speaker_afe_lock);int e=state.running?position_locked():ENXIO;
    if(!e)*played=state.played;
    else if(e!=EAGAIN && state.running){state.error=(uint32_t)e;int off=stop_locked();if(off)e=off;}
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_submit(const int16_t *pcm,uint32_t frames)
{
    lck_mtx_lock(&speaker_afe_lock);int e=state.running?position_locked():ENXIO;
    if(!e)e=copy_frames(pcm,frames);
    if(e && e!=EAGAIN && state.running){state.error=(uint32_t)e;int off=stop_locked();if(off)e=off;}
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_stop(void)
{
    lck_mtx_lock(&speaker_afe_lock);int e=stop_locked();
    if(e)state.error=(uint32_t)e;
    lck_mtx_unlock(&speaker_afe_lock);return e;
}
int x9_speaker_afe_status(struct x9_speaker_afe_status *s)
{
    if(!s)return EINVAL;lck_mtx_lock(&speaker_afe_lock);*s=state;s->version=1;
    lck_mtx_unlock(&speaker_afe_lock);return 0;
}
