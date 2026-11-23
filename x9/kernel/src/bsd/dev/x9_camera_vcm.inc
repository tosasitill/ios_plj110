/* AK7316M/T 24085 normal lens open/release, separate from sensor open.
 * Register facts/order from the board's V4L2 lens drivers; no PID setting,
 * NVM stores, DAC-mode unlock, front/UW inference or optical-position claim.
 * camera_control_lock held, capture retired before close. No public FOCUS
 * yet: the V4L2 focus control starts at0, independently of g_last_pos=511.
 */
static struct {
    uint64_t epoch;
    uint32_t sensor,slave,reads,pid,origin,control_pos,last_dac;
    uint32_t position_issued,position_acked,positions[12];
    uint32_t mode_issued,mode_acked,mode_observed;
    bool attempted,complete,pid_read,origin_read,position_known,close_attempted,closed;
    int error,close_error;
} cam_vcm;

static int cam_vcm_guard(uint64_t deadline)
{
    struct cam_sensor_ap ap={&cam_journal.sensor,cam_sensor_slave(cam_vcm.sensor)};
    int e;
    if(!cam_vcm.epoch || cam_vcm.epoch!=cam_sensor_live.epoch ||
       cam_vcm.sensor!=cam_sensor_live.probe.sensor || !cam_vcm.slave ||
       cam_vcm.slave!=cam_sensor_open_slave(cam_vcm.sensor) ||
       !cam_vcm.attempted || cam_vcm.closed || cam_sensor_live.close_attempted ||
       cam_sensor_open.epoch!=cam_vcm.epoch || !cam_sensor_open.complete ||
       !cam_sensor_open.active || cam_sensor_open.close_attempted ||
       cam_sensor_open.restored || session.capture || !cam_journal.afvdd.taken ||
       (!cam_vcm.close_attempted && cam_sensor_live.phase!=CAM_SENSOR_CONFIGURING) ||
       (session.stream && !cam_vcm.close_attempted))return CAM_FAIL(ESTALE);
    if(mach_absolute_time()>=deadline)return CAM_FAIL(ETIMEDOUT);
    /* stream is an attempted-stream ledger until power(false) returns.
     * Only the actual native retirement receipt admits this close access. */
    e=cam_vcm.close_attempted?cam_native_sensor_retirement_guard_locked(cam_vcm.epoch):0;
    if(!e)e=cam_sensor_lens_ap_guard(&ap,cam_vcm.close_attempted);
    return e;
}
static int cam_vcm_read(uint64_t deadline,uint8_t reg,uint8_t *bytes,unsigned count)
{
    int e;
    if((reg!=2 && reg!=0x16 && reg!=0x84) ||
       count!=(reg==0x84?2u:1u) || (reg!=2 && cam_vcm.sensor!=X9_CAMERA_BACK))
        return CAM_FAIL(EPERM);
    e=cam_vcm_guard(deadline);
    if(!e)e=cam_bus_transfer_n(&cam_journal.sensor,(uint8_t)cam_vcm.slave,
        &reg,1,bytes,count);
    if(!e) {
        cam_vcm.reads++;
        if(reg==2)cam_vcm.mode_observed=bytes[0];
        e=cam_vcm_guard(deadline);
    }
    return e;
}
static int cam_vcm_mode_check(uint64_t deadline,uint8_t target)
{
    int e=0;
    for(unsigned n=0;n<2 && !e;n++) {
        uint8_t mode=0;
        e=cam_vcm_read(deadline,2,&mode,1);
        if(!e && mode!=target)e=CAM_FAIL(ESTALE);
    }
    return e;
}
static int cam_vcm_position(uint64_t deadline,uint32_t position)
{
    uint32_t index=cam_vcm.position_issued;
    /* SMBus WORD_DATA is low-byte first; swab16(position<<6) in the
     * source therefore sends reg00, DAC[15:8], DAC[7:0]. */
    uint16_t dac=(uint16_t)(position<<6);
    uint8_t tx[3]={0,(uint8_t)(dac>>8),(uint8_t)dac};
    int e;
    if(position>1023 || index>=sizeof(cam_vcm.positions)/sizeof(cam_vcm.positions[0]) ||
       cam_vcm.position_acked!=index || cam_vcm.mode_issued!=cam_vcm.mode_acked ||
       (!cam_vcm.close_attempted &&
        (cam_vcm.sensor!=X9_CAMERA_BACK || index || position!=511)))return CAM_FAIL(EPERM);
    e=cam_vcm_guard(deadline);
    if(e)return e;
    cam_vcm.positions[index]=position;
    cam_vcm.position_issued++; /* BEFORE the transfer can post START. */
    e=cam_bus_transfer(&cam_journal.sensor,(uint8_t)cam_vcm.slave,tx,3,NULL);
    if(!e) {
        cam_vcm.position_acked++;
        cam_vcm.last_dac=position;cam_vcm.position_known=true;
        e=cam_vcm_guard(deadline); /* ACK remains recorded on timeout. */
    }
    return e; /* No guessed readable DAC/physical-settled predicate. */
}
static int cam_vcm_mode(uint64_t deadline)
{
    uint8_t value=cam_vcm.close_attempted?0x20:0;
    uint8_t tx[2]={2,value};
    unsigned bit=cam_vcm.close_attempted?2u:1u;
    int e;
    if((cam_vcm.mode_issued&bit) || cam_vcm.mode_issued!=cam_vcm.mode_acked ||
       (!cam_vcm.close_attempted && cam_vcm.sensor!=X9_CAMERA_BACK))return CAM_FAIL(EPERM);
    e=cam_vcm_guard(deadline);
    if(e)return e;
    cam_vcm.mode_issued|=bit;
    e=cam_bus_transfer(&cam_journal.sensor,(uint8_t)cam_vcm.slave,tx,2,NULL);
    if(!e) {
        cam_vcm.mode_acked|=bit;
        e=cam_vcm_guard(deadline);
    }
    return e;
}
static int cam_vcm_prepare(uint32_t which,struct x9_camera_config *r)
{
    uint64_t span,deadline;
    uint8_t mode=0,pid=0,origin[2]={0};
    int e;
    if(!cam_sensor_open_slave(which))return 0;
    if(cam_vcm.epoch!=cam_sensor_live.epoch || cam_vcm.sensor!=which ||
       cam_vcm.attempted)return CAM_FAIL(ESTALE);
    cam_vcm.attempted=true;r->stage=X9_CAM_CFG_VCM;
    nanoseconds_to_absolutetime(1000000000ull,&span);deadline=mach_absolute_time()+span;
    e=cam_vcm_guard(deadline);
    /* Same normal power_on wait, even though the shared VIN was already
     * taken by the sensor's board power sequence. DT has no vcamaf pins. */
    if(!e)delay(5100);
    if(!e)e=cam_vcm_read(deadline,2,&mode,1);
    if(!e)delay(3000); /* normal init's first status sample always waits3ms */
    if(!e && mode)e=CAM_FAIL(ESTALE); /* sensor open already qualified active; no retry */
    if(!e && which==X9_CAMERA_BACK) {
        e=cam_vcm_read(deadline,0x16,&pid,1);
        if(!e){cam_vcm.pid=pid;cam_vcm.pid_read=true;} /* mismatch only warns in vendor init */
        if(!e)e=cam_vcm_read(deadline,0x84,origin,2);
        if(!e) {
            cam_vcm.origin=(((uint32_t)origin[0]<<8)|origin[1])>>6;
            cam_vcm.origin_read=true;
        }
        if(!e)e=cam_vcm_position(deadline,511); /* initial g_last_pos */
        if(!e)e=cam_vcm_mode(deadline); /* normal main init writes active */
    }
    /* Tele goto_last_pos: both original caches are511, diff=0, no store.
     * Never turn that cache equality into an observed lens position. */
    if(!e && which==X9_CAMERA_TELEPHOTO)cam_vcm.origin=511;
    if(!e)e=cam_vcm_mode_check(deadline,0);
    if(e)cam_vcm.error=e;
    else cam_vcm.complete=true;
    r->reg=2;r->expected=0;r->observed=cam_vcm.mode_observed;
    printf("X9 CAMERA VCM INIT: epoch=%llu sensor=%u AF=%x pid=%x read=%u expected_pid=%x "
        "origin=%u read=%u control=%u DAC=%u known=%u positions=%u/%u modes=%x/%x "
        "complete=%u errno=%d (ACKed command only; no FOCUS/PID stores)\n",
        cam_vcm.epoch,which,cam_vcm.slave,cam_vcm.pid,cam_vcm.pid_read,
        which==X9_CAMERA_BACK?0x20:0,cam_vcm.origin,cam_vcm.origin_read,
        cam_vcm.control_pos,cam_vcm.last_dac,cam_vcm.position_known,
        cam_vcm.position_issued,cam_vcm.position_acked,cam_vcm.mode_issued,
        cam_vcm.mode_acked,cam_vcm.complete,e);
    return e;
}
static int cam_vcm_close(uint64_t epoch)
{
    uint64_t span,deadline;
    uint32_t position;
    int e;
    if(cam_vcm.epoch!=epoch)return CAM_FAIL(ESTALE);
    if(!cam_vcm.attempted) {
        cam_vcm.closed=true;
        return 0; /* ID-only session or configuration stopped before lens init */
    }
    if(cam_vcm.close_attempted)return cam_vcm.close_error?cam_vcm.close_error:ESTALE;
    cam_vcm.close_attempted=true;
    e=cam_vcm.error?cam_vcm.error:!cam_vcm.complete?CAM_FAIL(ESTALE):0;
    nanoseconds_to_absolutetime(1000000000ull,&span);deadline=mach_absolute_time()+span;
    if(!e)e=cam_vcm_mode_check(deadline,0);
    position=cam_vcm.control_pos;
    if(!e && position>1023)e=CAM_FAIL(ESTALE);
    /* Normal release uses focus->val, not the init's separate g_last_pos.
     * The V4L2 control defaults to0 until an absolute control succeeds.
     * Preserve the unconditional final origin store, including equal0. */
    for(unsigned n=position/100;n && !e;n--) {
        position-=100;
        e=cam_vcm_position(deadline,position);
        if(!e)delay(1000);
    }
    if(!e)e=cam_vcm_position(deadline,0);
    if(!e)e=cam_vcm_mode(deadline);
    if(!e && cam_vcm.sensor==X9_CAMERA_TELEPHOTO)delay(2800);
    if(!e)e=cam_vcm_mode_check(deadline,0x20);
    if(e)cam_vcm.close_error=e;
    else cam_vcm.closed=true;
    printf("X9 CAMERA VCM RELEASE: epoch=%llu sensor=%u control=%u DAC=%u known=%u "
        "positions=%u/%u modes=%x/%x mode=%x closed=%u errno=%d "
        "(normal park/sleep before sensor mode/supplier restore)\n",epoch,cam_vcm.sensor,
        cam_vcm.control_pos,cam_vcm.last_dac,cam_vcm.position_known,cam_vcm.position_issued,
        cam_vcm.position_acked,cam_vcm.mode_issued,cam_vcm.mode_acked,
        cam_vcm.mode_observed,cam_vcm.closed,e);
    return e;
}
static int cam_vcm_closed_mode(uint64_t epoch,uint8_t *mode)
{
    if(cam_vcm.epoch!=epoch || cam_vcm.sensor!=cam_sensor_open.sensor ||
       cam_vcm.error || cam_vcm.close_error || !cam_vcm.closed)return CAM_FAIL(ESTALE);
    *mode=0;
    if(cam_vcm.attempted) {
        if(!cam_vcm.complete || !cam_vcm.close_attempted ||
           cam_vcm.position_issued!=cam_vcm.position_acked ||
           !cam_vcm.position_known || cam_vcm.last_dac ||
           cam_vcm.mode_issued!=cam_vcm.mode_acked || !(cam_vcm.mode_acked&2u) ||
           cam_vcm.mode_observed!=0x20)return CAM_FAIL(ESTALE);
        *mode=0x20;
    }
    return 0;
}
