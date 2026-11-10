/* Independent XNU VUL1 engine, register layout/rate/routing checked against
 * OPPO kernel_device_modules-6.12/sound/soc/mediatek/mt6993:
 * mt6993-afe-pcm.c memif_data[VUL1], memif_ul1_ch{1,2}_mix;
 * mt6993-reg.h; common/mtk-base-afe.h IPM2P0_RATE_48K=0xa.
 * The MT6681/pads/clocks/MTKAIFv4 path is NOT replaced with stub success. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include "x9_mic_mt6993.h"

enum {VUL1_BASE_MSB=0x4d90,VUL1_BASE=0x4d94,VUL1_CUR_MSB=0x4d98,
    VUL1_CUR=0x4d9c,VUL1_END_MSB=0x4da0,VUL1_END=0x4da4,VUL1_CON0=0x4db0,
    CONN020=0x2300,CONN021=0x2320};
#define VUL1_ON (1u<<28)
LCK_GRP_DECLARE(afe_mic_group,"x9-mic-afe");
LCK_MTX_DECLARE(afe_mic_lock,&afe_mic_group);
static struct x9_mic_mt6993_platform hw;
static bool registered, worker_created, active, prepared, saved, dma_stuck;
static bool codec_prepared, capture_prepared;
static struct x9_mic_mt6681 codec;
static struct x9_mic_hardware_status diagnostic={.version=1,
    .chosen_phase={-1,-1},.cycle={-1,-1}};
static uint64_t generation,last_poll,last_progress;
static uint32_t cursor,old_regs[9];
static const uint32_t save_regs[]={VUL1_CON0,VUL1_BASE_MSB,VUL1_BASE,
    VUL1_END_MSB,VUL1_END,CONN020,CONN021,CONN020+4,CONN021+4};
static uint32_t rd(uint32_t reg)
{
    uint32_t value=hw.afe[reg/4];__asm__ volatile("dmb oshld":::"memory");return value;
}
static void wr(uint32_t reg,uint32_t value)
{
    hw.afe[reg/4]=value;__asm__ volatile("dsb oshst":::"memory");
}
static int stop_locked(void)
{
    active=false;
    if(saved) {
        wr(VUL1_CON0,rd(VUL1_CON0)&~VUL1_ON);
        /* Never restore DMA addresses or release memory while ON is stuck. */
        if(rd(VUL1_CON0)&VUL1_ON){dma_stuck=true;
            diagnostic.restore_errno=EIO;diagnostic.flags|=X9_MIC_HW_RESTORE_FAILED;return EIO;}
        delay(125); /* OPPO ADDA capture-off minimum one 8k sample */
        for(unsigned i=1;i<sizeof(save_regs)/sizeof(save_regs[0]);i++)wr(save_regs[i],old_regs[i]);
        wr(VUL1_CON0,old_regs[0]);saved=false;
    }
    int e=capture_prepared?hw.capture_unprepare():0;
    if(e)goto failed;
    capture_prepared=false;diagnostic.flags&=~X9_MIC_HW_CAPTURE;
    if(codec_prepared) {
        e=x9_mic_mt6681_unprepare(&codec);
        if(e)goto failed;
        codec_prepared=false;diagnostic.flags&=~X9_MIC_HW_POWERED;
    }
    e=prepared?hw.unprepare():0;
    if(!e)prepared=false;
    if(!e){if(!diagnostic.last_errno)diagnostic.stage=X9_MIC_HW_STOPPED;return 0;}
