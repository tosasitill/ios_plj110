/* Production lifecycle. No output-ring access, arbitrary RAM polling,
 * automatic retry/reset, power acquisition or Ready registration. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/locks.h>
#include "x9_dsp_playback.h"
#include "x9_dsp_shared.h"
#include "x9_adsp_audio_irq.h"
#include "x9_adsp_mt6993.h"
#include "x9_audio_resources.h"
LCK_GRP_DECLARE(playback_group,"x9-dsp-playback");
LCK_MTX_DECLARE(playback_lifecycle,&playback_group);
LCK_MTX_DECLARE(playback_session,&playback_group);
static struct {
    struct x9_dsp_pcm_session pcm;
    struct x9_dsp_pcm_region output;
    uint64_t ticket,child;
    uint32_t generation,last_generation,error,protocol_error;
    bool attempted,accepting,start_attempted,protocol_stopped;
} playback;
/* lifecycle protects identity/state; session protects ALL PCM accesses,
 * including the real IRQ worker. Never wait for worker pins under session. */
static int matches(uint64_t ticket,uint32_t generation)
{
    return !playback.attempted||!ticket||ticket!=playback.ticket||!generation||
        generation!=playback.generation?EPERM:0;
}
static int fault(int e)
{
    if(e==EAGAIN)e=EIO;
    if(!playback.error)playback.error=(uint32_t)e;
    playback.accepting=false;
    x9_audio_resources_fault(X9AudioResourceSpeaker,playback.ticket,e);
    return e;
}
static int halted(void)
{
    X9AudioDSPStatus s={0};int e=x9_adsp_status(0,&s);
    if(!e&&(s.generation!=playback.generation||s.state!=X9DSPOffline))e=ESTALE;
    if(!e)e=x9_adsp_mt6993_speaker_halted_validate(playback.ticket);
    return e;
}
/* BOTH lifecycle and session held. IRQ validator takes only its own lock,
 * in the SAME session->IRQ order as the actual worker. */
