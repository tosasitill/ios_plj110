/* ISP8s mtk_camsv_runtime_resume -> sv_reset_by_camsys_top:
 * after larb/clocks, before configuration/IRQ publication, CQ stop0/1,
 * ACK bit1 (100ms), stop0, then MRAW SW_RST_1: 0 -> 3<<(id*2+4) -> 0.
 * Live DT CAMSV id0, SCQ3a602000, mtk_cam-sv-regs.h MRAW3a680000/+a4.
 * This is a COLD initialization, never the natural-DONE retirement path.
 * Original RX samples and pre/post-reset samples remain separate evidence.
 * No current request, credit or source may exist at admission. Backing from
 * previous fully retired epochs remains pinned with its archived history.
 */
static struct {
    uint64_t epoch;
    unsigned sensor,issued,cq_samples,post_samples;
    uint32_t cq_before[2],cq_actual[2],cq_ack,top_before,top_actual;
    uint32_t outstanding[7][2];
    bool attempted,complete;
    bool inherited_logged,outstanding_logged;
    int error;
    struct x9_camera_receiver_sample before,after,post[2];
} cam_sv_reset;

static int cam_sv_cold_reset_retirement_guard(uint64_t epoch)
{
    if(!cam_sv_reset.attempted || cam_sv_reset.epoch!=epoch)return 0;
    /* A partially issued reset is not canceled by power-off or a new epoch. */
    return cam_sv_reset.error?cam_sv_reset.error:
        cam_sv_reset.complete?0:EBUSY;
}
static int cam_sv_cold_reset_guard(uint64_t epoch)
{
    const struct cam_rx_maps *m=&cam_rx_maps[cam_sv_reset.sensor-1];
    struct x9_camera_receiver_sample current,non_sv;
    static const struct {unsigned larb,port;} ports[]={
        {0,2},{0,0},{0,3},{1,0},{1,3},{2,0},{2,3}
    };
    static const unsigned off[]={0x344,0x34c,0x35c,0x364,0x36c,
        0x1d0,0x1d4,0x1d8,0x228,0x240,0x244,0x248,0x24c,0x250,
        0x1020,0x2014,0x2018};
    int e;
    if(epoch!=cam_sv_reset.epoch || !cam_sv_reset.attempted || cam_sv_reset.complete ||
       !cam_rx_sensor_held_locked(epoch) || cam_rx_live.close_attempted ||
       !cam_smi.held || cam_smi.epoch!=epoch || cam_smi.completed!=3 ||
       cam_smi.close_attempted || cam_smi.error || cam_sv.held ||
       cam_sv.request_held || (cam_sv.credit_issued && !cam_sv.credit_retired) ||
       cam_mac.held || cam_phy.held || cam_out.held)return CAM_FAIL(ESTALE);
    e=cam_native_source_standby_locked(epoch);if(e)return e;
    e=cam_irq_cold_reset_guard_locked(epoch,m);if(e)return e;
    e=cam_rx_sample(m,&current);if(e)return e;
    cam_sv_reset.after=current;
    if(cam_rx_busy(&current,cam_sensor_port(cam_sv_reset.sensor),0,0,0,0,0,0))return CAM_FAIL(EBUSY);
    /* Only CAMSV0 is reset. Never adopt a changed CSI/MAC/OUTMUX/async
     * input as a side effect of resetting this downstream engine. */
    non_sv=current;memcpy(non_sv.sv,cam_sv_reset.before.sv,sizeof(non_sv.sv));
    if(cam_rx_changed(&cam_sv_reset.before,&non_sv,cam_sensor_port(cam_sv_reset.sensor)))
        return CAM_FAIL(ESTALE);
    for(unsigned b=0;b<2;b++) {
        e=cam_mm_guard(&cam_mm,true);if(e)return e;
        e=cam_smi_clock_guard();if(e)return e;
        /* No IRQ/R-C status reads. Source enables, DMA/STG stops and
         * CQ thread0 remain inactive. CQ stop is the sole live command. */
        for(unsigned n=0;n<sizeof(off)/sizeof(off[0]);n++)
            {
                /* Android sv_reset_by_camsys_top has no pre-state check. */
                uint32_t v=cam_rr(m->sv[b],off[n]);
                if(v && !cam_sv_reset.inherited_logged) {
                    cam_sv_reset.inherited_logged=true;
                    printf("X9 CAMERA CAMSV RESET INHERITED: bank=%u off=%x value=%x (Android resets unconditionally)\n",
                        b,off[n],v);
                }
            }
    }
    for(unsigned n=0;n<sizeof(ports)/sizeof(ports[0]);n++) {
        vm_offset_t larb=cam_smi.larb[ports[n].larb];
        unsigned port=ports[n].port;
        e=cam_smi_clock_guard();if(e)return e;
        if(!larb)return CAM_FAIL(ENXIO);
        cam_sv_reset.outstanding[n][0]=cam_rr(larb,0x280+4*port);
        cam_sv_reset.outstanding[n][1]=cam_rr(larb,0x780+4*port);
        if((cam_sv_reset.outstanding[n][0] || cam_sv_reset.outstanding[n][1]) &&
           !cam_sv_reset.outstanding_logged) {
            cam_sv_reset.outstanding_logged=true;
            printf("X9 CAMERA CAMSV RESET INHERITED: larb=%u port=%u ostd=%x/%x (Android does not check)\n",
                ports[n].larb,port,cam_sv_reset.outstanding[n][0],cam_sv_reset.outstanding[n][1]);
        }
    }
    return cam_irq_cold_reset_guard_locked(epoch,m);
}
static int cam_sv_cold_reset_locked(uint64_t epoch)
{
    const struct cam_rx_maps *m=NULL;
    uint64_t span,end;
    int e;
    if(cam_sv_reset.attempted && cam_sv_reset.epoch==epoch)
        return cam_sv_reset.error?cam_sv_reset.error:EALREADY;
    if(cam_sv_reset.attempted && !cam_sv_reset.complete)
        printf("X9 CAMERA CAMSV RESET ADOPT: previous epoch=%llu incomplete errno=%d issued=%x; retrying (Android resets every resume)\n",
            cam_sv_reset.epoch,cam_sv_reset.error,cam_sv_reset.issued);
    memset(&cam_sv_reset,0,sizeof(cam_sv_reset));
    cam_sv_reset.attempted=true;cam_sv_reset.epoch=epoch;
    cam_sv_reset.sensor=cam_sensor_live.probe.sensor;
    if(!x9_camera_sensor_valid(cam_sv_reset.sensor)){e=CAM_FAIL(EINVAL);goto out;}
    m=&cam_rx_maps[cam_sv_reset.sensor-1];
    e=cam_rx_session_guard();if(e)goto out;
    e=cam_rx_sample(m,&cam_sv_reset.before);if(e)goto out;
    e=cam_sv_cold_reset_guard(epoch);if(e)goto out;
    for(unsigned b=0;b<2;b++) {
        cam_sv_reset.cq_before[b]=cam_rr(m->sv[b],0x2010);
    }
    cam_sv_reset.top_before=cam_rr(cam_mm.sv_cg,0xa4);
    if(cam_sv_reset.cq_before[0] || cam_sv_reset.cq_before[1] || cam_sv_reset.top_before)
        printf("X9 CAMERA CAMSV RESET INHERITED: cq=%x/%x mraw_rst=%x (Android overwrites)\n",
            cam_sv_reset.cq_before[0],cam_sv_reset.cq_before[1],cam_sv_reset.top_before);
    nanoseconds_to_absolutetime(100000000,&span);end=mach_absolute_time()+span;
    /* Keep the upstream zero writes even with a zero prior. These are
     * command edges, not ordinary geometry fields or DB transactions.
     * Every store is journaled first and is issued once; no timeout retry. */
    for(unsigned step=0;step<2;step++) {
        e=cam_sv_cold_reset_guard(epoch);if(e)goto out;
        cam_sv_reset.issued|=1u<<step;
        cam_rw(m->sv[0],0x2010,step);
    }
    for(unsigned poll=0;poll<100000;poll++) {
        if(mach_absolute_time()>=end){e=CAM_FAIL(ETIMEDOUT);goto out;}
        e=cam_sv_cold_reset_guard(epoch);if(e)goto out;
        cam_sv_reset.cq_actual[0]=cam_rr(m->sv[0],0x2010);
        if(mach_absolute_time()>=end){e=CAM_FAIL(ETIMEDOUT);goto out;}
        cam_sv_reset.cq_samples=(cam_sv_reset.cq_actual[0]&2)?
            cam_sv_reset.cq_samples+1:0;
        if(cam_sv_reset.cq_samples==2) {
            cam_sv_reset.cq_ack=cam_sv_reset.cq_actual[0];break;
        }
        delay(1);
    }
    if(cam_sv_reset.cq_samples!=2){e=CAM_FAIL(ETIMEDOUT);goto out;}
    e=cam_sv_cold_reset_guard(epoch);if(e)goto out;
    cam_sv_reset.issued|=1u<<2;cam_rw(m->sv[0],0x2010,0);
    e=cam_sv_cold_reset_guard(epoch);if(e)goto out;
    cam_sv_reset.top_actual=cam_rr(cam_mm.sv_cg,0xa4);
    /* Keep the upstream pulse contiguous. In particular, do not perform
     * sensor I2C or read a CAMSV engine while its reset is asserted. */
    for(unsigned step=0;step<3;step++) {
        cam_sv_reset.issued|=1u<<(3+step);
        cam_rw(cam_mm.sv_cg,0xa4,step==1?0x30:0);
        /* cam_rw includes dsb sy. Do not invent an ACK for the reset
         * pulse: upstream polls only CQ stop, then completes this pulse. */
    }
    /* A reset establishes new configuration defaults. Only twice-observed
     * inactive SV values become the next configuration journal's baseline;
     * original before, both post samples and old retired epochs are retained. */
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_sv_cold_reset_guard(epoch);if(e)goto out;
        cam_sv_reset.post[sample]=cam_sv_reset.after;
        for(unsigned b=0;b<2;b++) {
            cam_sv_reset.cq_actual[b]=cam_rr(m->sv[b],0x2010);
        }
        cam_sv_reset.top_actual=cam_rr(cam_mm.sv_cg,0xa4);
        /* First sample is after the pulse; stability is required between
         * the two post samples, not against a pre-reset default. */
        if(sample && memcmp(cam_sv_reset.post[0].sv,cam_sv_reset.post[1].sv,
            sizeof(cam_sv_reset.post[0].sv))) {
            printf("X9 CAMERA CAMSV RESET ADOPT: post-reset SV sample changed; using second (cq=%x/%x top=%x)\n",
                cam_sv_reset.cq_actual[0],cam_sv_reset.cq_actual[1],cam_sv_reset.top_actual);
        }
        cam_sv_reset.post_samples++;
        if(!sample)delay(10);
    }
    memcpy(cam_rx_live.expected.sv,cam_sv_reset.post[1].sv,sizeof(cam_rx_live.expected.sv));
    cam_sv_reset.complete=true;
    e=cam_rx_session_guard();
