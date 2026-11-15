/* Connect native config to the existing MT6993 hardware implementations.
 * No new ioctl, fake frame, reset, assumed grant or public-lock recursion.
 * Include after receiver.inc and before native sensor callbacks.
 */
#include "x9_camera_native_source.h"
static struct {
    bool attempted;
    struct x9_camera_native_source_status status;
} cam_native_source;

static int cam_native_source_standby_locked(uint64_t epoch)
{
    struct cam_sensor_ap ap;
    uint8_t byte;
    int e=cam_sensor_session_match(epoch);
    if(!e && cam_sensor_live.error)e=cam_sensor_live.error;
    if(!e && (cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED ||
        session.capture || session.stream))e=EBUSY;
    if(e)return e;
    ap=(struct cam_sensor_ap){&cam_journal.sensor,
        cam_sensor_live.probe.sensor==X9_CAMERA_BACK?0x1a:0x10};
    e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e && byte)e=EBUSY;
    if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e && byte)e=EBUSY;
    return e?e:cam_sensor_ap_guard(&ap);
}

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
}

/* Actual live.dts cdphy-dvfs-step0..3, not the driver's stale 356MHz
 * comment: 312/364/416/499MHz, 575/600/650/725mV. Both current preview
 * profiles fall below step0 data-rate limits. Accept only the corresponding
 * actual MM parent selector and BOTH freshly measured counts. Two counter
 * ticks are the original meter agreement bound, not a measured ppm claim.
 * Never change a PLL, shared AL mux, VCP DVFS or voltage request here.
 * required_vcore_uv is a REQUIREMENT, not voltage observed or applied.
 */
static int cam_native_source_rate_locked(uint64_t epoch)
{
    static const struct { uint32_t parent,mhz,uv; } steps[]={
        {9,312,575000},{12,364,600000},{14,416,650000},{15,499,725000}
    };
    struct x9_camera_native_source_status *s=&cam_native_source.status;
    const struct cam_mm_desc *d=&cam_mm.desc[X9_CAM_MM_SENINF_MUX];
    uint32_t parent;
    unsigned step;
    int e=cam_phy_dependencies();if(e)return e;
    if(!cam_phy.held || cam_phy.epoch!=epoch || cam_phy.error ||
       !(cam_phy.status.flags&X9_CAM_PHY_TIMING_CONFIGURED) ||
       cam_phy.status.clock_pending)return ESTALE;
    parent=(cam_rr(cam_mm.top,d->cfg)>>d->shift)&0xff;s->csi_parent=parent;
    if(parent&0xf0)return ENOTSUP;
    for(step=0;step<sizeof(steps)/sizeof(steps[0]);step++)if(parent==steps[step].parent)break;
    if(step==sizeof(steps)/sizeof(steps[0]))return ERANGE;
    s->inherited_step=step;s->required_vcore_uv=steps[step].uv;
    for(unsigned n=0;n<2;n++) {
        uint64_t actual=(uint64_t)cam_phy.status.clock_count[n]*26000000*2;
        uint64_t target=(uint64_t)steps[step].mhz*1000000*1024;
        uint64_t diff=actual>target?actual-target:target-actual;
        if(!cam_phy.status.clock_count[n] || diff>2ull*26000000*2)return ERANGE;
    }
    if((cam_phy.status.cphy && cam_phy.status.nominal_data_rate>=5400000000ull) ||
       (!cam_phy.status.cphy && cam_phy.status.nominal_data_rate>=9000000000ull))return ERANGE;
    /* Recheck the real retained source after both observations; an observed
     * rate still does not permit the capture authority to assert RATE/SMI. */
    e=cam_phy_dependencies();
    if(!e && ((cam_rr(cam_mm.top,d->cfg)>>d->shift)&0xff)!=parent)e=ESTALE;
    return e;
}

