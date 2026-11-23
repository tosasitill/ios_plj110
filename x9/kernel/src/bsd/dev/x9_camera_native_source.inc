/* Connect native config to the existing MT6993 hardware implementations.
 * No new ioctl, fake frame, assumed grant or public-lock recursion.
 * The normal ISP8s runtime-resume reset precedes all request/credit stores.
 * Include after receiver.inc and before native sensor callbacks.
 */
#include "x9_camera_native_source.h"
#include "x9_ufs/ufs_platform.h"
static struct {
    bool attempted;
    bool rate_valid;
    vm_offset_t pll;
    struct x9_camera_native_source_status status;
} cam_native_source;

static int cam_native_source_standby_locked_untimed(uint64_t epoch)
{
    struct cam_sensor_ap ap;
    uint8_t byte;
    int e=cam_sensor_session_match(epoch);
    if(!e && cam_sensor_live.error)e=cam_sensor_live.error;
    if(!e && (cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED ||
        session.capture || session.stream))e=CAM_FAIL(EBUSY);
    if(e)return e;
    ap=(struct cam_sensor_ap){&cam_journal.sensor,
        cam_sensor_slave(cam_sensor_live.probe.sensor)};
    e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e && byte)e=CAM_FAIL(EBUSY);
    if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e && byte)e=CAM_FAIL(EBUSY);
    return e?e:cam_sensor_ap_guard(&ap);
}
static int cam_native_source_standby_locked(uint64_t epoch)
{
    static unsigned hits;uint64_t t0=mach_absolute_time();
    int e=cam_native_source_standby_locked_untimed(epoch);
    cam_slow_note("cam_native_source_standby_locked",&hits,t0);
    return e;
}

#include "x9_camera_sv_reset.inc"

static void cam_native_source_snapshot_locked(void)
{
    struct x9_camera_native_source_status *s=&cam_native_source.status;
    if(cam_rx_sensor_held_locked(s->sensor_epoch))s->receiver_flags=cam_rx_live.status.flags;
    if(cam_smi.held && cam_smi.epoch==s->sensor_epoch) {
        s->smi_flags=cam_smi.status.flags;s->smi_a0_flags=cam_smi.status.a0.flags;
        s->smi_shared_flags=cam_smi.status.shared.flags;
    }
    if(cam_mac.held && cam_mac.epoch==s->sensor_epoch)s->mac_flags=cam_mac.status.flags;
    if(cam_phy.held && cam_phy.epoch==s->sensor_epoch) {
        s->phy_flags=cam_phy.status.flags;s->csi_clock_hz=cam_phy.status.csi_clock_hz;
        s->nominal_data_rate=cam_phy.status.nominal_data_rate;
        s->clock_count[0]=cam_phy.status.clock_count[0];
        s->clock_count[1]=cam_phy.status.clock_count[1];
    }
    if(cam_out.held && cam_out.epoch==s->sensor_epoch)s->outmux_flags=cam_out.status.flags;
    if(cam_native_bw.held && cam_native_bw.epoch==s->sensor_epoch) {
        s->average_bytes_sec=cam_native_bw.average;s->peak_bytes_sec=cam_native_bw.peak;
        memcpy(s->bandwidth_units,cam_native_bw.units,sizeof(s->bandwidth_units));
        memcpy(s->bandwidth_current,cam_native_bw.current,sizeof(s->bandwidth_current));
        s->bandwidth_ostd=cam_native_bw.ostd;
    }
}

/* Actual live.dts cdphy-dvfs-step0..3, not the driver's stale 356MHz
 * comment: 312/364/416/499MHz, 575/600/650/725mV. The current preview
 * profiles fall below step0 data-rate limits. Accept only the corresponding
 * actual MM parent selector and BOTH freshly measured counts. Two counter
 * ticks are the original meter agreement bound, not a measured ppm claim.
 * Native receiver open now cold-takes parent9 before any MM HWV vote;
 * the remaining table entries are only inherited-rate validation data.
 * The last clock is UNIVPLL2/5 =499.2MHz at its normal2496MHz rate;
 * DT represents the integralMHz only. Use actual PLL PCW, not499MHz as
 * the meter target. Never change a PLL, shared AL mux, VCP DVFS or voltage
 * request here. The native UFS supplier already holds a monotonic floor
 * >=650mV for this boot; require its real controller check, not a transient
 * high voltage gear. required_vcore_uv is not a PMIC measurement.
 */
