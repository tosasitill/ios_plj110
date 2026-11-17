/* Native RAW10 A0 static worst-case MMQoS supplier (control mutex/thread).
 * Actual MT6993 DT MMPC_V2, not the old packed VMMRC table: AP words
 * 0x31ae0000+ff0, 24 independent channel entries; mask+10b0.
 * L19/P0 -> common0/port5 -> channel1: SRT-write index5/HRT-write index7.
 * Only a cold ALL-ZERO AP table may use the vendor all-AP mask handshake;
 * disabling it under another AP user's demand would transiently drop that
 * user's floor. Preserve all other words and refuse any new AP writer.
 * Average/active-line peak bounds derive from actual verified LL/FL, the
 * vendor preview PLL pclk and the actual RAW10 stride plan, not fixed30fps.
 * Static AP demand replaces camera BWR's dynamic reporting for this one
 * path; do not start/overwrite the shared BWR engine. Require authoritative
 * MMPC TOTAL_CURRENT channel demand >= our margin-adjusted floor, plus
 * actual A0 HRT permission/OSTD. It is not a PMIC/DRAM-clock measurement
 * or DMA/SID lease; original native capture authority remains separate.
 */
enum { CAM_NATIVE_BW_WORDS=24, CAM_NATIVE_BW_SRT=5, CAM_NATIVE_BW_HRT=7,
    CAM_NATIVE_BW_NO_WORD=24, CAM_NATIVE_BW_MASK_WORD=25 };
static struct {
    vm_offset_t mmpc,mminfra;
    uint64_t epoch,pclk,average,peak;
    uint32_t before[CAM_NATIVE_BW_WORDS],expected[CAM_NATIVE_BW_WORDS];
    uint32_t mask_before,mask_expected,prior,next,current[2],ll,fl,ostd;
    uint32_t units[2],mix_mbps;
    unsigned pending;
    bool held,complete,restoring,close_attempted,conflict;
} cam_native_bw;

