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
#include "x9_mic_plj110.h"
#include "x9_audio_resources.h"

#include "x9_mic_mt6993_regs.h"
_Static_assert(sizeof(struct x9_mic_dma_status)==192,"DMA ABI size");
LCK_GRP_DECLARE(afe_mic_group,"x9-mic-afe");
LCK_MTX_DECLARE(afe_mic_lock,&afe_mic_group);
static struct x9_mic_mt6993_platform hw;
static bool registered, worker_created, armed, active, prepared, saved, dma_stuck;
static struct x9_mic_hardware_status diagnostic={.version=1,
    .chosen_phase={-1,-1},.cycle={-1,-1}};
static struct x9_mic_board_status board_diagnostic={.version=1};
static struct x9_mic_afe_status afe_diagnostic={.version=1};
static struct x9_mic_analog_status analog_diagnostic={.version=1};
static struct x9_mic_digital_status digital_diagnostic={.version=1};
static uint64_t generation,last_poll,last_progress;
static uint64_t resource_ticket;
static struct x9_mic_dma_status dma_diagnostic={.version=1,
    .blockers=X9_MIC_DMA_NEED_MAP|X9_MIC_DMA_NEED_OWNER|X9_MIC_DMA_NEED_COPY|
        X9_MIC_DMA_NEED_QUIESCE|X9_MIC_DMA_NEED_SOURCE_ID};
static bool dma_issued;
static uint32_t cursor,old_regs[7],expected_regs[7],written;
static const uint32_t reg_masks[]={VUL1_CAPTURE_MASK,0x1ff,0xfffffff0,
    0x1ff,0xfffffff0,UINT32_MAX,UINT32_MAX};
static const uint32_t save_regs[]={VUL1_CON0,VUL1_BASE_MSB,VUL1_BASE,
    VUL1_END_MSB,VUL1_END,CONN020,CONN021};
