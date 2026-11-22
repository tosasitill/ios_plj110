/* hhmain/hhtele open(), not a VCM focus provider. The Sony subdrivers use
 * one-byte register addresses for their AK7316 activation, then wait3ms.
 * Keep it separate from the sensor's two-byte register-address allowlist.
 * No position, EEPROM/PID programming or front/UW address inference.
 * Control mutex held. Restore a borrowed VCM mode before cold isolation.
 */
static struct {
    uint64_t epoch;
    uint32_t sensor,slave,prior,observed,id,id_bytes,reads;
    bool attempted,captured,issued,acked,active,complete;
    bool close_attempted,restore_issued,restore_acked,restored;
    int error,restore_error;
} cam_sensor_open;
static int cam_vcm_closed_mode(uint64_t,uint8_t *);

static uint8_t cam_sensor_open_slave(uint32_t sensor_id)
{
    return sensor_id==X9_CAMERA_BACK?0x0c:
        sensor_id==X9_CAMERA_TELEPHOTO?0x2d:0;
}
static int cam_sensor_open_guard(uint64_t deadline)
{
    struct cam_sensor_ap ap={&cam_journal.sensor,
        cam_sensor_slave(cam_sensor_open.sensor)};
    int e;
    if(!cam_sensor_open.epoch || cam_sensor_open.epoch!=cam_sensor_live.epoch ||
       cam_sensor_open.sensor!=cam_sensor_live.probe.sensor ||
       !cam_sensor_open.slave ||
       cam_sensor_open.slave!=cam_sensor_open_slave(cam_sensor_open.sensor) ||
       (cam_sensor_live.phase!=CAM_SENSOR_CONFIGURING &&
        !cam_sensor_open.close_attempted) ||
       cam_sensor_live.close_attempted || session.capture ||
       (session.stream && !cam_sensor_open.close_attempted) ||
       !cam_journal.afvdd.taken)return ESTALE;
    if(mach_absolute_time()>=deadline)return ETIMEDOUT;
    /* Core retains stream=true until power(false) completes. It records an
     * attempted stream, not current source activity. Close access instead
     * needs the independent source/DMA/IRQ/translation retirement receipt. */
    e=cam_sensor_open.close_attempted?
        cam_native_sensor_retirement_guard_locked(cam_sensor_open.epoch):0;
    if(e)return e;
    /* Use the sensor's rail, clock and pin journal for this AF client. */
    return cam_sensor_lens_ap_guard(&ap,cam_sensor_open.close_attempted);
}
static int cam_sensor_open_read(uint64_t deadline,uint8_t *value)
{
    const uint8_t reg=2;
    uint8_t byte=0;
    int e=cam_sensor_open_guard(deadline);
    if(!e)e=cam_bus_transfer(&cam_journal.sensor,(uint8_t)cam_sensor_open.slave,
        &reg,1,&byte);
    if(!e) {
        cam_sensor_open.reads++;
        cam_sensor_open.observed=byte;
        *value=byte;
        e=cam_sensor_open_guard(deadline);
    }
    return e;
}
static int cam_sensor_open_mode(uint64_t deadline,uint8_t value,bool restore)
{
    const uint8_t tx[2]={2,value};
    int e;
    if(!cam_sensor_open.captured ||
       (restore?(!cam_sensor_open.close_attempted || cam_sensor_open.restore_issued ||
                  !cam_sensor_open.active || value!=cam_sensor_open.prior):
                (cam_sensor_open.close_attempted || cam_sensor_open.issued || value)))
        return EPERM;
    e=cam_sensor_open_guard(deadline);
    if(e)return e;
    /* BEFORE the transfer can START. A failed command is never replayed,
     * and later readback cannot clear its original transaction error. */
    if(restore)cam_sensor_open.restore_issued=true;
    else cam_sensor_open.issued=true;
    e=cam_bus_transfer(&cam_journal.sensor,(uint8_t)cam_sensor_open.slave,tx,2,NULL);
    if(!e) {
        if(restore)cam_sensor_open.restore_acked=true;
        else cam_sensor_open.acked=true;
        e=cam_sensor_open_guard(deadline);
    }
    return e;
}
static int cam_sensor_open_observe(uint64_t deadline,uint8_t target)
{
    int e=0;
    for(unsigned sample=0;sample<2 && !e;sample++) {
        uint8_t value=0;
        e=cam_sensor_open_read(deadline,&value);
        if(!e && value!=target)e=ESTALE;
    }
    return e;
}
static int cam_sensor_open_id(uint64_t deadline,struct x9_camera_config *r)
{
    struct cam_sensor_ap ap={&cam_journal.sensor,
        cam_sensor_slave(cam_sensor_open.sensor)};
    uint32_t id=0;
    int e=0;
    for(unsigned n=0;n<2 && !e;n++) {
        uint8_t byte=0;
        r->reg=0x16+n;r->observed=UINT32_MAX;
        r->expected=n?sensors[cam_sensor_open.sensor-1].chip_id&255u:
            sensors[cam_sensor_open.sensor-1].chip_id>>8;
        e=cam_sensor_open_guard(deadline);
        if(!e)e=cam_sensor_ap_read(&ap,(uint16_t)r->reg,&byte);
        if(!e) {
            cam_sensor_open.id_bytes++;
            id=(id<<8)|byte;r->observed=byte;
            if(byte!=r->expected)e=ENXIO;
        }
        if(!e)e=cam_sensor_open_guard(deadline);
    }
    cam_sensor_open.id=id;
    return e;
}
static int cam_sensor_open_activate(uint64_t deadline,struct x9_camera_config *r)
{
    uint8_t before=0;
    int e;
    r->reg=2;r->expected=0;r->observed=UINT32_MAX;
    e=cam_sensor_open_read(deadline,&before);
    if(!e) {
        cam_sensor_open.prior=before;r->observed=before;
        /* All three modes are used by the supplied AK7316 drivers. Refuse
         * an unknown mode rather than guessing its restoration semantics. */
        if(before!=0 && before!=0x20 && before!=0x40)e=ENOTSUP;
    }
    if(!e)e=cam_sensor_open_observe(deadline,before);
    if(!e)cam_sensor_open.captured=true;
    /* The vendor open writes active even if it is already active. This
     * is one original command, not retry/poll-based reactivation. */
    if(!e)e=cam_sensor_open_mode(deadline,0,false);
    if(!e)delay(3000);
    if(!e)e=cam_sensor_open_observe(deadline,0);
    r->observed=cam_sensor_open.observed;
    if(!e)cam_sensor_open.active=true;
    return e;
}
static int cam_sensor_open_prepare(uint32_t which,struct x9_camera_config *r)
{
    uint64_t span,deadline;
    int e=0;
    if(!cam_sensor_open_slave(which))return 0;
    if(cam_sensor_open.epoch!=cam_sensor_live.epoch || cam_sensor_open.sensor!=which ||
       cam_sensor_open.attempted)return ESTALE;
    nanoseconds_to_absolutetime(1000000000ull,&span);deadline=mach_absolute_time()+span;
    cam_sensor_open.attempted=true;r->stage=X9_CAM_CFG_OPEN;
    /* hhmain: activate ->3ms->get_imgsensor_id; hhtele: ID->activate->3ms.
     * The earlier registration probe is not this actual open() ID read. */
    if(which==X9_CAMERA_BACK)e=cam_sensor_open_activate(deadline,r);
    if(!e)e=cam_sensor_open_id(deadline,r);
    if(!e && which==X9_CAMERA_TELEPHOTO)e=cam_sensor_open_activate(deadline,r);
    if(e)cam_sensor_open.error=e;
    else cam_sensor_open.complete=true;
    printf("X9 CAMERA SENSOR OPEN: epoch=%llu sensor=%u AF=%x prior=%x mode=%x "
        "issued/acked=%u/%u id=%x bytes=%u complete=%u errno=%d "
        "(vendor sensor-open activation, no FOCUS/PID/position writes)\n",
        cam_sensor_open.epoch,which,cam_sensor_open.slave,cam_sensor_open.prior,
        cam_sensor_open.observed,cam_sensor_open.issued,cam_sensor_open.acked,
        cam_sensor_open.id,cam_sensor_open.id_bytes,cam_sensor_open.complete,e);
    return e;
}
static int cam_sensor_open_restore(uint64_t epoch)
{
    uint64_t span,deadline;
    uint8_t close_mode=0;
    int e;
    if(cam_sensor_open.epoch!=epoch)return ESTALE;
    if(!cam_sensor_open.issued) {
        cam_sensor_open.restored=true;
        return 0; /* ID-only open or rejection before any VCM mutation. */
    }
    if(cam_sensor_open.close_attempted)return ESTALE;
    cam_sensor_open.close_attempted=true;
    /* A later sensor-ID/table error must not hide a completely observed
     * activation. Its mode can still restore, without clearing that error.
     * An ambiguous activation itself never takes this cleanup path. */
    if(!cam_sensor_open.captured || !cam_sensor_open.acked || !cam_sensor_open.active)
        e=cam_sensor_open.error?cam_sensor_open.error:ESTALE;
    else {
        nanoseconds_to_absolutetime(1000000000ull,&span);deadline=mach_absolute_time()+span;
        e=cam_vcm_closed_mode(epoch,&close_mode);
        if(!e)e=cam_sensor_open_observe(deadline,close_mode);
        if(!e && cam_sensor_open.prior!=close_mode) {
            e=cam_sensor_open_mode(deadline,(uint8_t)cam_sensor_open.prior,true);
            /* Preserve the tele driver's >2.71ms sleep entry bound, using
             * the same3ms wait as sensor open. VCM release is separate. */
            if(!e)delay(3000);
            if(!e)e=cam_sensor_open_observe(deadline,(uint8_t)cam_sensor_open.prior);
        }
    }
    if(e)cam_sensor_open.restore_error=e;
    else cam_sensor_open.restored=true;
    printf("X9 CAMERA SENSOR OPEN RESTORE: epoch=%llu sensor=%u prior=%x mode=%x "
        "issued/acked=%u/%u restored=%u errno=%d (before isolation/supply restore)\n",
        epoch,cam_sensor_open.sensor,cam_sensor_open.prior,cam_sensor_open.observed,
        cam_sensor_open.restore_issued,cam_sensor_open.restore_acked,
        cam_sensor_open.restored,e);
    return e;
}