out:
    cam_sv_reset.error=e;
    if(e && m) /* Android sv_reset "cq dma timeout" dump + gate state */
        printf("X9 CAMERA CAMSV RESET DUMP: sen_mode=%x cq_ctl=%x gals_dbg=%x ppc_rdy=%x/%x "
            "cam_m0/m1=%x/%x r1a0/r1a1=%x/%x mraw=%x larb19 stat=%x ostd=%x/%x\n",
            cam_rr(m->sv[0],0x140),cam_rr(m->sv[0],0x2010),cam_rr(cam_mm.cam_cg,0x414),
            cam_rr(cam_mm.cam_cg,0x588),cam_rr(cam_mm.cam_cg,0x58c),
            cam_rr(cam_mm.cam_cg,0),cam_rr(cam_mm.cam_cg,0x4c),
            cam_rr(cam_smi.gate[0].cg,0),cam_rr(cam_smi.gate[0].cg,0x10),cam_rr(cam_mm.sv_cg,0),
            cam_rr(cam_smi.larb[0],0),cam_rr(cam_smi.larb[0],0x280),cam_rr(cam_smi.larb[0],0x780));
    printf("X9 CAMERA CAMSV COLD RESET: sensor=%u epoch=%llu issued=%x cq=%x/%x "
        "ack=%x ack_samples=%u top=%x/%x post_samples=%u complete=%u errno=%d "
        "(ISP8s resume; before request/credit/source)\n",
        cam_sv_reset.sensor,epoch,cam_sv_reset.issued,cam_sv_reset.cq_actual[0],
        cam_sv_reset.cq_actual[1],cam_sv_reset.cq_ack,cam_sv_reset.cq_samples,cam_sv_reset.top_before,
        cam_sv_reset.top_actual,cam_sv_reset.post_samples,cam_sv_reset.complete,e);
    return e;
}