static int capture_access(void)
{
    if(hw.prepare!=x9_mic_plj110_capture_prepare)return 0;
    int e=x9_mic_plj110_capture_access();
    if(e && (afe_diagnostic.last_register&X9_MIC_AFE_REGISTER_SPM))
        dma_diagnostic.last_register=afe_diagnostic.last_register;
    return e;
}
static uint32_t rd(uint32_t reg)
{
    uint32_t value=hw.afe[reg/4];__asm__ volatile("dmb oshld":::"memory");return value;
}
static void wr(uint32_t reg,uint32_t value)
{
    hw.afe[reg/4]=value;__asm__ volatile("dsb oshst":::"memory");
}
static int program_reg(unsigned i,uint32_t value)
{
    int e=capture_access();if(e)return e;
    uint32_t mask=reg_masks[i];
    dma_diagnostic.last_register=save_regs[i];
    uint32_t old=rd(save_regs[i]);
    if((rd(VUL1_CON0)&(VUL1_ON|(1u<<7))) || (old&mask)!=(old_regs[i]&mask))return EBUSY;
    expected_regs[i]=value&mask;written|=1u<<i; /* before possibly effective write */
    dma_diagnostic.pending_flags=written;
    ++dma_diagnostic.journal_count;dma_diagnostic.last_register=save_regs[i];
    wr(save_regs[i],(old&~mask)|expected_regs[i]);
    return (rd(save_regs[i])&mask)==expected_regs[i]?0:EIO;
}
static int stop_locked(void)
{
    armed=active=false;
    /* Uncertain DMA ownership/drain is terminal for this boot. Do not turn
     * a second StopIO into an ON=0 success or free the retained buffer. */
    if(dma_stuck)return dma_diagnostic.restore_errno?dma_diagnostic.restore_errno:EIO;
    int e=0;
    if(saved) {
        e=capture_access();if(e)goto dma_failed;
        if(dma_issued) {
            dma_diagnostic.stage=X9_MIC_DMA_DISABLE;
            dma_diagnostic.last_register=VUL1_CON0;
            uint32_t con=rd(VUL1_CON0);
            if(con&(1u<<7)){e=EBUSY;goto dma_failed;}
            wr(VUL1_CON0,con&~VUL1_ON);
            dma_diagnostic.con0_after=rd(VUL1_CON0);
            dma_diagnostic.monitor_after=rd(VUL1_MON0);
            if(dma_diagnostic.con0_after&VUL1_ON){e=EIO;goto dma_failed;}
            dma_diagnostic.stage=X9_MIC_DMA_DRAIN;
            /* Local pending/WEN/sync and stable CUR are diagnostic only.
             * Only the board's complete write-path provider authorizes
             * restoring addresses/routing and releasing clocks/memory. */
            e=hw.dma_quiesce();if(e)goto dma_failed;
            e=capture_access();if(e)goto dma_failed;
            dma_diagnostic.monitor_after=rd(VUL1_MON0);
            dma_diagnostic.last_register=VUL1_MON0;
            if(dma_diagnostic.monitor_after&VUL1_MON_BUSY){e=EIO;goto dma_failed;}
            dma_diagnostic.flags|=X9_MIC_DMA_DRAINED;dma_issued=false;
        }
        dma_diagnostic.stage=X9_MIC_DMA_RESTORE;
        static const unsigned reverse_write_order[]={6,5,0,4,3,2,1};
        for(unsigned i=0;i<7;i++) {
            unsigned k=reverse_write_order[i];if(!(written&(1u<<k)))continue;
            e=capture_access();if(e)goto dma_failed;
            dma_diagnostic.last_register=save_regs[k];
            if(rd(VUL1_CON0)&(VUL1_ON|(1u<<7))){e=EBUSY;goto dma_failed;}
            uint32_t v=rd(save_regs[k]),mask=reg_masks[k];
            if((v&mask)!=expected_regs[k] && (v&mask)!=(old_regs[k]&mask)) {
                e=EBUSY;goto dma_failed; /* never overwrite a late foreign owner */
            }
            wr(save_regs[k],(v&~mask)|(old_regs[k]&mask));
            if((rd(save_regs[k])&mask)!=(old_regs[k]&mask)){e=EIO;goto dma_failed;}
            written&=~(1u<<k);dma_diagnostic.pending_flags=written;
            ++dma_diagnostic.journal_restored;
        }
        saved=false;dma_diagnostic.con0_after=rd(VUL1_CON0);
    }
    e=prepared?hw.unprepare():0;
    if(!e){prepared=false;diagnostic.flags&=~X9_MIC_HW_CAPTURE;}
    /* Do not leave userspace READY after a nominally successful STOP unless
     * the actual full-stop receipt is eligible for another cold START. Failed
     * prepare remains terminal without recycling its transaction evidence. */
    if(!e && hw.prepare==x9_mic_plj110_capture_prepare &&
       !diagnostic.last_errno && (dma_diagnostic.flags&X9_MIC_DMA_STARTED))
        e=x9_mic_plj110_capture_restart_ready();
    if(!e && resource_ticket) {
        e=x9_audio_resources_release(X9AudioResourceMic,resource_ticket);
        if(!e)resource_ticket=0;
    }
    if(!e){
        dma_diagnostic.flags|=X9_MIC_DMA_RESTORED;dma_diagnostic.stage=X9_MIC_DMA_OFF;
        if(!diagnostic.last_errno)diagnostic.stage=X9_MIC_HW_STOPPED;
        return 0;
    }
    goto failed;
dma_failed:
    dma_stuck=true;
failed:
    if(resource_ticket)x9_audio_resources_fault(X9AudioResourceMic,resource_ticket,e);
    if(hw.prepare==x9_mic_plj110_capture_prepare)x9_mic_plj110_capture_retain(e);
    dma_diagnostic.restore_register=dma_diagnostic.last_register;
    if(!dma_diagnostic.restore_errno)dma_diagnostic.restore_errno=e;
    dma_diagnostic.flags|=X9_MIC_DMA_RETAINED;
    if(!diagnostic.restore_errno)diagnostic.restore_errno=e;
    diagnostic.flags|=X9_MIC_HW_RESTORE_FAILED;
    /* DMA failure skips board teardown entirely. Make retained dependencies
     * visible on every cache surface without off-domain/extra MMIO. */
    if(prepared && (board_diagnostic.flags&X9_MIC_BOARD_ATTEMPTED)) {
        board_diagnostic.flags|=X9_MIC_BOARD_RETAINED;
        if(!board_diagnostic.restore_errno)board_diagnostic.restore_errno=e;
        if(afe_diagnostic.flags&X9_MIC_AFE_ACQUIRED) {
            afe_diagnostic.flags|=X9_MIC_AFE_RETAINED;
            if(!afe_diagnostic.restore_errno)afe_diagnostic.restore_errno=e;
        }
        if(analog_diagnostic.flags&X9_MIC_ANALOG_PREPARED) {
            analog_diagnostic.flags|=X9_MIC_ANALOG_RETAINED;
            if(!analog_diagnostic.restore_errno)analog_diagnostic.restore_errno=e;
        }
        if(digital_diagnostic.flags&X9_MIC_DIGITAL_ATTEMPTED) {
            digital_diagnostic.flags|=X9_MIC_DIGITAL_RETAINED;
            if(!digital_diagnostic.restore_errno)digital_diagnostic.restore_errno=e;
        }
    }
    return e;
}
static int afe_stop(void)
{
    lck_mtx_lock(&afe_mic_lock);int e=stop_locked();lck_mtx_unlock(&afe_mic_lock);return e;
}
static int afe_start(uint64_t g)
{
    lck_mtx_lock(&afe_mic_lock);int e=0;
    if(armed || active || prepared || dma_stuck || resource_ticket || (diagnostic.flags&X9_MIC_HW_RESTORE_FAILED))e=EBUSY;
    else {
        /* Previous published DMA/codec status belongs to the completed
         * transaction. Recycle it BEFORE bzero or marking prepared, so a
         * rejected restart cannot erase evidence or tear down an old lease. */
        if(hw.prepare==x9_mic_plj110_capture_prepare &&
           (board_diagnostic.flags&X9_MIC_BOARD_ATTEMPTED)) {
            e=hw.rearm();
            if(e){lck_mtx_unlock(&afe_mic_lock);return e;}
        }
        /* PLJ110 claims once inside its board pipeline and keeps that lease
         * across VUL1 polling/drain. A nested engine claim is nonrecursive
         * and would always fail. Other providers use the engine's lease. */
        if(hw.prepare!=x9_mic_plj110_capture_prepare) {
            e=x9_audio_resources_claim(X9AudioResourceMic,&resource_ticket);
            if(e){lck_mtx_unlock(&afe_mic_lock);return e;}
        }
        bzero(&dma_diagnostic,sizeof(dma_diagnostic));dma_diagnostic.version=1;
        dma_diagnostic.flags=X9_MIC_DMA_ATTEMPTED|X9_MIC_DMA_BOUND;
        dma_diagnostic.adc_channel=hw.adc_channel;dma_diagnostic.ring_bytes=hw.dma_bytes;
        dma_diagnostic.base=hw.dma_bus_address;dma_diagnostic.end=hw.dma_bus_address+hw.dma_bytes-1;
        bzero(&diagnostic,sizeof(diagnostic));diagnostic.version=1;
        diagnostic.stage=X9_MIC_HW_RESOURCES;
        diagnostic.chosen_phase[0]=diagnostic.chosen_phase[1]=-1;
        diagnostic.cycle[0]=diagnostic.cycle[1]=-1;
        prepared=true;
        e=hw.prepare(hw.afe,&diagnostic,&board_diagnostic,&afe_diagnostic,
            &analog_diagnostic,&digital_diagnostic,&dma_diagnostic,hw.dma_memory_type);
        /* A board may reuse read-only eligibility snapshots. Binding is a
         * separate verified contract, never inferred from those flags. */
        dma_diagnostic.flags|=X9_MIC_DMA_BOUND;dma_diagnostic.blockers=0;
        dma_diagnostic.adc_channel=hw.adc_channel;dma_diagnostic.ring_bytes=hw.dma_bytes;
        dma_diagnostic.base=hw.dma_bus_address;dma_diagnostic.end=hw.dma_bus_address+hw.dma_bytes-1;
        if(!e)e=capture_access();
        if(!e) {
            diagnostic.stage=X9_MIC_HW_DMA;
            dma_diagnostic.stage=X9_MIC_DMA_BASELINE;
            dma_diagnostic.con0_before=rd(VUL1_CON0);
            dma_diagnostic.monitor_before=rd(VUL1_MON0);
            dma_diagnostic.memif_con0=rd(MEMIF_CON0);
            dma_diagnostic.flags|=X9_MIC_DMA_REGISTERS_VALID;
            dma_diagnostic.active_memifs=0;
            for(unsigned i=0;i<32;i++)if(rd(x9_mic_memif_regs[i])&VUL1_ON)
                dma_diagnostic.active_memifs|=1u<<i;
            if(dma_diagnostic.active_memifs)e=EBUSY;
            if(dma_diagnostic.memif_con0&4){dma_diagnostic.last_register=MEMIF_CON0;e=EBUSY;}
            if(dma_diagnostic.con0_before&(VUL1_ON|(1u<<7)))e=EBUSY;
            if(dma_diagnostic.monitor_before&(VUL1_MON_BUSY|VUL1_MON_FULL))e=EBUSY;
            /* Every actual routing word must be empty; word3 is reserved.
             * Reject before saving/writing: failure must not restore foreign
             * routes or untouched address registers. */
            for(unsigned c=0;c<2 && !e;c++)for(unsigned w=0;w<7;w++) {
                uint32_t value=rd((c?CONN021:CONN020)+x9_mic_route_words[w]*4);
                dma_diagnostic.route[c][w]=value;
                if(value){dma_diagnostic.last_register=(c?CONN021:CONN020)+x9_mic_route_words[w]*4;e=EBUSY;}
            }
            if(!e)dma_diagnostic.flags|=X9_MIC_DMA_IDLE|X9_MIC_DMA_ROUTE_EMPTY;
        }
        if(!e) {
            for(unsigned i=0;i<7;i++)old_regs[i]=rd(save_regs[i]);
            written=0;saved=true;
            dma_diagnostic.stage=X9_MIC_DMA_CONFIG;
            uint64_t base=hw.dma_bus_address,end=base+hw.dma_bytes-1;
            e=hw.sync_for_device();
            if(!e)e=program_reg(1,(uint32_t)(base>>32));
            if(!e)e=program_reg(2,(uint32_t)base);
            if(!e)e=program_reg(3,(uint32_t)(end>>32));
            if(!e)e=program_reg(4,(uint32_t)end);
            /* stereo, signed packed S16, normal mode, fs=0xa (48k),
             * vendor VUL1 min/max=64 bytes. Journal also restores burst
             * fields; do not inherit unqualified bootloader settings. */
            if(!e)e=program_reg(0,VUL1_CAPTURE_CONFIG);
            if(!e)e=program_reg(5,1u<<16);
            if(!e)e=program_reg(6,1u<<17);
            if(!e)e=capture_access();
            if(e)goto start_failed;
            dma_diagnostic.stage=X9_MIC_DMA_ENABLE;dma_diagnostic.last_register=VUL1_CON0;
            dma_issued=true; /* before ON write, even a failed readback can DMA */
            dma_diagnostic.flags|=X9_MIC_DMA_STARTED;
            wr(VUL1_CON0,rd(VUL1_CON0)|VUL1_ON);
            if(!(rd(VUL1_CON0)&VUL1_ON))e=EIO;
            else {generation=g;cursor=0;last_progress=last_poll=mach_absolute_time();armed=true;
                diagnostic.flags|=X9_MIC_HW_CAPTURE;}
        }
start_failed:
        if(e) {diagnostic.last_errno=e;dma_diagnostic.last_errno=e;
            uint32_t reg=dma_diagnostic.last_register;
            int off=stop_locked();dma_diagnostic.last_register=reg;if(off)e=off;}
    }
    lck_mtx_unlock(&afe_mic_lock);return e;
}
static int afe_commit(uint64_t g)
{
    lck_mtx_lock(&afe_mic_lock);
    int e=0;
    if(!armed || active || generation!=g)e=ESTALE;
    else if(!prepared || !saved || !dma_issued || dma_stuck ||
            diagnostic.last_errno || diagnostic.restore_errno)e=EIO;
    else {
        /* Keep ON-time timestamps/cursor: a delayed commit must not hide a
         * ring wrap or stall by resetting the progress clock. No MMIO/data
         * consumption until the worker observes this committed state. */
        armed=false;active=true;dma_diagnostic.stage=X9_MIC_DMA_POLL;
    }
    lck_mtx_unlock(&afe_mic_lock);return e;
}
static int poll_locked(void)
{
    int access=capture_access();if(access)return access;
    dma_diagnostic.last_register=VUL1_MON0;
    if(!(rd(VUL1_CON0)&VUL1_ON) || (rd(VUL1_MON0)&VUL1_MON_FULL))return EIO;
    uint32_t hi=0,lo=0;bool stable=false;
    for(unsigned i=0;i<3;i++) {
        hi=rd(VUL1_CUR_MSB);lo=rd(VUL1_CUR);
        if(hi==rd(VUL1_CUR_MSB)){stable=true;break;}
    }
    uint64_t now=mach_absolute_time(),elapsed;
    absolutetime_to_nanoseconds(now-last_poll,&elapsed);
    uint64_t stalled;absolutetime_to_nanoseconds(now-last_progress,&stalled);
    if(stalled>=250000000)return ETIMEDOUT;
    dma_diagnostic.last_register=VUL1_CUR;
    if(!stable)return EAGAIN; /* instability is still bounded by last_progress */
    if(hi&~0x1ffu)return EIO;
    uint64_t ptr=((uint64_t)hi<<32)|lo,base=hw.dma_bus_address;
    if(!ptr && stalled<100000000){last_poll=now;return 0;} /* bounded initial DMA wait */
    if(ptr<base || ptr>=base+hw.dma_bytes || (ptr&3))return EIO;
    dma_diagnostic.cur_after=ptr;
    uint32_t next=(uint32_t)(ptr-base);
    /* A pointer alone cannot distinguish a full wrap. If the worker was
     * delayed >=100ms (ring >=341ms), report the estimated discontinuity
     * then stop/fault. Do not silently rebase and resume a continuous stream.
     * This estimate is not a hardware frame counter or measured sample rate. */
    if(elapsed>=100000000) {
        uint64_t frames=elapsed*X9_MIC_RATE/1000000000;
        x9_mic_backend_drop(generation,(uint32_t)MIN(frames,UINT32_MAX));
        dma_diagnostic.dropped_frames+=(uint32_t)MIN(frames,UINT32_MAX);
        return EOVERFLOW;
    }
    uint32_t bytes=(next+hw.dma_bytes-cursor)%hw.dma_bytes;
    if(!bytes){last_poll=now;return 0;}
    last_progress=now;
    if(bytes>hw.dma_bytes/2)return EIO;
    int16_t mono[256];
    while(bytes) {
        uint32_t n=MIN(MIN(bytes,hw.dma_bytes-cursor)/4,256u);
        int copy=hw.copy_frames(cursor,mono,n,hw.adc_channel);
        if(copy){bzero(mono,sizeof(mono));return copy;}
        dma_diagnostic.observed_frames+=n;
        /* Preserve increasing last-sample time across chunks of one poll. */
        uint64_t behind,t=now;
        nanoseconds_to_absolutetime((uint64_t)(bytes-n*4)/4*1000000000/X9_MIC_RATE,&behind);
        if(t>behind)t-=behind;
        int e=x9_mic_deliver(generation,mono,n,t);
        bzero(mono,sizeof(mono));
        if(e)return e; /* never silently advance past rejected PCM */
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
                diagnostic.last_errno=e;dma_diagnostic.last_errno=e;
                uint32_t reg=dma_diagnostic.last_register;
                uint64_t g=generation;int off=stop_locked();dma_diagnostic.last_register=reg;
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
    if(!p || p->version!=6 || p->adc_channel>1 || !p->afe || !p->dma ||
       (p->dma_memory_type!=X9_MIC_DMA_MEMORY_SRAM && p->dma_memory_type!=X9_MIC_DMA_MEMORY_DRAM) ||
       !p->prepare || !p->unprepare || !p->copy_frames || !p->sync_for_device || !p->dma_quiesce ||
       ((uintptr_t)p->afe&3) || ((uintptr_t)p->dma&15) || (p->dma_bus_address&15) ||
       !p->dma_bus_address || p->dma_bytes<65536 || p->dma_bytes>131072 || (p->dma_bytes&15) ||
       p->dma_bus_address>((1ULL<<41)-p->dma_bytes))return EINVAL;
    if(p->prepare==x9_mic_plj110_capture_prepare &&
       p->rearm!=x9_mic_plj110_capture_rearm)return EINVAL;
    lck_mtx_lock(&afe_mic_lock);
    if(registered || (board_diagnostic.flags&X9_MIC_BOARD_RETAINED)) {
        lck_mtx_unlock(&afe_mic_lock);return EBUSY;
    }
    if(p->prepare==x9_mic_plj110_capture_prepare &&
       (board_diagnostic.flags&X9_MIC_BOARD_ATTEMPTED)) {
        lck_mtx_unlock(&afe_mic_lock);return EALREADY;
    }
    hw=*p;
    int e=0;
    if(!worker_created) {
        thread_t t;
        if(kernel_thread_start(afe_worker,NULL,&t)!=KERN_SUCCESS) {
            e=ENOMEM;
        } else {thread_deallocate(t);worker_created=true;}
    }
    const struct x9_mic_backend backend={.version=3,
        /* PLJ110's actual full-drain provider remains required by binding.
         * Rearm checks a completed STOP, then repeats real cold calibration;
         * there is no spent one-shot READY latch for this lifecycle. */
        .flags=0,
        .start=afe_start,.commit=afe_commit,.stop=afe_stop};
    if(!e)e=x9_mic_register_backend(&backend);
    if(!e){registered=true;dma_diagnostic.blockers=0;dma_diagnostic.flags|=X9_MIC_DMA_BOUND;}
    lck_mtx_unlock(&afe_mic_lock);return e;
}
int x9_mic_plj110_register_dma(const struct x9_mic_plj110_dma *p)
{
    if(!p || p->version!=2 || p->flags!=X9_MIC_PLJ110_DMA_REQUIRED ||
       p->adc_channel!=0)return EINVAL;
    /* This supplies real PLJ110 lifecycle callbacks, not stub DMA/owner
     * callbacks. AFE SRAM SMC/idle/board XML cannot satisfy p's contract. */
    const struct x9_mic_mt6993_platform platform={
        .version=6,.adc_channel=0,.afe=p->afe,.dma=p->ring,
        .dma_bus_address=p->bus_address,.dma_bytes=p->ring_bytes,
        .dma_memory_type=p->memory_type,
        .prepare=x9_mic_plj110_capture_prepare,
        .unprepare=x9_mic_plj110_capture_unprepare,
        .rearm=x9_mic_plj110_capture_rearm,
        .dma_quiesce=p->quiesce,.copy_frames=p->copy_frames,
        .sync_for_device=p->sync_for_device};
    return x9_mic_mt6993_register(&platform);
}
void x9_mic_mt6993_get_hardware(struct x9_mic_hardware_status *s)
{
    lck_mtx_lock(&afe_mic_lock);*s=diagnostic;lck_mtx_unlock(&afe_mic_lock);
}
int x9_mic_mt6993_power_probe(void)
{
    lck_mtx_lock(&afe_mic_lock);
    int e=(registered || prepared || active || dma_stuck)?EBUSY:
        x9_mic_plj110_power_probe(&diagnostic,&board_diagnostic);
    lck_mtx_unlock(&afe_mic_lock);return e;
}
void x9_mic_mt6993_get_board(struct x9_mic_board_status *s)
{
    lck_mtx_lock(&afe_mic_lock);*s=board_diagnostic;lck_mtx_unlock(&afe_mic_lock);
}
int x9_mic_mt6993_calibrate(void)
{
    lck_mtx_lock(&afe_mic_lock);
    int e=(registered || prepared || active || dma_stuck)?EBUSY:
        x9_mic_plj110_calibrate(&diagnostic,&board_diagnostic,&afe_diagnostic);
    lck_mtx_unlock(&afe_mic_lock);return e;
}
void x9_mic_mt6993_get_afe(struct x9_mic_afe_status *s)
{
    lck_mtx_lock(&afe_mic_lock);*s=afe_diagnostic;lck_mtx_unlock(&afe_mic_lock);
}
int x9_mic_mt6993_analog_l(void)
{
    lck_mtx_lock(&afe_mic_lock);
    int e=(registered || prepared || active || dma_stuck)?EBUSY:
        x9_mic_plj110_analog_l(&diagnostic,&board_diagnostic,&afe_diagnostic,&analog_diagnostic);
    lck_mtx_unlock(&afe_mic_lock);return e;
}
void x9_mic_mt6993_get_analog(struct x9_mic_analog_status *s)
{
    lck_mtx_lock(&afe_mic_lock);*s=analog_diagnostic;lck_mtx_unlock(&afe_mic_lock);
}
int x9_mic_mt6993_digital_l(void)
{
    lck_mtx_lock(&afe_mic_lock);
    int e=(registered || prepared || active || dma_stuck)?EBUSY:
        x9_mic_plj110_digital_l(&diagnostic,&board_diagnostic,&afe_diagnostic,
            &analog_diagnostic,&digital_diagnostic,&dma_diagnostic);
    lck_mtx_unlock(&afe_mic_lock);return e;
}
void x9_mic_mt6993_get_digital(struct x9_mic_digital_status *s)
{
    lck_mtx_lock(&afe_mic_lock);*s=digital_diagnostic;lck_mtx_unlock(&afe_mic_lock);
}

void x9_mic_mt6993_get_dma(struct x9_mic_dma_status *s)
{
    lck_mtx_lock(&afe_mic_lock);*s=dma_diagnostic;lck_mtx_unlock(&afe_mic_lock);
}
void x9_mic_mt6993_get_signal(struct x9_mic_signal_status *s)
{
    lck_mtx_lock(&afe_mic_lock);x9_mic_plj110_get_signal(s);lck_mtx_unlock(&afe_mic_lock);
}

#include "x9_mic_mt6993_memory.inc"
