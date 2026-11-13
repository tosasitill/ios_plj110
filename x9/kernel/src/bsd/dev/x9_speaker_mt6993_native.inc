/* Production AUDIO/APLL2 child of the real core-A board owner. Included
 * under x9_speaker_clock_lock, using the same CCF/meter primitives but NOT
 * the cold TOP-off diagnostic admission, OUT4 journal or probe flags.
 * Source: mt6993-afe-clk.c APLL2 + clk-mt6993-adsp.c bank1 bits3/4/12
 * and bank3 ETDM_OUT4 bit17. AUDIO_ENGEN is offset0x14, NOT bank3 CG0x08.
 */
#include <vm/pmap.h>
extern ppnum_t pmap_find_phys(pmap_t,addr64_t);
static int take(bool,bool);
static int restore(void);
static struct {
    struct x9_speaker_clock_policy policy;
    uint64_t child,pad_child,pa_child,rail_child,leaf_serial;
    uint32_t last_generation,error,dirty,uncertain,prior[5];
    bool active,configured,tuner_captured,clocks_restored;
} native_clock;
/* OPPO mt6993_afe_runtime_resume (SPM 26M request, CBIP, ENGEN bit0
 * AUDIO_26M_EN_ON = "enable AFE") + mt6993_apll2_enable (tuner, bit3), the
 * same engine bits the mic owns (x9_mic_plj110_afe.inc). Without bit0 the
 * memif/ETDM never run. Restored in reverse; ENGEN off waits MON 0x7c. */