static int cam_native_source_prepare_locked(uint64_t epoch,
    struct x9_camera_native_source_status *result)
{
    struct x9_camera_receiver receiver={0};
    struct x9_camera_mac_status mac={0};
    struct x9_camera_phy_status phy={0};
    struct x9_camera_outmux_status outmux={0};
    struct x9_camera_native_source_status *s=&cam_native_source.status;
    int e;
    if(!result)return EINVAL;
    memset(result,0,sizeof(*result));result->sensor_epoch=epoch;
    e=cam_native_source_standby_locked(epoch);
    if(e){result->last_errno=e;return e;}
    if(cam_native_source.attempted && s->sensor_epoch==epoch) {
        *result=*s;return s->last_errno?s->last_errno:EALREADY;
    }
    /* A genuinely retired old epoch may be replaced, never an active or
     * partly configured receiver/SMI/PHY/OUTMUX from another lifetime. */
    if(cam_rx_live.epoch || cam_smi.held || cam_mac.held || cam_phy.held || cam_out.held) {
        result->last_errno=EBUSY;return EBUSY;
    }
    memset(s,0,sizeof(*s));s->sensor_epoch=epoch;s->sensor=cam_sensor_live.probe.sensor;
    s->inherited_step=UINT32_MAX;cam_native_source.attempted=true;
    s->stage=X9_CAM_NATIVE_SOURCE_RECEIVER;
    e=cam_rx_sensor_prepare_locked(epoch,&receiver);
    if(!e)e=cam_native_source_standby_locked(epoch);
    if(!e){s->completed|=1u<<s->stage;s->stage=X9_CAM_NATIVE_SOURCE_SMI;
        e=cam_smi_clocks_open_locked(epoch,s->sensor);}
    if(!e)e=cam_native_source_standby_locked(epoch);
    if(!e){s->completed|=1u<<s->stage;s->stage=X9_CAM_NATIVE_SOURCE_MAC;
        e=cam_mac_open_locked(epoch,&mac);}
    if(!e)e=cam_native_source_standby_locked(epoch);
    if(!e){s->completed|=1u<<s->stage;s->stage=X9_CAM_NATIVE_SOURCE_ANALOG;
        phy.sensor_epoch=epoch;phy.sensor=s->sensor;e=cam_phy_open_locked(epoch,&phy);}
    if(!e)e=cam_native_source_standby_locked(epoch);
    if(!e){s->completed|=1u<<s->stage;s->stage=X9_CAM_NATIVE_SOURCE_TIMING;
        e=cam_phy_timing_locked(epoch);}
    if(!e)e=cam_native_source_standby_locked(epoch);
    if(!e){s->completed|=1u<<s->stage;s->stage=X9_CAM_NATIVE_SOURCE_RATE;
        e=cam_native_source_rate_locked(epoch);}
    if(!e)e=cam_native_source_standby_locked(epoch);
    if(!e){s->completed|=1u<<s->stage;s->stage=X9_CAM_NATIVE_SOURCE_CTLE;
        e=cam_phy_ctle_locked(epoch);}
    if(!e)e=cam_native_source_standby_locked(epoch);
    if(!e){s->completed|=1u<<s->stage;s->stage=X9_CAM_NATIVE_SOURCE_OUTMUX;
        e=cam_out_open_locked(epoch,&outmux);}
    if(!e)e=cam_native_source_standby_locked(epoch);
    if(!e)e=cam_rx_sensor_guard_locked(epoch);
    if(!e)e=cam_smi_clock_guard();
    if(!e){s->completed|=1u<<s->stage;s->stage=X9_CAM_NATIVE_SOURCE_CONFIGURED;}
    else {
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
    printf("X9 CAMERA NATIVE CONFIG: sensor=%u epoch=%llu stage=%u completed=%x "
        "parent=%x counts=%u/%u CSI=%llu required_vcore=%u errno=%d (inactive; no DMA grant)\n",
        s->sensor,s->sensor_epoch,s->stage,s->completed,s->csi_parent,s->clock_count[0],
        s->clock_count[1],s->csi_clock_hz,s->required_vcore_uv,e);
    return e;
}
