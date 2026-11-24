/* Final ISP8s sv_reset(), mtk_cam-sv.c:797..969. This runs only AFTER
 * natural RAW/worker/GIC/credit/translation retirement, complete ordinary
 * SV restoration and physical source isolation/restoration. It does not
 * create any of those receipts. Old FBC/DMA journals are frozen; new reset
 * observations live here and are separately archived with the same epoch.
 * Control lock held; no IRQ owner/worker/request can run during this phase.
 */
static struct {
    uint64_t epoch,issued;
    unsigned sensor,phase,samples[3],post_samples;
    uint32_t ack[3],actual[3],fbc_before[2],fbc_after[2];
    uint32_t input[21],output[21],outstanding[7][2];
    uint32_t post_regs[2][2][CAM_SV_MAX_REGS];
    struct x9_camera_receiver_sample before,after,post[2];
    bool attempted,complete;
    int error;
} cam_sv_shutdown;
static const unsigned cam_sv_shutdown_stop[3]={0x1020,0x250,0x2010};

static int cam_sv_shutdown_guard(uint64_t epoch)
{
    struct x9_camera_receiver_sample current,non_sv;
    const struct cam_rx_maps *m=&cam_rx_maps[cam_sv_shutdown.sensor-1];
    static const struct {unsigned larb,port;} ports[]={
        {0,2},{0,0},{0,3},{1,0},{1,3},{2,0},{2,3}
    };
    static const unsigned disabled[]={0x344,0x34c,0x35c,0x364,0x36c,
        0x1d4,0x1d8,0x228,0x240,0x244,0x248,0x24c,0x2014,0x2018};
    int e;
    if(epoch!=cam_sv_shutdown.epoch || epoch!=cam_sv.epoch ||
       epoch!=cam_sensor_live.epoch || !cam_sensor_live.close_attempted ||
       cam_sensor_live.phase!=CAM_SENSOR_CLOSING ||
       !cam_rx_live.close_attempted || !cam_rx_sensor_held_locked(epoch) ||
       !cam_sv_controls_restored() || cam_mac.held || cam_phy.held || cam_out.held ||
       !cam_smi.held || cam_smi.epoch!=epoch || cam_smi.close_attempted ||
       cam_smi.completed!=3 || cam_smi.error)return CAM_FAIL(ESTALE);
    /* The parent close already asserted reset and disconnected MCLK.
     * Sensor I2C is no longer valid here; prove the real pin isolation,
     * not a second AP read after cam_sensor_live entered CLOSING. */
    e=cam_pins_guard(&cam_journal,true);if(e)return e;
    if(!cam_journal.pins_owned || cam_pin_mode(cam_journal.gpio,cam_journal.reset.pin) ||
       !cam_pin_get(cam_journal.gpio,0,cam_journal.reset.pin) ||
       cam_pin_get(cam_journal.gpio,0x100,cam_journal.reset.pin) ||
       cam_pin_mode(cam_journal.gpio,cam_journal.mclk.pin))return CAM_FAIL(EBUSY);
    e=cam_native_sensor_retirement_guard_locked(epoch);if(e)return e;
    e=cam_irq_cold_reset_guard_locked(epoch,m);if(e)return e;
    e=cam_rx_sample(m,&current);if(e)return e;
    cam_sv_shutdown.after=current;
    if(cam_rx_busy(&current,cam_sensor_port(cam_sv_shutdown.sensor),0,0,0,0,0,0))return CAM_FAIL(EBUSY);
    non_sv=current;memcpy(non_sv.sv,cam_sv_shutdown.before.sv,sizeof(non_sv.sv));
    if(cam_rx_changed(&cam_sv_shutdown.before,&non_sv,cam_sensor_port(cam_sv_shutdown.sensor)))
        return CAM_FAIL(ESTALE);
    for(unsigned bank=0;bank<2;bank++) {
        e=cam_sv_power_guard();if(e)return e;
        /* diag30: Android sv_reset never checks these. After a reset STG
         * 0x240/0x244/0x248 keep 0x5f3700/0x105f3700/0x205f3700 (start logs
         * them as CAMSV INHERITED); refusing made every close EBUSY. Log only. */
        for(unsigned n=0;n<sizeof(disabled)/sizeof(disabled[0]);n++) {
            uint32_t v=cam_rr(m->sv[bank],disabled[n]);
            static unsigned logs;
            if(v && logs<16){logs++;printf("X9 CAMERA CAMSV SHUTDOWN INHERITED: bank=%u off=%x value=%x (Android does not check)\n",bank,disabled[n],v);}
        }
    }
    for(unsigned n=0;n<sizeof(ports)/sizeof(ports[0]);n++) {
        vm_offset_t larb=cam_smi.larb[ports[n].larb];
        e=cam_smi_clock_guard();if(e)return e;
        if(!larb)return CAM_FAIL(ENXIO);
        cam_sv_shutdown.outstanding[n][0]=cam_rr(larb,0x280+4*ports[n].port);
        cam_sv_shutdown.outstanding[n][1]=cam_rr(larb,0x780+4*ports[n].port);
        /* Android sv_reset never checks larb OSTD; values stay in the journal. */
    }
    return cam_irq_cold_reset_guard_locked(epoch,m);
}
static void cam_sv_shutdown_store(unsigned step,unsigned bank,unsigned offset,uint32_t value)
{
    /* All call sites are the fixed, single upstream sequence below.
     * Store history precedes each possibly posted operation; no retry. */
    cam_sv_shutdown.output[step]=value;cam_sv_shutdown.issued|=1ull<<step;
    cam_rw(cam_sv.view[bank],offset,value);
}
static int cam_sv_shutdown_admit(uint64_t epoch)
{
    boolean_t old;
    int e=cam_native_sensor_retirement_guard_locked(epoch);
    if(!e)e=cam_rx_session_guard();if(e)return e;
    lck_mtx_lock(&camera_irq_lock);
    old=cam_irq_spin_take();
    if(cam_irq.prepared || cam_irq.owns || cam_irq.accepting || cam_irq.busy ||
       cam_irq.count || cam_irq.error || (cam_irq.epoch && cam_irq.epoch!=epoch))e=CAM_FAIL(ESTALE);
    if(!e && cam_sv.credit_issued) {
        if(!cam_sv.credit_retired || !cam_irq.close_attempted || !cam_irq_fbc.bound ||
           !cam_repeat_retirement_receipt_locked() || cam_irq_fbc.error ||
           cam_irq_fbc.source_error || cam_irq_fbc.epoch!=epoch ||
           cam_irq_fbc.source_phase!=CAM_STREAM_STOPPED ||
           cam_irq_fbc.source_issued!=(CAM_STREAM_ON|CAM_STREAM_OFF) ||
           cam_irq_fbc.source_ack!=(CAM_STREAM_ON|CAM_STREAM_OFF) ||
           cam_source.phase!=CAM_SOURCE_STOPPED || cam_source.epoch!=epoch || cam_source.error ||
           cam_dma_stop.phase!=CAM_DMA_STOP_RESTORED || cam_dma_stop.epoch!=epoch ||
           cam_dma_stop.error || !cam_stream_close_restore_ready())e=CAM_FAIL(ESTALE);
        for(unsigned bank=0;bank<2 && !e;bank++) {
            uint32_t ctl,cnt;
            e=cam_irq_power();if(e)break; /* non-sleeping under IRQ spin */
            ctl=cam_rr(cam_sv.view[bank],0x540);cnt=cam_rr(cam_sv.view[bank],0x544);
            e=cam_irq_fbc_note_locked(bank,ctl,cnt);
            cam_sv_shutdown.fbc_before[bank]=cnt;
            if(!e && (cnt!=cam_sv.credit_spent_count[bank] || (cnt&CAM_IRQ_FBC_CNT)))e=CAM_FAIL(ESTALE);
        }
    } else if(!e && (cam_source.captured || cam_irq_fbc.bound || cam_irq_fbc.source_issued))e=CAM_FAIL(ESTALE);
    cam_irq_spin_drop(old);
    if(!e) {
        struct x9_camera_camsv_slot *slot=cam_irq.slot;
        lck_mtx_lock(&camera_camsv_lock);
        if(cam_sv.credit_issued && (!slot || slot->last_errno ||
           (slot->state!=X9_CAMSV_RETIRED && slot->state!=X9_CAMSV_CANCELLED) ||
           memcmp(&slot->request,&cam_sv.request,sizeof(cam_sv.request)) ||
           slot->dma.backing!=cam_sv.request_dma.backing))e=CAM_FAIL(ESTALE);
        if(!e && cam_sv.credit_issued) {
            struct x9_camera_s1 *map=slot->dma.backing;
            lck_mtx_lock(&camera_s1_lock);
            e=map?cam_s1_check_private(map):ESTALE;
            if(!e && (map->raw_errno ||
               map->raw_state!=(slot->state==X9_CAMSV_CANCELLED?CAM_RAW_CANCELLED:CAM_RAW_IDLE) ||
               !cam_s1_dma_match(map,slot)))e=CAM_FAIL(ESTALE);
            lck_mtx_unlock(&camera_s1_lock);
        }
        lck_mtx_unlock(&camera_camsv_lock);
    }
    lck_mtx_unlock(&camera_irq_lock);return e;
}
static int cam_sv_shutdown_locked(uint64_t epoch)
{
    const struct cam_rx_maps *m;
    int e;
    /* ID-only/diagnostic lifetimes never issued these native SW requests. */
    if(!cam_sv_qos.captured || cam_sv_qos.epoch!=epoch || !cam_sv_qos.issued)return 0;
    if(cam_sv_shutdown.attempted && cam_sv_shutdown.epoch==epoch)
        return cam_sv_shutdown.error?cam_sv_shutdown.error:ESTALE;
    if(cam_sv_shutdown.attempted && !cam_sv_shutdown.complete)
        printf("X9 CAMERA CAMSV SHUTDOWN ADOPT: previous epoch=%llu incomplete phase=%u errno=%d; retrying\n",
            cam_sv_shutdown.epoch,cam_sv_shutdown.phase,cam_sv_shutdown.error);
    memset(&cam_sv_shutdown,0,sizeof(cam_sv_shutdown));
    cam_sv_shutdown.attempted=true;cam_sv_shutdown.epoch=epoch;
    cam_sv_shutdown.sensor=cam_sv.status.sensor;
    if(!x9_camera_sensor_valid(cam_sv_shutdown.sensor) || !cam_sv_qos.configured ||
       cam_sv_qos.error || cam_sv_qos.retired){e=CAM_FAIL(ESTALE);goto out;}
    m=&cam_rx_maps[cam_sv_shutdown.sensor-1];
    e=cam_sv_shutdown_admit(epoch);if(e)goto out;
    e=cam_rx_sample(m,&cam_sv_shutdown.before);if(e)goto out;
    e=cam_sv_shutdown_guard(epoch);if(e)goto out;
    for(unsigned b=0;b<2;b++) {
        for(unsigned n=0;n<3;n++)
        {
            /* diag30: a previous reset leaves its ACK (2) in SW_RST_CTL; the
             * sequence below writes 0 before issuing the reset, as sv_reset does.
             * Only a reset still in progress (any other bit) refuses. */
            uint32_t v=cam_rr(m->sv[b],cam_sv_shutdown_stop[n]);
            if(v&~2u){e=CAM_FAIL(EBUSY);goto out;}
            if(v)printf("X9 CAMERA CAMSV SHUTDOWN INHERITED: bank=%u stop=%x value=%x (prior reset ACK)\n",b,cam_sv_shutdown_stop[n],v);
        }
        for(unsigned n=0;n<CAM_SV_QOS_COUNT;n++) {
            bool allowed=false;
            e=cam_sv_power_guard();if(e)goto out;
            e=cam_sv_qos_observe(b,cam_sv_qos_offsets[n],
                cam_rr(m->sv[b],cam_sv_qos_offsets[n]),&allowed);if(e)goto out;
        }
    }
    if(cam_rr(m->sv[0],0x7c)>1 || cam_rr(m->sv[0],0x1d0)){e=CAM_FAIL(EBUSY);goto out;}
    cam_sv_shutdown.phase=1;
    /* common_disable inhibits DMA DCM before sv_reset. */
    e=cam_sv_shutdown_guard(epoch);if(e)goto out;
    cam_sv_shutdown.input[0]=cam_rr(m->sv[0],0x7c);
    cam_sv_shutdown_store(0,0,0x7c,1);
    for(unsigned step=1;step<=5;step++) {
        unsigned bank=step>=3?1:0,offset=step==5?0x2100:0x140;
        unsigned mask=step==5?1u<<21:(1u<<((step&1)?16:18));
        uint32_t value;
        e=cam_sv_shutdown_guard(epoch);if(e)goto out;
        value=cam_rr(m->sv[bank],offset);cam_sv_shutdown.input[step]=value;
        cam_sv_shutdown_store(step,bank,offset,value&~mask);
    }
    /* sv_reset stop writes: DMA0/CQ0/STG0, DMA1/STG1/CQ1. */
    for(unsigned step=6;step<12;step++) {
        static const unsigned offsets[]={0x1020,0x2010,0x250,0x1020,0x250,0x2010};
        e=cam_sv_shutdown_guard(epoch);if(e)goto out;
        cam_sv_shutdown_store(step,0,offsets[step-6],step>=9?1:0);
    }
    for(unsigned role=0;role<3;role++) {
        uint64_t span,end;
        nanoseconds_to_absolutetime(CAM_GUARD_BUDGET_NS,&span);end=mach_absolute_time()+span;
        for(unsigned poll=0;poll<100000;poll++) {
            if(mach_absolute_time()>=end){e=CAM_FAIL(ETIMEDOUT);goto out;}
            e=cam_sv_shutdown_guard(epoch);if(e)goto out;
            cam_sv_shutdown.actual[role]=cam_rr(m->sv[0],cam_sv_shutdown_stop[role]);
            if(mach_absolute_time()>=end){e=CAM_FAIL(ETIMEDOUT);goto out;}
            if(cam_sv_shutdown.actual[role]&~3u){e=CAM_FAIL(ESTALE);goto out;}
            cam_sv_shutdown.samples[role]=(cam_sv_shutdown.actual[role]&2)?
                cam_sv_shutdown.samples[role]+1:0;
            if(cam_sv_shutdown.samples[role]==2) {
                cam_sv_shutdown.ack[role]=cam_sv_shutdown.actual[role];break;
            }
            delay(1);
        }
        if(cam_sv_shutdown.samples[role]!=2){e=CAM_FAIL(ETIMEDOUT);goto out;}
    }
    e=cam_sv_shutdown_guard(epoch);if(e)goto out;
    for(unsigned role=0;role<3;role++)
        if(!(cam_rr(m->sv[0],cam_sv_shutdown_stop[role])&2)){e=CAM_FAIL(ESTALE);goto out;}
    cam_sv_shutdown.phase=2;
    /* Original normal tail, kept contiguous while central reset is held.
     * No guarded CSR/I2C sampling between SW_CTL=1 and SW_CTL=0. */
    for(unsigned step=12;step<21;step++) {
        static const unsigned offset[]={0x2010,0x2008,0x7c,0x1d0,0x1020,0x250,0x2010,0x2010,0x1d0};
        static const uint32_t value[]={0x11,0,0,1,0,0,0x10,0,0};
        cam_sv_shutdown_store(step,0,offset[step-12],value[step-12]);
    }
    cam_sv_shutdown.phase=3;
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_sv_shutdown_guard(epoch);if(e)goto out;
        cam_sv_shutdown.post[sample]=cam_sv_shutdown.after;
        for(unsigned bank=0;bank<2;bank++) {
            e=cam_sv_power_guard();if(e)goto out;
            for(unsigned n=0;n<cam_sv.count;n++)
                cam_sv_shutdown.post_regs[sample][bank][n]=cam_rr(m->sv[bank],cam_sv.offset[n]);
            for(unsigned n=0;n<CAM_SV_QOS_COUNT;n++)
                if(!sample && cam_sv_shutdown.post_regs[sample][bank][cam_sv_find(cam_sv_qos_offsets[n])]!=
                   cam_sv_qos.before[bank][n])
                    printf("X9 CAMERA CAMSV SHUTDOWN ADOPT: bank=%u qos=%x value=%x cold=%x (Android sv_reset leaves it)\n",
                        bank,cam_sv_qos_offsets[n],
                        cam_sv_shutdown.post_regs[sample][bank][cam_sv_find(cam_sv_qos_offsets[n])],
                        cam_sv_qos.before[bank][n]);
            cam_sv_shutdown.fbc_after[bank]=cam_rr(m->sv[bank],0x544);
            if(!sample && (cam_sv_shutdown.fbc_after[bank]&CAM_IRQ_FBC_CNT))
                printf("X9 CAMERA CAMSV SHUTDOWN ADOPT: bank=%u fbc=%x after reset\n",
                    bank,cam_sv_shutdown.fbc_after[bank]);
        }
        for(unsigned role=0;role<3;role++) {
            cam_sv_shutdown.actual[role]=cam_rr(m->sv[0],cam_sv_shutdown_stop[role]);
            if(cam_sv_shutdown.actual[role]){e=CAM_FAIL(ESTALE);goto out;}
        }
        if(cam_rr(m->sv[0],0x7c) || cam_rr(m->sv[0],0x2008) ||
           cam_rr(m->sv[0],0x1d0)){e=CAM_FAIL(ESTALE);goto out;}
        if(sample && memcmp(cam_sv_shutdown.post_regs[0],cam_sv_shutdown.post_regs[1],
            sizeof(cam_sv_shutdown.post_regs[0])))
            printf("X9 CAMERA CAMSV SHUTDOWN ADOPT: post-reset registers changed between samples\n");
        cam_sv_shutdown.post_samples++;
        if(!sample)delay(10);
    }
    memcpy(cam_rx_live.expected.sv,cam_sv_shutdown.post[1].sv,sizeof(cam_rx_live.expected.sv));
    e=cam_rx_session_guard();if(e)goto out;
    cam_sv_qos.retired=true;cam_sv_shutdown.complete=true;cam_sv_shutdown.phase=4;
out:
    cam_sv_shutdown.error=e;
    if(e && !cam_sv_qos.error)cam_sv_qos.error=e;
    printf("X9 CAMERA CAMSV SHUTDOWN: epoch=%llu phase=%u issued=%llx ack=%x/%x/%x "
        "samples=%u/%u/%u post_samples=%u fbc_prior=%x/%x fbc_reset=%x/%x "
        "qos_retired=%u complete=%u errno=%d (after natural retirement; old journals preserved)\n",
        epoch,cam_sv_shutdown.phase,cam_sv_shutdown.issued,cam_sv_shutdown.ack[0],
        cam_sv_shutdown.ack[1],cam_sv_shutdown.ack[2],cam_sv_shutdown.samples[0],
        cam_sv_shutdown.samples[1],cam_sv_shutdown.samples[2],cam_sv_shutdown.post_samples,
        cam_sv_shutdown.fbc_before[0],cam_sv_shutdown.fbc_before[1],cam_sv_shutdown.fbc_after[0],
        cam_sv_shutdown.fbc_after[1],cam_sv_qos.retired,cam_sv_shutdown.complete,e);
    return e;
}
