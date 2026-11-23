/* Real cold sensor callbacks for a supplied native capture engine. They use
 * internal locked helpers because core already owns camera_control_lock.
 * No duplicated table pass and no public API/control-mutex reentry. */
#include "x9_camera_native_sensor.h"
LCK_MTX_DECLARE(camera_native_registration_lock,&camera_group);
static struct {
    struct x9_camera_native_engine engine;
    uint64_t epoch;
    uint32_t sensor;
    bool configured,prepared;
    int error;
    struct x9_camera_native_source_status source;
} cam_native_sensor;
static int cam_native_sensor_retirement_guard_locked(uint64_t epoch)
{
    if(!epoch)epoch=cam_native_sensor.epoch; /* MM-only close checks current native owner */
    if(!epoch || epoch!=cam_native_sensor.epoch || !cam_native_sensor.engine.version)return 0;
    if(!x9_camera_sensor_valid(cam_native_sensor.sensor))return CAM_FAIL(ESTALE);
    return cam_native_sensor.engine.retirement_guard(cam_native_sensor.engine.context,
        epoch,&sensors[cam_native_sensor.sensor-1]);
}

static int cam_native_sensor_match(const struct x9_camera_sensor *s)
{
    if(!s || !x9_camera_sensor_valid(s->sensor) ||
       s!=&sensors[s->sensor-1])return CAM_FAIL(EINVAL);
    if(!cam_native_sensor.epoch || cam_native_sensor.sensor!=s->sensor ||
       cam_native_sensor.epoch!=cam_sensor_live.epoch)return CAM_FAIL(ESTALE);
    return cam_sensor_session_match(cam_native_sensor.epoch);
}
static int cam_native_sensor_power(const struct x9_camera_sensor *s,int on)
{
    struct x9_camera_probe r;
    int e;
    if(!s || !x9_camera_sensor_valid(s->sensor) ||
       s!=&sensors[s->sensor-1] || (on!=0 && on!=1))return CAM_FAIL(EINVAL);
    if(on) {
        if(cam_native_sensor.epoch)return CAM_FAIL(EBUSY);
        /* A clean ID NACK (ENXIO) of ANOTHER sensor whose session already
         * closed (epoch 0) must not block probing the next sensor. Any
         * other sticky fault still blocks. */
        /* Log 347: a clean close zeroes .sensor, so r23b never matched.
         * epoch==0 here (checked above) means the NACKing session closed. */
        if(cam_native_sensor.error==ENXIO && cam_native_sensor.sensor!=s->sensor)
            cam_native_sensor.error=0;
        if(cam_native_sensor.error)return cam_native_sensor.error;
        cam_native_sensor.sensor=s->sensor;
        cam_native_sensor.configured=cam_native_sensor.prepared=false;
        e=cam_sensor_session_open_locked(s->sensor,&cam_native_sensor.epoch,&r);
        if(e) {
            cam_native_sensor.error=e;
            /* Same journal record as mode3: stage/blockers/clock/rail/pin
             * snapshot already read by the failed open. No new MMIO. */
            if(cam_native_sensor.epoch)cam_sensor_cold_log(&r);
        }
        return e;
    }
    /* Core pairs even an unadmitted power-on error once. An empty epoch is
     * not a DMA retirement claim; no sensor/MM scope was admitted here. */
    if(!cam_native_sensor.epoch)return 0;
    e=cam_native_sensor_match(s);if(e)return e;
    e=cam_sensor_session_close_locked(cam_native_sensor.epoch,&r);
    /* Recycle only AFTER the original sensor/RX/SV/SMI/MM chain has
     * physically closed. The capture wrapper preserves the old receipt
     * and backing; a stopped IRQ or restored translation is not enough.
     * ID-only registration probes have no prepared capture and are no-op.
     * An unsuccessful recycle retains this epoch and its original fault. */
    if(!e)e=cam_native_capture_recycle_locked(cam_native_sensor.epoch);
    if(e) {if(!cam_native_sensor.error)cam_native_sensor.error=e;}
    else {
        cam_native_sensor.epoch=0;cam_native_sensor.sensor=0;
        cam_native_sensor.configured=cam_native_sensor.prepared=false;
    }
    return e;
}
static int cam_native_sensor_read8(const struct x9_camera_sensor *s,uint16_t reg,uint8_t *v)
{
    struct cam_sensor_ap ap;
    int e;
    if(!v)return CAM_FAIL(EINVAL);
    e=cam_native_sensor_match(s);if(e)return e;
    if(cam_native_sensor.error)return cam_native_sensor.error;
    ap=(struct cam_sensor_ap){&cam_journal.sensor,(uint8_t)s->slave};
    e=cam_sensor_ap_read(&ap,reg,v);
    if(e)cam_native_sensor.error=e;
    return e;
}
static int cam_native_sensor_write8(const struct x9_camera_sensor *s,uint16_t reg,uint8_t v)
{
    struct cam_sensor_ap ap;
    int e=cam_native_sensor_match(s);if(e)return e;
    if(cam_native_sensor.error)return cam_native_sensor.error;
    /* 0100 belongs to cam_stream_set_locked. Generic exposure writes are
     * not allowed to mutate metadata of an already issued immutable request.
     * The continuous native engine still needs a frame-bound AE scheduler. */
    if(session.capture || session.stream)return CAM_FAIL(EBUSY);
    ap=(struct cam_sensor_ap){&cam_journal.sensor,(uint8_t)s->slave};
    e=cam_sensor_ap_write(&ap,reg,v);
    if(e)cam_native_sensor.error=e;
    return e;
}
static int cam_native_sensor_configure(const struct x9_camera_sensor *s,
    struct x9_camera_config *r)
{
    int e=cam_native_sensor_match(s);if(e)return e;
    if(cam_native_sensor.error)return cam_native_sensor.error;
    if(cam_native_sensor.configured || cam_native_sensor.prepared)return CAM_FAIL(EALREADY);
    e=cam_sensor_session_configure_locked(cam_native_sensor.epoch,r);
    if(!e) {
        cam_native_sensor.configured=true;
        cam_progress("configure:source-prepare");
        e=cam_native_source_prepare_locked(cam_native_sensor.epoch,&cam_native_sensor.source);
        if(!e)cam_progress("configure:engine-prepare");
        if(!e)e=cam_native_sensor.engine.prepare(cam_native_sensor.engine.context,
            cam_native_sensor.epoch,s);
        if(!e)cam_native_sensor.prepared=true;
    }
    if(e)cam_native_sensor.error=e;
    return e;
}
static int cam_native_sensor_capture(const struct x9_camera_sensor *s,int on,uint64_t generation)
{
    int e=cam_native_sensor_match(s);if(e)return e;
    if(on && cam_native_sensor.error)return cam_native_sensor.error;
    if(on && (!cam_native_sensor.configured || !cam_native_sensor.prepared))return CAM_FAIL(ENXIO);
    e=cam_native_sensor.engine.capture(cam_native_sensor.engine.context,
        cam_native_sensor.epoch,s,on,generation,&cam_sensor_live.config);
    if(e && !cam_native_sensor.error)cam_native_sensor.error=e;
    return e;
}
static int cam_native_sensor_exposure(const struct x9_camera_sensor *s,uint32_t exposure_lines,
    uint32_t gain_q10,uint32_t frame_lines)
{
    int e=cam_native_sensor_match(s);if(e)return e;
    if(cam_native_sensor.error)return cam_native_sensor.error;
    if(!cam_native_sensor.prepared || !session.capture || !session.stream)return CAM_FAIL(ENXIO);
    e=cam_native_sensor.engine.exposure(cam_native_sensor.engine.context,
        cam_native_sensor.epoch,s,exposure_lines,gain_q10,frame_lines);
    if(e)cam_native_sensor.error=e;
    return e;
}
int x9_camera_native_sensor_register(const struct x9_camera_native_engine *engine)
{
    const struct x9_camera_platform p={.version=X9_CAMERA_PLATFORM_VERSION,
        .power=cam_native_sensor_power,.read8=cam_native_sensor_read8,
        .write8=cam_native_sensor_write8,.configure=cam_native_sensor_configure,
        .exposure=cam_native_sensor_exposure,
        .capture=cam_native_sensor_capture};
    int e;
    if(!engine || engine->version!=X9_CAMERA_NATIVE_ENGINE_VERSION || !engine->context ||
       !engine->admit || !engine->prepare || !engine->capture || !engine->exposure ||
       !engine->retirement_guard)return CAM_FAIL(EINVAL);
    lck_mtx_lock(&camera_native_registration_lock);
    /* Check before admit: no second hardware admission after an earlier
     * successful registration or partially failed physical probe. */
    lck_mtx_lock(&camera_control_lock);lck_mtx_lock(&camera_lock);
    e=st.mode!=2?EPERM:hw.version || opened || cam_sensor_session_active ||
        cam_stop_owned() || session.latched || cam_native_sensor.engine.version?CAM_FAIL(EBUSY):0;
    lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);
    if(!e)e=engine->admit(engine->context);
    if(!e) {
        lck_mtx_lock(&camera_control_lock);
        cam_native_sensor.engine=*engine;
        lck_mtx_unlock(&camera_control_lock);
        e=x9_camera_register_platform(&p);
    }
    lck_mtx_unlock(&camera_native_registration_lock);
    return e;
}