static int cam_native_source_voltage_locked(void)
{
    struct x9_camera_native_source_status *s=&cam_native_source.status;
    uint32_t *v=s->vcore;
    if(!sweet_x9_vcore_ufs_check(v))return CAM_FAIL(ENXIO);
    /* The step index equals the regulator selector only for these four
     * supported CDPHY steps. UFS may retain a stronger monotonic floor. */
    if(((v[1]>>4)&7)<s->inherited_step ||
       ((v[2]>>8)&7)<s->inherited_step)return CAM_FAIL(ERANGE);
    return 0;
}
static uint64_t cam_native_source_pll_hz(uint32_t con1)
{
    uint64_t vco=(26000000ull*(con1&0x3fffffu)+0x3fffu)>>14;
    unsigned div=1u<<((con1>>24)&7);
    return (vco+div-1)/div;
}
/* No MM/SV recursion. Called only while the original receiver/PLL region
 * mappings are held, in thread context outside IRQ/slot/S1 spin locks.
 * Checks the actual inherited rate/floor; never changes them to repair drift.
 */
static int cam_native_source_rate_guard_locked(uint64_t epoch)
{
    struct x9_camera_native_source_status *s=&cam_native_source.status;
    const struct cam_mm_desc *d=&cam_mm.desc[X9_CAM_MM_SENINF_MUX];
    int e;
    if(s->last_errno)return s->last_errno;
    if(!cam_native_source.rate_valid || s->sensor_epoch!=epoch || !cam_native_source.pll ||
       !cam_rx_sensor_held_locked(epoch) || !cam_mm.active)return CAM_FAIL(ESTALE);
    if(((cam_rr(cam_mm.top,d->cfg)>>d->shift)&((1u<<d->width)-1u))!=s->csi_parent ||
       (cam_rr(cam_mm.top,0x330)&d->effect_mask) ||
       !(cam_rr(cam_mm.top,0x270)&(1u<<(14-cam_phy.status.csi_port))) ||
       (cam_rr(cam_native_source.pll,0x34)&0x10)!=s->pll_fenc ||
       (cam_rr(cam_native_source.pll,0xc)&0x073fffff)!=s->pll_rate)return CAM_FAIL(ESTALE);
    e=cam_native_rate_guard(&cam_mm,false);if(e)return e;
    if(!cam_native_rate.seen || !cam_native_rate.applied || cam_native_rate.level!=2 ||
       !s->cam_clock_hz || !(cam_rr(cam_mm.top,0x270)&(1u<<18)))return CAM_FAIL(ESTALE);
    e=cam_native_bw_guard();if(e)return e;
    e=cam_smi_a0_guard();if(e)return e;
    if(!cam_native_bw.held || !cam_native_bw.complete || cam_native_bw.epoch!=epoch ||
       (cam_smi_a0.expected[CAM_SMI_A0_OSTD]&0xffu)!=cam_native_bw.ostd || /* bit8 RO (373) */
       (cam_smi_a0.expected[CAM_SMI_A0_ULTRA]&1) ||
       (cam_smi_a0.expected[CAM_SMI_A0_NONSEC]&8))return CAM_FAIL(ESTALE);
    e=cam_native_source_voltage_locked();
    return e;
}
static int cam_native_source_processing_locked(void)
{
    struct x9_camera_native_source_status *s=&cam_native_source.status;
    uint64_t hz;
    int e=cam_native_rate_guard(&cam_mm,false);if(e)return e;
    if(!cam_native_rate.seen || !cam_native_rate.applied || cam_native_rate.level!=2)
        return CAM_FAIL(ENXIO);
    s->cam_parent=cam_native_rate.expected_cfg;s->cam_rc_level=cam_native_rate.rc_level;
    /* RC runs at max(all XPU votes) like Android: require at least our
     * level-2 rate (416 MHz), measured, not an exact parent/rate. */
    hz=cam_native_source_pll_hz(cam_native_rate.pcw)/6;
    {
        uint64_t sum=0;
        for(unsigned n=0;n<2;n++) {
            uint64_t actual,target;
            e=cam_phy_meter_measure(X9_CAM_MM_CAM_MUX,14,&s->cam_clock_count[n]);if(e)return e;
            actual=(uint64_t)s->cam_clock_count[n]*26000000*2;target=hz*1024;
            if(actual+2ull*26000000*2<target)return CAM_FAIL(ERANGE);
            sum+=actual/1024;
        }
        hz=sum/2;
    }
    e=cam_native_rate_guard(&cam_mm,false);if(e)return e;
    s->cam_clock_hz=hz;
    printf("X9 CAMERA CAM PROCESSING CLOCK: parent=%x RC=%x counts=%u/%u Hz=%llu "
        "(two fresh ID14 observations; bandwidth separately required)\n",
        s->cam_parent,s->cam_rc_level,s->cam_clock_count[0],s->cam_clock_count[1],hz);
    return 0;
}
static int cam_native_source_rate_locked(uint64_t epoch)
{
    static const struct { uint32_t parent,div,uv; uint64_t pll_base,hz; } steps[]={
        {9,8,575000,0x10043100,312000000},
        {12,6,600000,0x10043000,364000000},
        {14,6,650000,0x10043100,416000000},
        {15,5,725000,0x10043100,499200000}
    };
    struct x9_camera_native_source_status *s=&cam_native_source.status;
    const struct cam_mm_desc *d=&cam_mm.desc[X9_CAM_MM_SENINF_MUX];
    static vm_offset_t pll_maps[2];
    uint32_t parent;
    uint64_t clock,diff;
    unsigned step,pll_slot;
    int e=cam_phy_dependencies();if(e)return e;
    if(!cam_phy.held || cam_phy.epoch!=epoch || cam_phy.error ||
       !(cam_phy.status.flags&X9_CAM_PHY_TIMING_CONFIGURED) ||
       cam_phy.status.clock_pending)return CAM_FAIL(ESTALE);
    parent=(cam_rr(cam_mm.top,d->cfg)>>d->shift)&((1u<<d->width)-1u);s->csi_parent=parent;
    /* Android clk-mt6993.c mm_seninf*_sel: lsb 0, width 4 (sel only).
     * Log 377 refused raw byte 0x89 (sel 9 = 312 MHz) on bit7. */
    if((cam_rr(cam_mm.top,d->cfg)>>d->shift)&0xf0u)
        printf("X9 CAMERA SOURCE MUX: raw=%x sel=%x (upper bits outside Android width 4)\n",
            (cam_rr(cam_mm.top,d->cfg)>>d->shift)&0xffu,parent);
    for(step=0;step<sizeof(steps)/sizeof(steps[0]);step++)if(parent==steps[step].parent)break;
    if(step==sizeof(steps)/sizeof(steps[0]))return CAM_FAIL(ERANGE);
    s->inherited_step=step;s->required_vcore_uv=steps[step].uv;
    e=cam_native_source_voltage_locked();if(e)return e;
    pll_slot=steps[step].pll_base==0x10043000?0:1;
    if(!pll_maps[pll_slot])pll_maps[pll_slot]=ml_io_map(steps[step].pll_base,0x100);
    cam_native_source.pll=pll_maps[pll_slot];
    if(!cam_native_source.pll)return CAM_FAIL(ENOMEM);
    s->pll_rate=cam_rr(cam_native_source.pll,0xc)&0x073fffff;
    s->pll_fenc=cam_rr(cam_native_source.pll,0x34)&0x10;
    if(s->pll_fenc!=0x10)return CAM_FAIL(ENXIO);
    /* Borrow the existing normal PLL only. A valid unrelated rate cannot
     * be converted into the named CDPHY operating point by rounding. */
    clock=cam_native_source_pll_hz(s->pll_rate)/steps[step].div;
    diff=clock>steps[step].hz?clock-steps[step].hz:steps[step].hz-clock;
    if(diff>steps[step].hz/1000)return CAM_FAIL(ERANGE);
    for(unsigned n=0;n<2;n++) {
        uint64_t actual=(uint64_t)cam_phy.status.clock_count[n]*26000000*2;
        uint64_t target=clock*1024;
        uint64_t delta=actual>target?actual-target:target-actual;
        if(!cam_phy.status.clock_count[n] || delta>2ull*26000000*2)return CAM_FAIL(ERANGE);
    }
    if((cam_phy.status.cphy && cam_phy.status.nominal_data_rate>=5400000000ull) ||
       (!cam_phy.status.cphy && cam_phy.status.nominal_data_rate>=9000000000ull))return CAM_FAIL(ERANGE);
    /* Recheck the real retained source after both observations; an observed
     * rate still does not permit the capture authority to assert RATE/SMI. */
    e=cam_native_source_processing_locked();
    if(!e)e=cam_native_bw_take_locked(epoch);
    if(!e)e=cam_phy_dependencies();
    if(!e && ((cam_rr(cam_mm.top,d->cfg)>>d->shift)&((1u<<d->width)-1u))!=parent)e=CAM_FAIL(ESTALE);
    if(!e) {
        cam_native_source.rate_valid=true;e=cam_native_source_rate_guard_locked(epoch);
        if(e)cam_native_source.rate_valid=false;
    }
    return e;
}