failed:
    if(!diagnostic.restore_errno)diagnostic.restore_errno=e;
    diagnostic.flags|=X9_MIC_HW_RESTORE_FAILED;
    return e;
}
static int afe_stop(void)
{
    lck_mtx_lock(&afe_mic_lock);int e=stop_locked();lck_mtx_unlock(&afe_mic_lock);return e;
}
static int afe_start(uint64_t g)
{
    lck_mtx_lock(&afe_mic_lock);int e=0;
    if(active || prepared || dma_stuck)e=EBUSY;
    else {
        bzero(&diagnostic,sizeof(diagnostic));diagnostic.version=1;
        diagnostic.stage=X9_MIC_HW_RESOURCES;
        diagnostic.chosen_phase[0]=diagnostic.chosen_phase[1]=-1;
        diagnostic.cycle[0]=diagnostic.cycle[1]=-1;
        prepared=true;e=hw.prepare();
        if(!e && (rd(VUL1_CON0)&VUL1_ON))e=EBUSY;
        if(!e) {
            codec_prepared=true;
            e=x9_mic_mt6681_power_calibrate(&codec,&hw.codec,hw.afe);
            diagnostic=codec.status;
        }
        if(!e) {
            diagnostic.stage=X9_MIC_HW_ADC;capture_prepared=true;
            e=hw.capture_prepare(&diagnostic);
            if(!e)e=x9_mic_mt6681_verify_capture(&codec);
            if(e)diagnostic.last_register=codec.status.last_register;
            if(!e && (rd(VUL1_CON0)&VUL1_ON))e=EBUSY;
        }
        if(!e) {
            diagnostic.stage=X9_MIC_HW_DMA;
            for(unsigned i=0;i<sizeof(save_regs)/sizeof(save_regs[0]);i++)old_regs[i]=rd(save_regs[i]);
            saved=true;
            /* The board owns ALL routing words for these outputs, not just
             * bit16/17. Refuse an inherited/mixed source rather than record
             * DL/PCM/sine. Other words (2,4..7) must also be empty. */
            for(unsigned c=0;c<2 && !e;c++)for(unsigned w=0;w<8;w++) {
                if(w==3)continue;
                if(rd((c?CONN021:CONN020)+w*4))e=EBUSY;
            }
        }
        if(!e) {
            uint64_t base=hw.dma_bus_address,end=base+hw.dma_bytes-1;
            hw.sync_for_device();
            wr(VUL1_BASE_MSB,(uint32_t)(base>>32));wr(VUL1_BASE,(uint32_t)base);
            wr(VUL1_END_MSB,(uint32_t)(end>>32));wr(VUL1_END,(uint32_t)end);
            /* stereo, 16-bit packed, normal mode; select fs=0xa (48k).
             * Burst/domain fields are board-qualified values from prepare. */
            uint32_t mask=(0x1fu<<8)|(1u<<6)|(1u<<5)|(1u<<4)|(1u<<3)|(1u<<2)|3u;
            uint32_t con=(old_regs[0]&~mask)|(0xau<<8)|(1u<<3);
            wr(CONN020,1u<<16);wr(CONN021,1u<<17);
            wr(VUL1_CON0,con|VUL1_ON);
            /* Address registers expose bits31:4; low nibble is reserved. */
            if(!(rd(VUL1_CON0)&VUL1_ON) || (rd(VUL1_BASE)&~15u)!=((uint32_t)base&~15u) ||
               (rd(VUL1_END)&~15u)!=((uint32_t)end&~15u) ||
               (rd(VUL1_BASE_MSB)&0x1ffu)!=(uint32_t)(base>>32) ||
               (rd(VUL1_END_MSB)&0x1ffu)!=(uint32_t)(end>>32))e=EIO;
            else {generation=g;cursor=0;last_progress=last_poll=mach_absolute_time();active=true;
                diagnostic.flags|=X9_MIC_HW_CAPTURE;}
        }
        if(e) {diagnostic.last_errno=e;int off=stop_locked();if(off)e=off;}
    }
    lck_mtx_unlock(&afe_mic_lock);return e;
}
static int poll_locked(void)
{
    uint32_t hi=0,lo=0;bool stable=false;
    for(unsigned i=0;i<3;i++) {
        hi=rd(VUL1_CUR_MSB);lo=rd(VUL1_CUR);
        if(hi==rd(VUL1_CUR_MSB)){stable=true;break;}
    }
    if(!stable)return EAGAIN;
    uint64_t ptr=((uint64_t)hi<<32)|lo,base=hw.dma_bus_address;
    uint64_t now=mach_absolute_time(),elapsed;
    absolutetime_to_nanoseconds(now-last_poll,&elapsed);
    uint64_t stalled;absolutetime_to_nanoseconds(now-last_progress,&stalled);
    if(stalled>=250000000)return ETIMEDOUT;
    if(!ptr && elapsed<100000000)return 0; /* bounded initial DMA wait */
    if(ptr<base || ptr>=base+hw.dma_bytes || (ptr&3))return EIO;
    uint32_t next=(uint32_t)(ptr-base);
    /* A pointer alone cannot distinguish a full wrap. If the worker was
     * delayed >=100ms (ring >=341ms), discard ambiguous pending bytes instead
     * of delivering stale or overwritten PCM as a successful recording. */
    if(elapsed>=100000000) {
        uint64_t frames=elapsed*X9_MIC_RATE/1000000000;
        x9_mic_backend_drop(generation,(uint32_t)MIN(frames,UINT32_MAX));
        if(next!=cursor)last_progress=now;
        cursor=next;last_poll=now;return 0;
    }
    uint32_t bytes=(next+hw.dma_bytes-cursor)%hw.dma_bytes;
    if(!bytes)return 0;
    last_progress=now;
    if(bytes>hw.dma_bytes/2)return EIO;
    int16_t mono[256];
    while(bytes) {
        uint32_t n=MIN(MIN(bytes,hw.dma_bytes-cursor)/4,256u);
        hw.sync_for_cpu(cursor,n*4);
        const int16_t *pcm=(const int16_t *)((const uint8_t *)hw.dma+cursor);
        for(uint32_t i=0;i<n;i++)mono[i]=pcm[i*2+hw.adc_channel];
        /* Preserve increasing last-sample time across chunks of one poll. */
        uint64_t behind,t=now;
        nanoseconds_to_absolutetime((uint64_t)(bytes-n*4)/4*1000000000/X9_MIC_RATE,&behind);
        if(t>behind)t-=behind;
        int e=x9_mic_deliver(generation,mono,n,t);
        bzero(mono,sizeof(mono));
        if(e && e!=ESTALE)return e;
        cursor=(cursor+n*4)%hw.dma_bytes;bytes-=n*4;
    }
    last_poll=now;return 0;
}
static void afe_worker(__unused void *arg,__unused wait_result_t result)
{
    static int cookie;
    for(;;) {
        lck_mtx_lock(&afe_mic_lock);
        if(active) {
            int e=poll_locked();
            if(e && e!=EAGAIN) {
                diagnostic.last_errno=e;
                uint64_t g=generation;int off=stop_locked();
                x9_mic_backend_fault(g,off?off:e);
            }
        }
        lck_mtx_unlock(&afe_mic_lock);
        uint64_t ticks;nanoseconds_to_absolutetime(5000000,&ticks);
        (void)assert_wait_deadline(&cookie,THREAD_UNINT,mach_absolute_time()+ticks);
        thread_block(THREAD_CONTINUE_NULL);
    }
}
int x9_mic_mt6993_register(const struct x9_mic_mt6993_platform *p)
{
    if(!p || p->version!=2 || p->adc_channel>1 || !p->afe || !p->dma ||
       !p->prepare || !p->unprepare || !p->sync_for_cpu || !p->sync_for_device ||
       !p->capture_prepare || !p->capture_unprepare || p->codec.version!=1 ||
       p->codec.flags!=X9_MT6681_REQUIRED || !p->codec.read8 || !p->codec.write8 ||
       ((uintptr_t)p->afe&3) || ((uintptr_t)p->dma&15) || (p->dma_bus_address&15) ||
       !p->dma_bus_address || p->dma_bytes<65536 || p->dma_bytes>131072 || (p->dma_bytes&15) ||
       p->dma_bus_address>((1ULL<<41)-p->dma_bytes))return EINVAL;
    lck_mtx_lock(&afe_mic_lock);
    if(registered){lck_mtx_unlock(&afe_mic_lock);return EBUSY;}
    hw=*p;
    int e=0;
    if(!worker_created) {
        thread_t t;
        if(kernel_thread_start(afe_worker,NULL,&t)!=KERN_SUCCESS) {
            e=ENOMEM;
        } else {thread_deallocate(t);worker_created=true;}
    }
    const struct x9_mic_backend backend={1,afe_start,afe_stop};
    if(!e)e=x9_mic_register_backend(&backend);
    if(!e)registered=true;
    lck_mtx_unlock(&afe_mic_lock);return e;
}
void x9_mic_mt6993_get_hardware(struct x9_mic_hardware_status *s)
{
    lck_mtx_lock(&afe_mic_lock);*s=diagnostic;lck_mtx_unlock(&afe_mic_lock);
}