static int cam_native_bw_guard(void)
{
    uint32_t observed;
    int e;
    if(!cam_native_bw.held)return 0;
    if(cam_native_bw.conflict || !cam_smi.held || cam_native_bw.epoch!=cam_smi.epoch)
        return ESTALE;
    e=x9_mm_power_validate(cam_mm.receive.power_token,X9_MM_POWER_CAMERA);if(e)return e;
    observed=cam_rr(cam_native_bw.mmpc,0x10b0);
    if(observed!=cam_native_bw.mask_expected &&
       !(cam_native_bw.pending==CAM_NATIVE_BW_MASK_WORD && observed==cam_native_bw.prior))
        goto conflict;
    for(unsigned n=0;n<CAM_NATIVE_BW_WORDS;n++) {
        observed=cam_rr(cam_native_bw.mmpc,0xff0+4*n);
        if(observed!=cam_native_bw.expected[n] &&
           !(cam_native_bw.pending==n && observed==cam_native_bw.prior))goto conflict;
    }
    if(cam_native_bw.complete && cam_native_bw.pending==CAM_NATIVE_BW_NO_WORD) {
        cam_native_bw.current[0]=cam_rr(cam_native_bw.mmpc,0xd8c+4*CAM_NATIVE_BW_SRT);
        cam_native_bw.current[1]=cam_rr(cam_native_bw.mmpc,0xd8c+4*CAM_NATIVE_BW_HRT);
        if(cam_native_bw.current[0]==0xffffffffu || cam_native_bw.current[1]==0xffffffffu ||
           cam_native_bw.current[0]<cam_native_bw.units[0] ||
           cam_native_bw.current[1]<cam_native_bw.units[1])goto conflict;
    }
    return 0;
conflict:
    cam_native_bw.conflict=true;
    printf("X9 CAMERA MMQOS CONFLICT: epoch=%llu mask=%x pending=%u "
        "current=%x/%x required=%x/%x (retain AP/SMI/MM demand)\n",
        cam_native_bw.epoch,cam_rr(cam_native_bw.mmpc,0x10b0),cam_native_bw.pending,
        cam_native_bw.current[0],cam_native_bw.current[1],cam_native_bw.units[0],
        cam_native_bw.units[1]);
    return ESTALE;
}
static int cam_native_bw_write(unsigned word,uint32_t value)
{
    uint32_t *expected,offset,observed;
    int e=cam_native_bw_guard();if(e)return e;
    if(cam_native_bw.pending!=CAM_NATIVE_BW_NO_WORD)return EBUSY;
    if(word==CAM_NATIVE_BW_MASK_WORD) {
        if(value!=0 && value!=0x0fffffffu)return EINVAL;
        expected=&cam_native_bw.mask_expected;offset=0x10b0;
    } else {
        if(word!=CAM_NATIVE_BW_SRT && word!=CAM_NATIVE_BW_HRT)return EINVAL;
        expected=&cam_native_bw.expected[word];offset=0xff0+4*word;
    }
    if(*expected==value)return 0;
    /* Vendor mminfra dummy read before the AP mask/table transaction;
     * the real shared token was validated above, not an unpowered probe. */
    (void)cam_rr(cam_native_bw.mminfra,0x400);
    cam_native_bw.prior=*expected;cam_native_bw.next=value;*expected=value;
    cam_native_bw.pending=word;
    cam_rw(cam_native_bw.mmpc,offset,value);
    observed=cam_rr(cam_native_bw.mmpc,offset);
    if(observed!=value) {
        if(observed!=cam_native_bw.prior)cam_native_bw.conflict=true;
        return EIO;
    }
    cam_native_bw.pending=CAM_NATIVE_BW_NO_WORD;return cam_native_bw_guard();
}
static int cam_native_bw_pending_retire(void)
{
    uint32_t observed,offset;
    int e;
    if(cam_native_bw.pending==CAM_NATIVE_BW_NO_WORD)return 0;
    if(cam_native_bw.restoring)return EBUSY;
    e=cam_native_bw_guard();if(e)return e;
    offset=cam_native_bw.pending==CAM_NATIVE_BW_MASK_WORD?0x10b0:
        0xff0+4*cam_native_bw.pending;
    observed=cam_rr(cam_native_bw.mmpc,offset);
    /* Prior alone is not retirement; never replay an unresolved write. */
    if(observed!=cam_native_bw.next)return EBUSY;
    cam_native_bw.pending=CAM_NATIVE_BW_NO_WORD;return cam_native_bw_guard();
}
static int cam_native_bw_take_locked(uint64_t epoch)
{
    static vm_offset_t mmpc,mminfra;
    struct x9_camera_pipeline plan;
    struct cam_sensor_ap ap;
    uint8_t hi,lo;
    uint32_t ll,fl,min_fl;
    uint64_t pclk,period,margin,mix;
    int e;
    if(cam_native_bw.held)return EBUSY;
    if(!cam_native_clock_request.epoch || epoch!=cam_smi.epoch ||
       cam_sensor_live.epoch!=epoch || cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED ||
       session.capture || session.stream)return ESTALE;
    e=cam_pipeline_plan(cam_smi.status.sensor,&plan);if(e)return e;
    ap=(struct cam_sensor_ap){&cam_journal.sensor,
        cam_sensor_slave(cam_smi.status.sensor)};
    e=cam_sensor_ap_read(&ap,0x0100,&lo);if(!e && lo)e=EBUSY;
    if(!e)e=cam_sensor_ap_read(&ap,0x0104,&lo);if(!e && lo)e=EBUSY;
    if(e)return e;
    e=cam_sensor_ap_read(&ap,0x0342,&hi);
    if(!e)e=cam_sensor_ap_read(&ap,0x0343,&lo);if(e)return e;
    ll=((uint32_t)hi<<8)|lo;
    e=cam_sensor_ap_read(&ap,0x0340,&hi);
    if(!e)e=cam_sensor_ap_read(&ap,0x0341,&lo);if(e)return e;
    fl=((uint32_t)hi<<8)|lo;
    pclk=sensors[plan.sensor-1].pclk;
    min_fl=sensors[plan.sensor-1].frame_lines;
    if(ll!=sensors[plan.sensor-1].line_length || fl<min_fl || fl<plan.height)
        return ERANGE;
    /* AE may extend FL, never shorten the configured minimum. Hold the
     * worst case minimum frame period, even if this actual FL is longer. */
    period=(uint64_t)ll*min_fl;
    if(!mmpc)mmpc=ml_io_map(0x31ae0000,0x2000);
    if(!mminfra)mminfra=ml_io_map(0x30000000,0x1000);
    if(!mmpc || !mminfra)return ENOMEM;
    memset(&cam_native_bw,0,sizeof(cam_native_bw));
    cam_native_bw.mmpc=mmpc;cam_native_bw.mminfra=mminfra;cam_native_bw.epoch=epoch;
    cam_native_bw.pclk=pclk;cam_native_bw.ll=ll;cam_native_bw.fl=min_fl;
    cam_native_bw.average=((uint64_t)plan.bytes*pclk+period-1)/period;
    cam_native_bw.peak=((uint64_t)plan.stride*pclk+ll-1)/ll;
    /* Vendor SRT_DATA_BW and HRT occupancy are10/7. Round the 16MB/s
     * request units UP, not down, so the bounded RAW demand is retained. */
    margin=16000000ull*7;
    cam_native_bw.units[0]=(uint32_t)((cam_native_bw.average*10+margin-1)/margin);
    cam_native_bw.units[1]=(uint32_t)((cam_native_bw.peak*10+margin-1)/margin);
    if(!cam_native_bw.units[0] || !cam_native_bw.units[1] ||
       cam_native_bw.units[0]>1023 || cam_native_bw.units[1]>1023)return ERANGE;
    /* ISP8s calc_bw -> to_qos_icc truncates B/s /1024 to ICC KB/s,
     * unlike generic Bps_to_icc's /1000. aggregate_requests_v2 then
     * takes max(avg,peak); icc_to_MBps truncates /1000 BEFORE SHIFT_ROUND.
     * MT6993 L19/P0 is write/bw_ratio9; live DT policy3
     * means OSTDL_3: ceil(mix_MBps/512)*3, not ceil(mix_MBps/256).
     * The 10/7 channel occupancy above does not enter per-port OSTD.
     * This finite single-path supplier never requests MTK_MMQOS_MAX_BW,
     * so no v2_max_ostd/display-policy override applies. Same source
     * starts at1 for sub-MB/s demand, then clamps at max_ratio64. */
    mix=cam_native_bw.peak>cam_native_bw.average?
        cam_native_bw.peak:cam_native_bw.average;
    cam_native_bw.mix_mbps=(uint32_t)((mix/1024ull)/1000ull);
    cam_native_bw.ostd=cam_native_bw.mix_mbps?
        ((cam_native_bw.mix_mbps+511u)>>9)*3u:1u;
    if(cam_native_bw.ostd>64)cam_native_bw.ostd=64;
    e=cam_smi_a0_guard();if(!e)e=cam_smi_a0_idle();if(e)return e;
    e=cam_sensor_ap_read(&ap,0x0100,&lo);if(!e && lo)e=EBUSY;
    if(!e)e=cam_sensor_ap_read(&ap,0x0104,&lo);if(!e && lo)e=EBUSY;
    if(e)return e;
    cam_native_bw.mask_before=cam_native_bw.mask_expected=cam_rr(mmpc,0x10b0);
    if(cam_native_bw.mask_before!=0 && cam_native_bw.mask_before!=0x0fffffffu)return EBUSY;
    for(unsigned n=0;n<CAM_NATIVE_BW_WORDS;n++) {
        cam_native_bw.before[n]=cam_native_bw.expected[n]=cam_rr(mmpc,0xff0+4*n);
        if(cam_native_bw.before[n])return EBUSY;
    }
    delay(10);cam_native_bw.held=true;cam_native_bw.pending=CAM_NATIVE_BW_NO_WORD;
    e=cam_native_bw_guard();
    if(!e)e=cam_native_bw_write(CAM_NATIVE_BW_MASK_WORD,0);
    if(!e)e=cam_native_bw_write(CAM_NATIVE_BW_SRT,cam_native_bw.units[0]);
    if(!e)e=cam_native_bw_write(CAM_NATIVE_BW_HRT,cam_native_bw.units[1]);
    if(!e)e=cam_native_bw_write(CAM_NATIVE_BW_MASK_WORD,0x0fffffff);
    if(!e)for(unsigned n=0;n<10000;n++) {
        e=cam_native_bw_guard();if(e)break;
        cam_native_bw.current[0]=cam_rr(mmpc,0xd8c+4*CAM_NATIVE_BW_SRT);
        cam_native_bw.current[1]=cam_rr(mmpc,0xd8c+4*CAM_NATIVE_BW_HRT);
        if(cam_native_bw.current[0]!=0xffffffffu && cam_native_bw.current[1]!=0xffffffffu &&
           cam_native_bw.current[0]>=cam_native_bw.units[0] &&
           cam_native_bw.current[1]>=cam_native_bw.units[1]) {
            cam_native_bw.complete=true;break;
        }
        if(n==9999)e=ETIMEDOUT;else delay(10);
    }
    if(!e)e=cam_smi_a0_write(CAM_SMI_A0_OSTD,UINT32_MAX,cam_native_bw.ostd);
    if(!e)e=cam_smi_a0_write(CAM_SMI_A0_ULTRA,1,0);
    if(!e)e=cam_smi_a0_write(CAM_SMI_A0_NONSEC,8,0);
    if(!e)e=cam_native_bw_guard();
    printf("X9 CAMERA MMQOS TAKE: epoch=%llu LL/FL=%u/%u pclk=%llu bytes=%u stride=%u "
        "avg/peak=%llu/%llu units/current=%u/%u:%u/%u mix_MBps=%u OSTD=%u errno=%d "
        "(MMPC V2 AP floor; HRT A0 only, not DMA/SID lease)\n",
        epoch,ll,min_fl,pclk,plan.bytes,plan.stride,cam_native_bw.average,cam_native_bw.peak,
        cam_native_bw.units[0],cam_native_bw.units[1],cam_native_bw.current[0],
        cam_native_bw.current[1],cam_native_bw.mix_mbps,cam_native_bw.ostd,e);
    return e;
}
static int cam_native_bw_close_locked(void)
{
    int e;
    if(!cam_native_bw.held)return 0;
    if(cam_native_bw.close_attempted || cam_native_bw.epoch!=cam_smi.epoch)return ESTALE;
    cam_native_bw.close_attempted=true;
    /* This is only supplier withdrawal after the upstream mandatory native
     * engine guard has authorized the actual capture retirement scope.
     * SMI port idle alone is NOT full DMA/SMMU retirement or a DMA grant.
     * Keep that authority separate: ordinary live capture checks still need
     * held+complete bandwidth, and only the real retire-stage guard may
     * permit this AP budget to retire before A0/shared/clocks restore. */
    e=cam_native_bw_pending_retire();if(!e)e=cam_smi_a0_idle();if(e)return e;
    cam_native_bw.complete=false;cam_native_bw.restoring=true;
    e=cam_native_bw_write(CAM_NATIVE_BW_MASK_WORD,0);
    if(!e)e=cam_native_bw_write(CAM_NATIVE_BW_HRT,cam_native_bw.before[CAM_NATIVE_BW_HRT]);
    if(!e)e=cam_native_bw_write(CAM_NATIVE_BW_SRT,cam_native_bw.before[CAM_NATIVE_BW_SRT]);
    if(!e)e=cam_native_bw_write(CAM_NATIVE_BW_MASK_WORD,cam_native_bw.mask_before);
    if(!e)e=cam_native_bw_guard();
    if(e)return e; /* original SMI/A0 close must retain the remaining floor */
    /* Original SMI close keeps using its supplier guard during A0/clock
     * restoration. No outstanding receipt now: guard returns0 with held=false.
     * This must not relax native_source_rate_guard_locked() on a live path. */
    cam_native_bw.held=false;
    printf("X9 CAMERA MMQOS RESTORE: epoch=%llu AP table/mask=%x restored "
        "(producer inactive; aggregate hardware budgets not overwritten)\n",
        cam_native_bw.epoch,cam_native_bw.mask_before);
    return 0;
}