static int admission(void)
{
    if(playback.error)return (int)playback.error;
    if(!playback.accepting||!playback.child||!playback.pcm.bound)return ENXIO;
    int e=playback.pcm.error?(int)playback.pcm.error:0;
    if(!e)e=playback.pcm.hw.validate(playback.pcm.hw.context);
    if(!e)e=playback.pcm.hw.validate_afe(playback.pcm.hw.context,&playback.pcm.afe_buffer);
    if(!e&&playback.start_attempted)e=x9_adsp_audio_irq_validate(playback.ticket,
        playback.generation,&playback.pcm);
    return e?fault(e):0;
}
int x9_dsp_playback_prepare(uint64_t ticket,uint32_t generation,
    const struct x9_dsp_pcm_buffer *afe,uint32_t start_frames)
{
    if(!ticket||!generation||!afe)return EINVAL;
    const struct x9_dsp_pcm_buffer a=*afe;
    /* Syntax first; real supplier below must still prove the exact route. */
    uint8_t wire[X9_DSP_PCM_RECORD_BYTES];
    int e=x9_dsp_pcm_encode_buffer(&a,wire,sizeof(wire));
    if(e)return e;
    if(a.hardware!=1||a.memory!=1||a.format!=2||a.rate!=48000||a.channels!=2||
       a.direction!=1||a.bytes!=X9_DSP_SHARED_RING_BYTES||a.read!=a.base||
       a.write!=a.base||a.count||a.counter||a.period_frames!=480||a.periods!=64||
       start_frames<960||start_frames>=a.bytes/4||start_frames%480)return EINVAL;
    lck_mtx_lock(&playback_lifecycle);
    if(playback.attempted||generation<=playback.last_generation) {
        lck_mtx_unlock(&playback_lifecycle);return EBUSY;
    }
    /* Foreign ticket cannot consume this singleton's epoch. */
    e=x9_adsp_mt6993_speaker_validate(ticket);
    X9AudioDSPStatus s={0};if(!e)e=x9_adsp_status(0,&s);
    if(!e&&(s.generation!=generation||s.state!=X9DSPOnline||s.error))e=ESTALE;
    if(e){lck_mtx_unlock(&playback_lifecycle);return e;}
    playback.ticket=ticket;playback.generation=playback.last_generation=generation;
    playback.attempted=true;playback.accepting=false;playback.error=playback.protocol_error=0;
    playback.start_attempted=playback.protocol_stopped=false;
    e=x9_dsp_shared_pcm_hold(ticket,generation,&playback.child);
    struct x9_dsp_pcm_binding binding={0};
    if(!e)e=x9_dsp_shared_pcm_binding(&binding);
    if(!e&&(binding.core||binding.generation!=generation))e=ESTALE;
    if(!e)e=x9_dsp_shared_afe_region(&playback.output);
    if(!e&&(playback.output.bus!=a.base||playback.output.bytes!=a.bytes))e=EPERM;
    if(!e)e=binding.validate_afe(binding.context,&a);
    lck_mtx_lock(&playback_session);
    if(!e)e=x9_dsp_pcm_bind(&playback.pcm,&binding);
    if(!e)e=x9_dsp_pcm_open(&playback.pcm);
    if(!e)e=x9_dsp_pcm_hw_params(&playback.pcm,480);
    if(!e)e=x9_dsp_pcm_afe_params(&playback.pcm,&a,false);
    if(!e)e=x9_dsp_pcm_afe_params(&playback.pcm,&a,true);
    if(!e)e=x9_dsp_pcm_prepare(&playback.pcm,start_frames);
    if(e)e=fault(e);else playback.accepting=true;
    lck_mtx_unlock(&playback_session);lck_mtx_unlock(&playback_lifecycle);return e;
}
int x9_dsp_playback_submit(uint64_t ticket,uint32_t generation,
    const int16_t *stereo,uint32_t frames)
{
    if(!stereo||!frames||frames>X9_DSP_SHARED_RING_BYTES/4)return EINVAL;
    uint64_t address=(uint64_t)(uintptr_t)stereo,bytes=(uint64_t)frames*4;
    if((address&1)||address>UINT64_MAX-bytes)return EINVAL;
    lck_mtx_lock(&playback_lifecycle);int e=matches(ticket,generation);
    if(e){lck_mtx_unlock(&playback_lifecycle);return e;}
    /* Never accept DSP output as an input pointer. Native PCM also rejects
     * ATOD/DTOA/input overlap. Compare metadata only; do not read output. */
    uint64_t out=(uint64_t)(uintptr_t)playback.output.cpu;
    if(playback.output.cpu&&address<out+playback.output.bytes&&out<address+bytes) {
        lck_mtx_unlock(&playback_lifecycle);return EINVAL;
    }
    lck_mtx_lock(&playback_session);e=admission();
    if(!e)e=x9_dsp_pcm_submit(&playback.pcm,stereo,frames);
    /* EAGAIN only before copying for input-ring backpressure; native submit
     * converts authority/mailbox EAGAIN to a latched fault. */
    if(e&&e!=EAGAIN&&e!=EINVAL)e=fault(e);
    if(!e&&playback.start_attempted)e=x9_adsp_audio_irq_validate(ticket,generation,&playback.pcm);
    if(e&&e!=EAGAIN&&e!=EINVAL)e=fault(e);
    lck_mtx_unlock(&playback_session);lck_mtx_unlock(&playback_lifecycle);return e;
}
int x9_dsp_playback_start(uint64_t ticket,uint32_t generation)
{
    lck_mtx_lock(&playback_lifecycle);int e=matches(ticket,generation);
    if(e){lck_mtx_unlock(&playback_lifecycle);return e;}
    lck_mtx_lock(&playback_session);e=admission();
    if(!e&&playback.start_attempted)e=EBUSY;
    if(!e&&(!(playback.pcm.flags&X9PCMPrepared)||!(playback.pcm.flags&X9PCMAFEPrepared)))e=ENXIO;
    if(!e&&(playback.pcm.pending_frames||
       playback.pcm.submitted<playback.pcm.buffer.start_threshold))e=EAGAIN;
    if(!e)playback.start_attempted=true;
    lck_mtx_unlock(&playback_session);
    if(!e) {
        /* bind takes session itself. lifecycle excludes submit/teardown,
         * while queued workers remain free to acquire session and drain. */
        e=x9_adsp_audio_irq_bind(ticket,generation,&playback.pcm,&playback_session);
        lck_mtx_lock(&playback_session);
        if(!e)e=admission(); /* Catch an actual IRQ fault before START. */
        if(!e)e=x9_dsp_pcm_start(&playback.pcm);
        if(!e)e=x9_adsp_audio_irq_validate(ticket,generation,&playback.pcm);
        if(e)e=fault(e); /* no bind/START replay; retain partial ownership */
        lck_mtx_unlock(&playback_session);
    }
    lck_mtx_unlock(&playback_lifecycle);return e;
}
int x9_dsp_playback_halt(uint64_t ticket,uint32_t generation)
{
    lck_mtx_lock(&playback_lifecycle);int e=matches(ticket,generation);
    if(e){lck_mtx_unlock(&playback_lifecycle);return e;}
    playback.accepting=false;
    if(x9_adsp_audio_irq_binding_held(ticket,generation,&playback.pcm))
        e=x9_adsp_audio_irq_quiesce(ticket,generation,&playback.pcm);
    /* Best-effort protocol cleanup at most once. A protocol error must not
     * prevent independent safety drain/halt. STOP/CLOSE ACK is not halt. */
    lck_mtx_lock(&playback_session);
    if(!playback.protocol_stopped) {
        playback.protocol_stopped=true;
        if(playback.pcm.bound) {
            int stop=x9_dsp_pcm_stop(&playback.pcm);
            int close=x9_dsp_pcm_close(&playback.pcm);
            int protocol=stop?stop:close;
            if(protocol){playback.protocol_error=(uint32_t)protocol;fault(protocol);}
        }
    }
    lck_mtx_unlock(&playback_session);
    /* Failure to mask/drain workers forbids reset even if STOP succeeded.
     * Missing shared pin never grants an independently unproved reset. */
    if(!e&&playback.child)e=x9_dsp_shared_pcm_drained(ticket,generation,playback.child);
    if(!e&&playback.child)e=x9_adsp_mt6993_speaker_control(1,ticket);
    if(!e)e=halted();
    if(e)e=fault(e);
    lck_mtx_unlock(&playback_lifecycle);return e;
}
int x9_dsp_playback_release(uint64_t ticket,uint32_t generation)
{
    lck_mtx_lock(&playback_lifecycle);int e=matches(ticket,generation);
    if(e){lck_mtx_unlock(&playback_lifecycle);return e;}
    playback.accepting=false;e=halted();
    if(!e&&x9_adsp_audio_irq_binding_held(ticket,generation,&playback.pcm))
        e=x9_adsp_audio_irq_release(ticket,generation,&playback.pcm);
    if(!e&&playback.child) {
        e=x9_dsp_shared_pcm_drop(ticket,generation,playback.child);
        if(!e)playback.child=0;
    }
    if(!e) {
        /* IRQ references are retired BEFORE shared drop/session retirement;
         * a Running flag alone never permits parent unmap. */
        lck_mtx_lock(&playback_session);memset(&playback.pcm,0,sizeof(playback.pcm));
        memset(&playback.output,0,sizeof(playback.output));
        lck_mtx_unlock(&playback_session);playback.attempted=false;
        /* last generation and first errors survive; no same-epoch retry. */
    } else e=fault(e);
    lck_mtx_unlock(&playback_lifecycle);return e;
}