static int cam_native_source_config_log(const struct x9_camera_native_source_status *s,
    const char *step,int error)
{
    printf("X9 CAMERA NATIVE CONFIG: sensor=%u epoch=%llu stage=%u completed=%x "
        "parent=%x counts=%u/%u CSI=%llu required_vcore=%u errno=%d step=%s\n",
        s->sensor,s->sensor_epoch,s->stage,s->completed,s->csi_parent,s->clock_count[0],
        s->clock_count[1],s->csi_clock_hz,s->required_vcore_uv,error,step);
    return error;
}

static int cam_native_source_prepare_locked(uint64_t epoch,
    struct x9_camera_native_source_status *result)
{
    struct x9_camera_receiver receiver={0};
    struct x9_camera_mac_status mac={0};
    struct x9_camera_phy_status phy={0};
    struct x9_camera_outmux_status outmux={0};
    struct x9_camera_native_source_status *s=&cam_native_source.status;
    const char *step;
    int e;
    if(!result)return CAM_FAIL(EINVAL);
    memset(result,0,sizeof(*result));result->sensor_epoch=epoch;
    e=cam_native_source_standby_locked(epoch);
    if(e) {
        result->last_errno=e;
        return cam_native_source_config_log(result,"entry-standby",e);
    }
    if(cam_native_source.attempted && s->sensor_epoch==epoch) {
        *result=*s;
        return cam_native_source_config_log(result,"already-attempted",
            s->last_errno?s->last_errno:EALREADY);
    }
    /* A genuinely retired old epoch may be replaced, never an active or
     * partly configured receiver/SMI/PHY/OUTMUX from another lifetime. */
    if(cam_rx_live.epoch || cam_smi.held || cam_mac.held || cam_phy.held || cam_out.held) {
        result->last_errno=EBUSY;
        return cam_native_source_config_log(result,"lifetime-busy",EBUSY);
    }
    memset(s,0,sizeof(*s));cam_native_source.rate_valid=false;
    s->sensor_epoch=epoch;s->sensor=cam_sensor_live.probe.sensor;
    s->inherited_step=UINT32_MAX;cam_native_source.attempted=true;
    s->stage=X9_CAM_NATIVE_SOURCE_RECEIVER;
    step="clock-request";
    e=cam_native_clock_request_locked(epoch,s->sensor);
    if(!e) {
        step="receiver-open";
        e=cam_rx_sensor_prepare_locked(epoch,&receiver);
    }
    cam_native_clock_cancel_unused_locked();
    if(!e) {
        step="standby";
        e=cam_native_source_standby_locked(epoch);
    }
    if(!e) {
        s->completed|=1u<<s->stage;
        s->stage=X9_CAM_NATIVE_SOURCE_SMI;
        step="smi-clocks-open";
        e=cam_smi_clocks_open_locked(epoch,s->sensor);
    }
    if(!e) {
        step="standby";
        e=cam_native_source_standby_locked(epoch);
    }
    if(!e) {
        step="camsv-cold-reset";
        e=cam_sv_cold_reset_locked(epoch);
    }
    if(!e) {
        s->completed|=1u<<s->stage;
        s->stage=X9_CAM_NATIVE_SOURCE_MAC;
        step="mac-open";
        e=cam_mac_open_locked(epoch,&mac);
    }
    if(!e) {
        step="standby";
        e=cam_native_source_standby_locked(epoch);
    }
    if(!e) {
        s->completed|=1u<<s->stage;
        s->stage=X9_CAM_NATIVE_SOURCE_ANALOG;
        step="phy-open";
        phy.sensor_epoch=epoch;
        phy.sensor=s->sensor;
        e=cam_phy_open_locked(epoch,&phy);
    }
    if(!e) {
        step="standby";
        e=cam_native_source_standby_locked(epoch);
    }
    if(!e) {
        s->completed|=1u<<s->stage;
        s->stage=X9_CAM_NATIVE_SOURCE_TIMING;
        step="phy-timing";
        e=cam_phy_timing_locked(epoch);
    }
    if(!e) {
        step="standby";
        e=cam_native_source_standby_locked(epoch);
    }
    if(!e) {
        s->completed|=1u<<s->stage;
        s->stage=X9_CAM_NATIVE_SOURCE_RATE;
        step="source-rate";
        e=cam_native_source_rate_locked(epoch);
    }
    if(!e) {
        step="standby";
        e=cam_native_source_standby_locked(epoch);
    }
    if(!e) {
        s->completed|=1u<<s->stage;
        s->stage=X9_CAM_NATIVE_SOURCE_CTLE;
        step="phy-ctle";
        e=cam_phy_ctle_locked(epoch);
    }
    if(!e) {
        step="standby";
        e=cam_native_source_standby_locked(epoch);
    }
    if(!e) {
        s->completed|=1u<<s->stage;
        s->stage=X9_CAM_NATIVE_SOURCE_OUTMUX;
        step="outmux-open";
        e=cam_out_open_locked(epoch,&outmux);
    }
    if(!e) {
        step="standby";
        e=cam_native_source_standby_locked(epoch);
    }
    if(!e) {
        step="receiver-guard";
        e=cam_rx_sensor_guard_locked(epoch);
    }
    if(!e) {
        step="smi-guard";
        e=cam_smi_clock_guard();
    }
    if(!e) {
        step="rate-guard";
        e=cam_native_source_rate_guard_locked(epoch);
    }
    if(!e) {
        s->completed|=1u<<s->stage;
        s->stage=X9_CAM_NATIVE_SOURCE_CONFIGURED;
        step="configured";
    }
    else {
        cam_native_source.rate_valid=false;
        /* Preserve the original journals for the sole native power-off.
         * No local unwind can retire a posted meter/calibration/DB request. */
        if(cam_out.held)cam_out_fault(e);
        if(cam_phy.held)cam_phy_fault(e);
        if(cam_mac.held)cam_mac_fault(e);
        if(cam_smi.held)cam_smi_fault(e);
        if(cam_rx_sensor_held_locked(epoch))cam_rx_session_fault(e);
        cam_sensor_session_fault(e);
    }
    s->last_errno=e;cam_native_source_snapshot_locked();*result=*s;
    return cam_native_source_config_log(s,step,e);
}