enum { NT_SPM, NT_CBIP, NT_TUNER, NT_ENGEN, NT_CG, NT_COUNT };
static struct field native_tuner[]={
    /* AFE_SPM_CONTROL_REQ: SRCCLKENA bit2 (resume) + mt6993_afe_dram_request
     * APSRC bit3/DDREN bit6/DDREN_URG bit7, needed by the DRAM-backed DL24
     * (the mic's VUL1 sets the same three). Held until after the drain. */
    {0x60,0xcc,0xcc,0,0},
    {0x4380,1,1,0,0},       /* AFE_CBIP_CFG0 bit0 */
    {0x38,0xfff7,0x375,0,0}, /* AFE_APLL2_TUNER_CFG, source 0x374 + EN */
    {0x14,9,9,0,0},         /* AUDIO_ENGEN_CON0 26M_EN_ON bit0 + APLL2_EN_ON bit3 */
    {0x08,1u<<17,0,0,0},    /* bank3 ETDM_OUT4 clock CG only, NOT engine EN */
};
_Static_assert(sizeof(native_tuner)/sizeof(native_tuner[0])==NT_COUNT,"native tuner journal");
static uint32_t native_engen_expected(void)
{return native_clock.tuner_captured?native_tuner[NT_ENGEN].expected&15:0;}
static int native_engen_idle(void)
{
    uint32_t value=rd(j.afe,0x14)&15;
    if(value==native_engen_expected())return 0;
    /* Cleanup of the LAST uncertain ENGEN write accepts only its observed
     * prior, not a generic inactive value or the generation's old baseline. */
    if(native_clock.tuner_captured && (native_clock.uncertain&(1u<<NT_ENGEN)) &&
       value==(native_clock.prior[NT_ENGEN]&15))return 0;
    s.last_register=0x14;s.last_value=value;return EBUSY;
}
_Static_assert(sizeof(X9AudioClockStatus)==216,"clock diagnostic ABI unchanged");
_Static_assert(sizeof(X9AudioPLLStatus)==264,"PLL diagnostic ABI unchanged");
static int native_fault(int e)
{
    if(e==EAGAIN)e=EIO;
    if(!native_clock.error)native_clock.error=(uint32_t)e;
    s.error=pll_s.error=native_clock.error;
    s.flags|=X9ClockPoisoned;pll_s.flags|=X9PLLPoisoned;
    x9_audio_resources_fault(X9AudioResourceSpeaker,s.ticket,e);return e;
}
static int native_board(void)
{
    /* AP-only hold: no transport exists; board lease + generation only. */
    if(x9_adsp_mt6993_speaker_ap_only())
        return x9_adsp_mt6993_speaker_online(s.ticket,native_clock.policy.generation);
    int e=x9_adsp_mt6993_speaker_validate(s.ticket);
    X9AudioDSPStatus ds={0};if(!e)e=x9_adsp_status(0,&ds);
    /* Transport quarantine is NOT loss of this physical child. In particular
     * a malformed reply/watchdog must not prevent mute -> AFE disable/drain
     * BEFORE the real DSP halt. No new playback is admitted here and neither
     * DSP nor clock historical errors are cleared. Board/HWV accessibility,
     * generation and owned clock images are still freshly qualified. */
    if(!e && (ds.generation!=native_clock.policy.generation ||
       (ds.state!=X9DSPOnline && ds.state!=X9DSPOffline &&
        ds.state!=X9DSPQuarantined)))e=ESTALE;
    if(!e && ds.state==X9DSPOffline)e=x9_adsp_mt6993_speaker_halted_validate(s.ticket);
    return e;
}
static int native_mapping(void)
{
    if(!j.afe || (j.afe&PAGE_MASK) || PAGE_SIZE!=0x4000)return EINVAL;
    for(uint32_t off=0;off<0x10000;off+=PAGE_SIZE) {
        ppnum_t p=pmap_find_phys(kernel_pmap,(addr64_t)(j.afe+off));
        if(!p || ptoa_64(p)!=0x1a110000ULL+off)return EFAULT;
    }
    return 0;
}
static int native_tuner_change(unsigned n,uint32_t value)
{
    /* Independent board AO qualification occurs before AFE access. A PLL
     * fault does not grant access by clearing an error flag on this child. */
    int e=native_board();if(!e)e=access();if(e)return e;
    struct field *p=&native_tuner[n];uint32_t bit=1u<<n,old=rd(j.afe,p->off);
    if((old&p->mask)!=(p->expected&p->mask) &&
       !((native_clock.uncertain&bit) && (old&p->mask)==(native_clock.prior[n]&p->mask))) {
        s.last_register=p->off;s.last_value=old;return EBUSY;
    }
    uint32_t next=(old&~p->mask)|(value&p->mask);
    if((next&p->mask)==(old&p->mask)) {
        p->expected=next;native_clock.uncertain&=~bit;return 0;
    }
    native_clock.prior[n]=old;p->expected=next;
    native_clock.dirty|=bit;native_clock.uncertain|=bit;
    wr(j.afe,p->off,next);
    /* Qualify the same board again before readback; a revoked lease cannot
     * be repaired by probing powered-down AFE registers. */
    e=native_board();if(!e)e=access();if(e)return e;
    uint32_t actual=rd(j.afe,p->off);
    if((actual&p->mask)!=(next&p->mask)) {
        s.last_register=p->off;s.last_value=actual;return EIO;
    }
    native_clock.uncertain&=~bit;return 0;
}
static int native_tuner_verify(bool saved,bool recovering)
{
    if(!native_clock.tuner_captured)return 0;
    int e=native_board();if(!e)e=access();if(e)return e;
    for(unsigned n=0;n<NT_COUNT;n++) {
        struct field *p=&native_tuner[n];uint32_t v=rd(j.afe,p->off)&p->mask;
        uint32_t expected=(saved?p->saved:p->expected)&p->mask;
        if(v!=expected && !(recovering && !saved && (native_clock.uncertain&(1u<<n)) &&
           v==(native_clock.prior[n]&p->mask))) {
            s.last_register=p->off;s.last_value=v;return EBUSY;
        }
    }
    return 0;
}
static int native_tuner_take(void)
{
    int e=native_board();if(!e)e=idle();if(!e)e=pll_consumers();if(e)return e;
    /* All memifs/engines were proved inactive, but APLL clocks have their
     * own enable register. Do not borrow an inherited active generator. */
    e=refuse(j.afe,0x14,15,0);if(e)return e;
    /* A tuner already enabled is accepted only in exactly our image (then
     * the write is a no-op and restore keeps it). Log 281: TOP_CON2 (0x8)
     * reads 0, i.e. LK left the ETDM clocks ungated; ungated is the target,
     * so both 0 and 1 are accepted (restore returns the observed value). */
    uint32_t tuner=rd(j.afe,0x38);
    if((tuner&1) && (tuner&0xfff7)!=0x375){s.last_register=0x38;s.last_value=tuner;return EBUSY;}
    printf("X9 SPK CLK: inherited spm=%x cbip=%x tuner=%x engen=%x top2=%x top4=%x\n",
        rd(j.afe,0x60),rd(j.afe,0x4380),tuner,rd(j.afe,0x14),rd(j.afe,8),rd(j.afe,0x10));
    for(unsigned n=0;n<NT_COUNT;n++)native_clock.prior[n]=native_tuner[n].saved=
        native_tuner[n].expected=rd(j.afe,native_tuner[n].off);
    native_clock.tuner_captured=true;
    e=change(0,0); /* HOPPING/F26M/APLL2/H208M/TUNER2 bits0/1/3/4/12 only */
    for(unsigned n=0;!e && n<NT_COUNT;n++)e=native_tuner_change(n,native_tuner[n].value);
    if(!e)e=native_tuner_verify(false,false);return e;
}
static int native_tuner_restore(void)
{
    if(!native_clock.tuner_captured)return 0;
    int e=native_tuner_verify(false,true);if(e)return e;
    for(unsigned n=NT_COUNT;n>0;n--)if(native_clock.dirty&(1u<<(n-1))) {
        e=native_tuner_change(n-1,native_tuner[n-1].saved);if(e)return e;
        native_clock.dirty&=~(1u<<(n-1));
        /* As the mic and OPPO disable path: AFE off must settle (MON 0)
         * before the tuner/CBIP/SPM request below it is withdrawn. */
        if(n-1==NT_ENGEN && !(native_tuner[NT_ENGEN].saved&1)) {
            e=wait_bits(j.afe,0x7c,UINT32_MAX,0,10000);if(e)return e;
        }
    }
    e=native_tuner_verify(true,false);if(e)return e;
    /* Gate APLL2/H208M/TUNER2 before dropping their source, but keep register
     * HOPPING/F26M until the final AFE/clock-journal restore. */
    e=change(0,fields[0].expected|0x1018);if(e)return e;
    native_clock.tuner_captured=false;return 0;
}
static int native_validate_locked(uint64_t ticket)
{
    if(!native_clock.active || !j.combined || !ticket || s.ticket!=ticket)return EPERM;
    /* This is PHYSICAL access validation for the AFE platform, not its new
     * stream/Ready decision. A remembered stream fault cannot block cleanup;
     * unknown HWV/update/meter actions and unconfigured acquisition still do.
     * Never clear stuck journals to manufacture a successful validation. */
    if(!native_clock.configured || j.stuck || pj.stuck)return ENXIO;
    int e=native_board();if(!e)e=native_mapping();
    if(!e)e=access();if(!e)e=pll_consumers();
    /* CCF defines APLL2 FENC offset0x34 bit4. Expected-on is necessary,
     * but is NOT a PLL-lock or audio-rate certificate. Admission already
     * required fresh meter cycles; runtime must not trust their old flag
     * after source fencing or an unowned ENGEN enable changed. */
    if(!e)e=pll_refuse(j.apll,0x34,16,16);
    if(!e)e=native_engen_idle(); /* only clock generator image, not memif idle */
    if(!e)e=verify_fields(false);if(!e)e=native_tuner_verify(false,false);
    return e?native_fault(e):0;
}
int x9_speaker_mt6993_clock_acquire(const struct x9_speaker_clock_policy *p)
{
    if(!p || p->version!=X9_SPEAKER_CLOCK_POLICY_VERSION || !p->ticket ||
       !p->generation || !p->validate_drained)return EINVAL;
    if(!sweet_early_is_x9() || sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    lck_mtx_lock(&x9_speaker_clock_lock);
    int e=native_clock.active || (s.flags&X9ClockHeld) ||
        ((s.flags&X9ClockAttempted) && !j.combined) ||
        p->generation<=native_clock.last_generation?EBUSY:0;
    if(!e)e=x9_adsp_mt6993_speaker_online(p->ticket,p->generation);
    uint64_t child=0;if(!e)e=x9_adsp_mt6993_clock_hold(p->ticket,&child);
    if(!e) {
        native_clock.policy=*p;native_clock.child=child;native_clock.active=true;
        native_clock.last_generation=p->generation;native_clock.error=0;
        native_clock.configured=native_clock.clocks_restored=false;
        /* Previous native cycle may be forgotten only AFTER full physical
         * cleanup/drop. Permanent mappings are retained, not leaked per flow. */
        s=(X9AudioClockStatus){.version=X9_AUDIO_CLOCK_STATUS_VERSION,
            .flags=X9ClockAttempted|X9ClockHeld,.ticket=p->ticket};
        pll_s=(X9AudioPLLStatus){.version=X9_AUDIO_PLL_STATUS_VERSION,
            .flags=X9PLLAttempted|X9PLLHeld,.ticket=p->ticket};
        j.combined=true;j.stuck=false;j.captured=false;j.mux_owned=0;
        pj.stuck=pj.captured=pj.pll_written=pj.trigger_issued=pj.meter_done=false;
        pj.gen_fenc=0;native_clock.dirty=native_clock.uncertain=0;
        native_clock.tuner_captured=false;fields[0].mask=0x101b;
        s.smc_result[0]=s.smc_result[1]=INT64_MIN;
        /* Recheck AFTER pinning the parent: a changed/resetting transport
         * between the first status read and child acquisition grants nothing.
         * Do not issue AUDIO SMC/votes before this second live admission. */
        CLKSTEP("native board");
        e=native_board();
        printf("X9 SPK CLK: native board -> %d\n",e);IOSleep(600);
        if(!e)e=x9_adsp_mt6993_speaker_online(p->ticket,p->generation);
        if(!e)e=take(true,false);
        if(!e){CLKSTEP("native mapping");e=native_mapping();}
        if(!e){native_clock.configured=true;e=native_validate_locked(p->ticket);}
        if(e)e=native_fault(e); /* retained child; no automatic unwind */
    }
    lck_mtx_unlock(&x9_speaker_clock_lock);return e;
}
int x9_speaker_mt6993_clock_validate(uint64_t ticket)
{
    lck_mtx_lock(&x9_speaker_clock_lock);int e=native_validate_locked(ticket);
    lck_mtx_unlock(&x9_speaker_clock_lock);return e;
}
int x9_speaker_mt6993_clock_afe(uint64_t ticket,volatile uint32_t **out)
{
    if(!out)return EINVAL;
    lck_mtx_lock(&x9_speaker_clock_lock);int e=native_validate_locked(ticket);
    if(!e)*out=(volatile uint32_t *)j.afe;
    lck_mtx_unlock(&x9_speaker_clock_lock);return e;
}
int x9_speaker_mt6993_clock_held(uint64_t ticket)
{
    lck_mtx_lock(&x9_speaker_clock_lock);
    int held=native_clock.active && ticket && s.ticket==ticket;
    lck_mtx_unlock(&x9_speaker_clock_lock);return held;
}
static int native_leaf_hold(uint64_t ticket,uint64_t *lease,uint64_t *child)
{
    if(!lease)return EINVAL;*lease=0;
    lck_mtx_lock(&x9_speaker_clock_lock);
    int e=*child?EBUSY:native_validate_locked(ticket);
    if(!e&&child==&native_clock.rail_child&&native_clock.pa_child)e=EBUSY;
    if(!e && native_clock.error)e=ESTALE;
    if(!e)e=x9_adsp_mt6993_speaker_online(ticket,native_clock.policy.generation);
    if(!e && native_clock.leaf_serial==UINT64_MAX)e=EOVERFLOW;
    if(!e)*lease=*child=++native_clock.leaf_serial;
    lck_mtx_unlock(&x9_speaker_clock_lock);return e;
}
static int native_leaf_validate(uint64_t ticket,uint64_t lease,uint64_t *child)
{
    lck_mtx_lock(&x9_speaker_clock_lock);
    int e=!lease||lease!=*child?EPERM:native_validate_locked(ticket);
    lck_mtx_unlock(&x9_speaker_clock_lock);return e;
}
static int native_leaf_drop(uint64_t ticket,uint64_t lease,uint64_t *child)
{
    lck_mtx_lock(&x9_speaker_clock_lock);
    int e=!lease||lease!=*child?EPERM:native_validate_locked(ticket);
    if(!e)e=x9_adsp_mt6993_speaker_halted_validate(ticket);
    if(!e)*child=0;
    lck_mtx_unlock(&x9_speaker_clock_lock);return e;
}
int x9_speaker_mt6993_clock_pad_hold(uint64_t ticket,uint64_t *lease)
{return native_leaf_hold(ticket,lease,&native_clock.pad_child);}
int x9_speaker_mt6993_clock_pad_validate(uint64_t ticket,uint64_t lease)
{return native_leaf_validate(ticket,lease,&native_clock.pad_child);}
int x9_speaker_mt6993_clock_pad_drop(uint64_t ticket,uint64_t lease)
{return native_leaf_drop(ticket,lease,&native_clock.pad_child);}
int x9_speaker_mt6993_clock_pa_hold(uint64_t ticket,uint64_t *lease)
{return native_leaf_hold(ticket,lease,&native_clock.pa_child);}
int x9_speaker_mt6993_clock_pa_validate(uint64_t ticket,uint64_t lease)
{return native_leaf_validate(ticket,lease,&native_clock.pa_child);}
int x9_speaker_mt6993_clock_pa_drop(uint64_t ticket,uint64_t lease)
{return native_leaf_drop(ticket,lease,&native_clock.pa_child);}
int x9_speaker_mt6993_clock_rail_hold(uint64_t ticket,uint64_t *lease)
{return native_leaf_hold(ticket,lease,&native_clock.rail_child);}
int x9_speaker_mt6993_clock_rail_validate(uint64_t ticket,uint64_t lease)
{return native_leaf_validate(ticket,lease,&native_clock.rail_child);}
int x9_speaker_mt6993_clock_rail_quiesced_validate(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&x9_speaker_clock_lock);
    int e=!lease||lease!=native_clock.rail_child?EPERM:native_validate_locked(ticket);
    /* PA's I2C6 child pins pa_child, too. Check here, not via PA reset lock:
     * PA validate_power is permitted to enter the independent rail mutex. */
    if(!e&&native_clock.pa_child)e=EBUSY;
    if(!e)e=x9_adsp_mt6993_speaker_halted_validate(ticket);
    lck_mtx_unlock(&x9_speaker_clock_lock);return e;
}
int x9_speaker_mt6993_clock_rail_drop(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&x9_speaker_clock_lock);
    int e=!lease||lease!=native_clock.rail_child?EPERM:native_validate_locked(ticket);
    if(!e&&native_clock.pa_child)e=EBUSY;
    if(!e)e=x9_adsp_mt6993_speaker_halted_validate(ticket);
    if(!e)native_clock.rail_child=0;
    lck_mtx_unlock(&x9_speaker_clock_lock);return e;
}
static int native_restored_ao(void)
{
    int e=x9_adsp_mt6993_speaker_halted_validate(s.ticket);
    if(e)return e;
    /* A failed post-hold admission or first mapping created no clock action.
     * Do not require an AFE/PLL mapping merely to drop this untouched child.
     * Acquisition sets AOCaptured BEFORE its first SMC/vote/mux mutation. */
    if(!(s.flags&X9ClockAOCaptured))return s.owned_vote || j.mux_owned ||
        j.captured || pj.captured || native_clock.tuner_captured?EIO:0;
    if(!j.hwv || !j.vlp || !j.apll)return ENXIO;
    /* No AFE read after AUDIO power removal. Fresh AO proofs protect a
     * drop retry, not the cached clocks_restored boolean alone. */
    /* A borrowed (LK-inherited, log 269) AUDIO domain must still be on as
     * found, never busy; an owned one must be fully off. */
    const unsigned audio_off[]={0x20708,0x12900,0x13700,0x13704,0x1131c};
    const bool borrowed=s.borrowed_vote&1;
    for(unsigned n=0;!e && n<5;n++) {
        if(borrowed && n==4)continue; /* power status follows the inherited on state */
        e=refuse(j.hwv,audio_off[n],1,borrowed && n<3?1:0);
    }
    if(!e)e=refuse(j.vlp,0x90,0x8383,s.vlp_before&0x8383);
    if(!e)e=refuse(j.vlp,0x274,0xc0000000,s.fenc_before&0xc0000000);
    if(!e && (pll_s.flags&X9PLLCaptured)) {
        e=refuse(j.vlp,0x80,0x8300,pll_s.consumers[0]&0x8300);
        if(!e)e=refuse(j.vlp,0x270,4,pll_s.consumers[1]&4); /* borrowed ENGEN2 FENC stays on */
        for(unsigned n=0;!e && n<7;n++)e=refuse(j.apll,pll_regs[n],
            n==3?16:UINT32_MAX,pll_s.pll_before[n]&(n==3?16:UINT32_MAX));
    }
    return e;
}
int x9_speaker_mt6993_clock_release(uint64_t ticket)
{
    lck_mtx_lock(&x9_speaker_clock_lock);
    int e=!native_clock.active || !ticket || s.ticket!=ticket?EPERM:0;
    if(!e && (native_clock.pad_child || native_clock.pa_child || native_clock.rail_child)) {
        /* Ordering misuse is not a hardware fault. Do not restore clocks,
         * invoke drain or poison an otherwise valid in-flight pad owner. */
        lck_mtx_unlock(&x9_speaker_clock_lock);return EBUSY;
    }
    if(!e)e=x9_adsp_mt6993_speaker_halted_validate(ticket);
    /* This mandatory callback proves DMA stopped independently; idle() below
     * is only an enable-image check, not an authoritative drain substitute. */
    /* Log 276: acquisition failed (pll cold EBUSY) before configured, so the
     * AFE pointer was never handed out (clock_afe requires configured) and no
     * producer can exist; the drain callback then returned ENXIO and release
     * kept the lease, blocking the mic. Drain is required only once handed out. */
    if(!e && !native_clock.clocks_restored && native_clock.configured)e=native_clock.policy.validate_drained(
        native_clock.policy.context,ticket,0x1a110000,0x10000);
    if(!e && !native_clock.clocks_restored) {
        if(j.stuck || pj.stuck)e=EIO; /* unknown request: keep parents */
        if(!e && j.captured)e=idle();
        if(!e)e=native_tuner_restore();
        if(!e)e=pll_restore();
        if(!e)e=restore();
        if(!e)native_clock.clocks_restored=true;
    }
    if(!e)e=native_restored_ao();
    if(!e)e=x9_adsp_mt6993_clock_drop(ticket,native_clock.child);
    s.restore_error=pll_s.restore_error=(uint32_t)e;
    if(!e) {
        native_clock.child=0;native_clock.active=native_clock.configured=false;
        s.flags&=~X9ClockHeld;s.flags|=X9ClockRestored;s.ticket=0;s.stage=X9ClockDone;
        pll_s.flags&=~X9PLLHeld;pll_s.flags|=X9PLLRestored;pll_s.ticket=0;
        pll_s.stage=X9PLLDone;
        /* Historical error remains. Next flow needs new real generation. */
    } else if(native_clock.active)e=native_fault(e);
    lck_mtx_unlock(&x9_speaker_clock_lock);return e;
}
