/* Boot microphone check (x9_mic_boot=<ms>, needs x9_mic=2).
 * Binds the native DRAM ring + VUL1 drain provider, then records once through
 * the normal START/commit/STOP backend: MT6681 cold power, MTKAIF phase, ADC
 * RC, codec SRC/TX, AFE RX, VUL1 DMA. Only statistics are printed: no PCM is
 * stored, written or routed anywhere, and nothing is played back. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <mach/vm_types.h>
#include <kern/clock.h>
#include "x9_microphone.h"
#include "x9_mic_mt6993.h"
#include "x9_mic_plj110.h"
#include "x9_mic_mt6993_regs.h"
#include "x9_mic_bringup.h"
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int sweet_x9_microphone_mode(void);
extern void IOSleep(unsigned int); /* let the USB console drain (logs 260/261) */
static volatile uint32_t *afe;
static bool bound;
/* Called by the memory provider after VUL1 ON was cleared and read back, with
 * the board's AFE clock lease still held. Requires ON=0, the write cursor to
 * stop moving and the VUL1 monitor's pending/WEN/sync bits to clear, on two
 * consecutive 1 ms samples; bounded at 20 ms, otherwise the ring is retained. */
static int vul1_quiesce(void *context)
{
    (void)context;
    if(!afe)return ENXIO;
    uint32_t prev=afe[VUL1_CUR/4],stable=0;
    for(unsigned i=0;i<20;i++) {
        delay(1000);
        uint32_t con=afe[VUL1_CON0/4],mon=afe[VUL1_MON0/4],cur=afe[VUL1_CUR/4];
        if(con&VUL1_ON)return EBUSY;
        stable=(cur==prev && !(mon&VUL1_MON_BUSY))?stable+1:0;
        prev=cur;
        if(stable>=2)return 0;
    }
    return ETIMEDOUT;
}
static uint64_t isqrt(uint64_t v)
{
    uint64_t r=0,b=1ull<<62;
    while(b>v)b>>=2;
    while(b){if(v>=r+b){v-=r+b;r=(r>>1)+b;}else r>>=1;b>>=2;}
    return r;
}
static void report(void)
{
    struct x9_mic_hardware_status h;struct x9_mic_board_status b;struct x9_mic_afe_status a;
    struct x9_mic_analog_status an;struct x9_mic_digital_status d;struct x9_mic_dma_status m;
    struct x9_mic_signal_status s;
    x9_mic_mt6993_get_hardware(&h);x9_mic_mt6993_get_board(&b);x9_mic_mt6993_get_afe(&a);
    x9_mic_mt6993_get_analog(&an);x9_mic_mt6993_get_digital(&d);x9_mic_mt6993_get_dma(&m);
    x9_mic_mt6993_get_signal(&s);
    printf("X9 MIC: board stage=%u flags=%x err=%u restore=%u reg=%x bus_fault=%x/%u | codec stage=%u flags=%x err=%u restore=%u reg=%x\n",
        b.stage,b.flags,b.last_errno,b.restore_errno,b.last_register,b.bus_fault_register,b.bus_fault_errno,
        h.stage,h.flags,h.last_errno,h.restore_errno,h.last_register);
    printf("X9 MIC: afe stage=%u flags=%x err=%u restore=%u reg=%x | analog stage=%u flags=%x err=%u restore=%u reg=%x\n",
        a.stage,a.flags,a.last_errno,a.restore_errno,a.last_register,
        an.stage,an.flags,an.last_errno,an.restore_errno,an.last_register);
    printf("X9 MIC: digital stage=%u flags=%x err=%u restore=%u reg=%x codec_restore=%u afe_restore=%u | signal flags=%x err=%u reads=%u\n",
        d.stage,d.flags,d.last_errno,d.restore_errno,d.last_register,d.codec_restore_errno,d.afe_restore_errno,
        s.flags,s.last_errno,s.reads);
    printf("X9 MIC: dma stage=%u flags=%x err=%u restore=%u reg=%x active_memifs=%x base=%llx cur=%llx observed=%llu dropped=%llu journal=%u/%u\n",
        m.stage,m.flags,m.last_errno,m.restore_errno,m.last_register,m.active_memifs,
        m.base,m.cur_after,m.observed_frames,m.dropped_frames,m.journal_count,m.journal_restored);
}
int x9_mic_bringup_run(unsigned ms)
{
    printf("X9 MIC: boot capture %u ms (record only; statistics, nothing stored or played)\n",ms);IOSleep(250);
    if(sweet_x9_microphone_mode()!=2){printf("X9 MIC: x9_mic mode %d, need 2\n",sweet_x9_microphone_mode());return ENXIO;}
    int e=0;
    if(!bound) {
        if(!afe)afe=(volatile uint32_t *)ml_io_map(X9_MIC_AFE_PHYS,X9_MIC_AFE_BYTES);
        if(!afe){printf("X9 MIC: AFE map failed\n");return ENOMEM;}
        const struct x9_mic_plj110_native_dma p={.version=2,
            .flags=X9_MIC_PLJ110_AIN0_VERIFIED|X9_MIC_PLJ110_DMA_DRAIN_VERIFIED,
            .afe=afe,.ring_bytes=65536,.context=NULL,.quiesce=vul1_quiesce};
        e=x9_mic_plj110_register_native_dma(&p);
        printf("X9 MIC: native DMA bind -> %d\n",e);IOSleep(250);
        if(e){report();return e;}
        bound=true;
    }
    struct x9_mic_capture_result r;
    e=x9_mic_kernel_capture(ms,&r);
    uint64_t rms=r.frames?isqrt(r.sum_squares/r.frames):0;
    long long mean=r.frames?(long long)(r.sum/(int64_t)r.frames):0;
    printf("X9 MIC: capture -> %d start=%d run=%d stop=%d last=%d frames=%llu captured=%llu dropped=%llu zeros=%llu\n",
        e,r.start_errno,r.run_errno,r.stop_errno,r.last_errno,r.frames,r.captured,r.dropped,r.zeros);
    printf("X9 MIC: pcm min=%d max=%d mean=%lld rms=%llu first=%d,%d,%d,%d,%d,%d,%d,%d\n",
        r.min,r.max,mean,rms,r.first[0],r.first[1],r.first[2],r.first[3],
        r.first[4],r.first[5],r.first[6],r.first[7]);
    report();IOSleep(500);
    return e;
}
