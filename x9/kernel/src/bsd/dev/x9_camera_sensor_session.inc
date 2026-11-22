/* Persistent cold sensor/AP FIFO lifetime. The same open/configure/close
 * engine is used by the existing explicit probe and standby paths; no
 * native capture adapter or boot-time hardware probing is fabricated.
 * camera_control_lock serializes EVERY entry and the shared cold journal.
 * PMIF is acquired only inside existing rail transactions, never held across
 * a session, user wait, AP FIFO operation or configuration callback.
 */
#include "x9_camera_sensor_session.h"
/* Defined by the actual powered receiver engine later in this TU. These
 * helpers use the already-held control mutex; no public entrypoint reentry. */
static int cam_rx_sensor_prepare_locked(uint64_t,struct x9_camera_receiver *);
static int cam_rx_sensor_guard_locked(uint64_t);
static int cam_rx_sensor_close_locked(uint64_t);
static bool cam_rx_sensor_held_locked(uint64_t);
static bool cam_rx_sensor_mm2_take_failed_locked(uint64_t);
enum cam_sensor_session_phase {
    CAM_SENSOR_EMPTY, CAM_SENSOR_POWERING, CAM_SENSOR_STANDBY,
    CAM_SENSOR_CONFIGURING, CAM_SENSOR_CONFIGURED, CAM_SENSOR_FAULT,
    CAM_SENSOR_CLOSING, CAM_SENSOR_RETAINED
};
static struct {
    uint64_t serial, epoch;
    enum cam_sensor_session_phase phase;
    int error, close_error;
    bool close_attempted;
    struct x9_camera_probe probe;
    struct x9_camera_config config;
} cam_sensor_live;
static struct x9_camera_config cam_config_last={.version=1};
struct cam_sensor_ap { struct cam_probe_bus *bus; uint8_t slave; };

static int cam_sensor_session_fault(int e)
{
    if(!cam_sensor_live.error)cam_sensor_live.error=e;
    cam_sensor_live.phase=CAM_SENSOR_FAULT;
    cam_probe_latched=true;
    cam_sensor_live.probe.flags|=X9_CAM_PROBE_LATCHED;
    cam_sensor_live.probe.last_errno=cam_sensor_live.error;
    return e;
}
/* Each sensor request checks consumed WL channels AND interleaved shared
 * PMIC rails, not just the snapshot taken before the table loop. Clock, pin,
 * FIFO and AP configuration guards still run in the transfer. These are
 * visible-state checks, not a lease or an atomic ABA detector. */
static int cam_sensor_ap_local_guard(struct cam_sensor_ap *ap)
{
    struct cam_probe_journal *j=&cam_journal;
    uint8_t enable=0;
    int e;
    if(ap->bus!=&j->sensor || !cam_sensor_live.epoch || cam_sensor_live.close_attempted)
        return ESTALE;
    if(ap->slave!=(cam_sensor_slave(cam_sensor_live.probe.sensor)))return EINVAL;
    e=cam_wl_guard(j,false,&enable);
    if(!e && (enable&j->board.wl_mask)!=j->board.wl_mask)e=EBUSY;
    if(!e)e=cam_clock_check(j,false,true);
    if(!e)e=cam_pins_guard(j,false);
    return e;
}
static int cam_sensor_ap_guard(struct cam_sensor_ap *ap)
{
    int e=cam_sensor_ap_local_guard(ap);
    return e?e:cam_rx_sensor_guard_locked(cam_sensor_live.epoch);
}

/* Android VCM release uses the powered AF I2C client independently of MM_2.
 * Close callers have already checked the capture retirement receipt. When
 * MM_2 acquisition failed before CSI or receiver admission, the lens can
 * finish its I2C close using the sensor's live rail, clock and pin guards. */
static int cam_sensor_lens_ap_guard(struct cam_sensor_ap *ap,bool closing)
{
    if(closing && cam_rx_sensor_mm2_take_failed_locked(cam_sensor_live.epoch))
        return cam_sensor_ap_local_guard(ap);
    return cam_sensor_ap_guard(ap);
}
static int cam_sensor_ap_read(void *context, uint16_t reg, uint8_t *v)
{
    struct cam_sensor_ap *ap=context;
    const uint8_t tx[2]={(uint8_t)(reg>>8),(uint8_t)reg};
    uint8_t byte=0;
    int e=cam_sensor_ap_guard(ap);
    if(!e && sensors[cam_sensor_live.probe.sensor-1].table_word &&
       (!cam_journal.page_known || cam_journal.page_pending || cam_journal.sensor_page!=0x4000))e=ESTALE;
    if(!e)e=cam_bus_transfer(ap->bus,ap->slave,tx,2,&byte);
    if(!e)*v=byte;
    return e;
}
static int cam_sensor_ap_write(void *context, uint16_t reg, uint8_t v)
{
    struct cam_sensor_ap *ap=context;
    const uint8_t tx[3]={(uint8_t)(reg>>8),(uint8_t)reg,v};
    bool fab=ap->slave==0x10 && ((reg==0x0a02 && v==0x7f) || (reg==0x0a00 && v<=1));
    bool mirror=reg==0x0101 && v==sensors[cam_sensor_live.probe.sensor-1].mirror;
    int e;
    if(cam_sensor_forbidden(reg) && !mirror && !fab)return EPERM;
    e=cam_sensor_ap_guard(ap);
    if(!e && sensors[cam_sensor_live.probe.sensor-1].table_word &&
       (!cam_journal.page_known || cam_journal.page_pending || cam_journal.sensor_page!=0x4000))e=ESTALE;
    if(!e)e=cam_bus_transfer(ap->bus,ap->slave,tx,3,NULL);
    return e;
}
static int cam_sensor_ap_read_word(void *context,uint16_t reg,uint16_t *v)
{
    struct cam_sensor_ap *ap=context;uint8_t bytes[2]={0};
    const uint8_t tx[2]={(uint8_t)(reg>>8),(uint8_t)reg};
    int e=cam_sensor_ap_guard(ap);
    if(!e && (cam_sensor_live.probe.sensor!=X9_CAMERA_ULTRAWIDE ||
       !cam_journal.page_known || cam_journal.page_pending || cam_journal.sensor_page!=0x4000))e=ESTALE;
    if(!e)e=cam_bus_transfer_n(ap->bus,ap->slave,tx,2,bytes,2);
    if(!e)*v=(uint16_t)((uint16_t)bytes[0]<<8)|bytes[1];
    return e;
}
static int cam_sensor_ap_write_word(void *context,uint16_t reg,uint16_t v)
{
    struct cam_sensor_ap *ap=context;struct cam_probe_journal *j=&cam_journal;
    uint8_t tx[4]={(uint8_t)(reg>>8),(uint8_t)reg,(uint8_t)(v>>8),(uint8_t)v};
    if(cam_sensor_live.probe.sensor!=X9_CAMERA_ULTRAWIDE ||
       cam_sensor_live.phase!=CAM_SENSOR_CONFIGURING || j->page_pending)return EPERM;
    if(reg==0xfcfc) {if(!cam_uw_page(v))return EPERM;}
    else {
        if(!j->page_known)return ESTALE;
        if(j->sensor_page==0x4000 && (reg==0x100 || reg==0x101 || reg==0x103 ||
           reg==0x104 || reg==0x16 || reg==0x17))return EPERM;
        if(j->sensor_page==0x4000 && ((reg==0 && v!=0xe && v!=0x38e5) ||
           ((reg==0x6014 || reg==0x6018) && v!=1)))return EPERM;
    }
    int e=cam_sensor_ap_guard(ap);if(e)return e;
    if(reg==0xfcfc)j->page_pending=true; /* before ambiguous transfer */
    e=cam_bus_transfer_n(ap->bus,ap->slave,tx,4,NULL,0);
    if(!e && reg==0xfcfc) {j->sensor_page=v;j->page_known=true;j->page_pending=false;}
    return e;
}
#include "x9_camera_sensor_open.inc"
#include "x9_camera_vcm.inc"
#include "x9_camera_sensor_calibration.inc"
static int cam_sensor_session_match(uint64_t epoch)
{
    if(!epoch || epoch!=cam_sensor_live.epoch)return ESTALE;
    if(cam_sensor_live.close_attempted)return cam_sensor_live.close_error?cam_sensor_live.close_error:ESTALE;
    return 0;
}
static int cam_sensor_session_open_locked(uint32_t which,uint64_t *epoch,
    struct x9_camera_probe *r)
{
    struct cam_sensor_ap ap;
    uint8_t byte=0;
    int e;
    *epoch=0;
    memset(r,0,sizeof(*r));r->version=1;r->sensor=which;
    if(!x9_camera_sensor_valid(which))e=EINVAL;
    else if(st.mode!=2 && st.mode!=3)e=EPERM;
    else if(cam_sensor_live.epoch || cam_mm_live.epoch)e=EBUSY;
    else if(session.capture || session.stream || session.latched)e=EBUSY;
    else if(cam_probe_latched || cam_resources_latched)e=EIO;
    else if(cam_sensor_live.serial==UINT64_MAX)e=EOVERFLOW;
    else e=0;
    if(e) {
        r->last_errno=e;
        if(cam_probe_latched || cam_resources_latched)r->flags=X9_CAM_PROBE_LATCHED;
        return e; /* do not memset or unwind a foreign/retained journal */
    }
    cam_sensor_live.epoch=++cam_sensor_live.serial;
    *epoch=cam_sensor_live.epoch; /* BEFORE the first possibly ambiguous mutation */
    cam_sensor_session_active=true;
    cam_sensor_live.phase=CAM_SENSOR_POWERING;
    cam_sensor_live.error=cam_sensor_live.close_error=0;
    cam_sensor_live.close_attempted=false;
    memset(&cam_sensor_live.config,0,sizeof(cam_sensor_live.config));
    bzero(&cam_sensor_open,sizeof(cam_sensor_open));
    cam_sensor_open.epoch=cam_sensor_live.epoch;
    cam_sensor_open.sensor=which;cam_sensor_open.slave=cam_sensor_open_slave(which);
    bzero(&cam_vcm,sizeof(cam_vcm));
    cam_vcm.epoch=cam_sensor_live.epoch;cam_vcm.sensor=which;
    cam_vcm.slave=cam_sensor_open_slave(which);
    r->flags=X9_CAM_PROBE_ATTEMPTED;r->stage=X9_CAM_STAGE_PREFLIGHT;
    cam_sensor_live.probe=*r;
    e=cam_sensor_cold_open(which,r);
    cam_sensor_live.probe=*r;
    ap=(struct cam_sensor_ap){&cam_journal.sensor,cam_sensor_slave(which)};
    /* Persist only a matched, physically reset, standby sensor. Unexpected
     * stream/hold is not fixed with a guessed stream-off/group release. */
    if(!e)e=cam_sensor_ap_read(&ap,0x0100,&byte);
    if(!e && byte)e=EBUSY;
    if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);
    if(!e && byte)e=EBUSY;
    if(e)cam_sensor_session_fault(e);
    else cam_sensor_live.phase=CAM_SENSOR_STANDBY;
    cam_sensor_live.probe.i2c_status[0]=cam_journal.sensor.last_status;
    cam_sensor_live.probe.i2c_status[1]=cam_journal.ldo.last_status;
    *r=cam_sensor_live.probe;
    /* No RESTORED/DONE/READY: dependencies remain physically held. */
    return e;
}
static int cam_sensor_session_configure_locked(uint64_t epoch,struct x9_camera_config *r)
{
    struct cam_sensor_ap ap;
    struct cam_sensor_io io;
    uint32_t which=cam_sensor_live.probe.sensor;
    int e=cam_sensor_session_match(epoch);
    memset(r,0,sizeof(*r));r->version=1;r->sensor=which;r->observed=UINT32_MAX;
    if(!e && cam_sensor_live.error)e=cam_sensor_live.error;
    if(!e && cam_sensor_live.phase!=CAM_SENSOR_STANDBY)
        e=cam_sensor_live.phase==CAM_SENSOR_CONFIGURED?EALREADY:EBUSY;
    if(e) {
        r->last_errno=e;
        if(cam_probe_latched || cam_resources_latched)r->flags=X9_CAM_CFG_LATCHED;
        return e;
    }
    e=cam_sensor_config_preflight(which,r);
    if(e){r->last_errno=e;return e;} /* no table/state writes on source rejection */
    r->flags|=X9_CAM_CFG_ATTEMPTED;
    cam_sensor_live.phase=CAM_SENSOR_CONFIGURING;
    cam_sensor_live.probe.stage=X9_CAM_STAGE_CONFIG;
    ap=(struct cam_sensor_ap){&cam_journal.sensor,cam_sensor_slave(which)};
    io=(struct cam_sensor_io){.context=&ap,.read=cam_sensor_ap_read,.write=cam_sensor_ap_write,
        .read_word=cam_sensor_ap_read_word,.write_word=cam_sensor_ap_write_word};
    e=cam_sensor_open_prepare(which,r);
    if(!e) {
        r->stage=X9_CAM_CFG_EEPROM;
        e=cam_cal_preload(which);
    }
    if(!e && cam_cal_profiles[which-1].header)io.calibrate=cam_cal_apply;
    if(!e)e=cam_sensor_configure(&io,which,r);
    if(!e)e=cam_cal_mode_check(which,r);
    if(!e)e=cam_vcm_prepare(which,r);
    if(!e)e=cam_sensor_ap_guard(&ap); /* supplies still match after final readback */
    if(e){cam_sensor_session_fault(e);r->flags|=X9_CAM_CFG_LATCHED;}
    else cam_sensor_live.phase=CAM_SENSOR_CONFIGURED;
    r->last_errno=e;r->probe=cam_sensor_live.probe;
    cam_sensor_live.config=*r;cam_config_last=*r;
    /* VERIFIED means this powered standby, not CSI/rate/stream readiness. */
    return e;
}
static int cam_sensor_session_close_locked(uint64_t epoch,struct x9_camera_probe *r)
{
    const char *step="vcm-release";
    int off,e=cam_sensor_session_match(epoch);
    if(e)return e; /* stale/second close has no MMIO or journal mutation */
    e=cam_native_sensor_retirement_guard_locked(epoch);
    if(e)return e;
    /* An independent sensor caller cannot remove supplies below a capture
     * attempt. Core stop must clear capture only after actual HW drain. */
    if(session.capture)return EBUSY;
    /* Normal lens release first, then restore the borrowed sensor-open
     * VCM mode, before close marking/reset/MM/supplier teardown. Both need
     * this still-powered AP bus and the independent capture retirement. */
    off=cam_vcm_close(epoch);
    if(!off) {
        step="lens-open-restore";
        off=cam_sensor_open_restore(epoch);
    }
    cam_sensor_live.close_attempted=true;
    cam_sensor_live.phase=CAM_SENSOR_CLOSING;
    /* Do not lower MM clocks while a powered sensor can emit packets, and
     * do not lower sensor supplies after a receiver/MM conflict. This is
     * only retirement of an unprogrammed receive lifetime, not DMA drain. */
    if(!off && cam_rx_sensor_held_locked(epoch)) {
        bool mm2_failed=cam_rx_sensor_mm2_take_failed_locked(epoch);
        step="sensor-isolate";
        off=cam_cold_isolate(&cam_journal);
        if(!off) {
            step="receiver-close";
            off=cam_rx_sensor_close_locked(epoch);
            if(!off || mm2_failed) {
                int restore_err=cam_restore(&cam_journal);
                if(restore_err) {
                    step="supplier-restore";
                    off=restore_err;
                }
            }
        }
    } else if(!off) {
        step="supplier-restore";
        off=cam_restore(&cam_journal);
    }
    if(off)printf("X9 CAMERA SENSOR CLOSE: sensor=%u epoch=%llu step=%s original=%d errno=%d\n",
        cam_sensor_live.probe.sensor,epoch,step,cam_sensor_live.error,off);
    cam_sensor_live.close_error=off;
    cam_sensor_live.probe.restore_errno=off;
    cam_sensor_live.probe.i2c_status[0]=cam_journal.sensor.last_status;
    cam_sensor_live.probe.i2c_status[1]=cam_journal.ldo.last_status;
    if(!off)cam_sensor_live.probe.flags|=X9_CAM_PROBE_RESTORED;
    else {
        cam_probe_latched=true;
        cam_sensor_live.probe.flags|=X9_CAM_PROBE_LATCHED;
        if(!cam_sensor_live.error)cam_sensor_live.probe.stage=X9_CAM_STAGE_RESTORE;
    }
    cam_sensor_live.probe.last_errno=cam_sensor_live.error?cam_sensor_live.error:off;
    if(!cam_sensor_live.probe.last_errno)cam_sensor_live.probe.stage=X9_CAM_STAGE_DONE;
    *r=cam_sensor_live.probe;cam_probe_last=*r;
    if(off)cam_sensor_live.phase=CAM_SENSOR_RETAINED;
    else {
        cam_sensor_live.phase=CAM_SENSOR_EMPTY;cam_sensor_live.epoch=0;
        cam_sensor_session_active=false;
        /* Log 348: a clean ID NACK (ENXIO) latched cam_probe_latched via
         * session_fault, so the next sensor failed with EIO without power.
         * The restore just succeeded (every rail/pin/clock back at entry),
         * so a clean NACK leaves nothing retained: unlatch. */
        if(cam_sensor_live.error==ENXIO && !cam_resources_latched) {
            cam_probe_latched=false;
            cam_sensor_live.error=0;
        }
    }
    /* off is the unwind result, not the earlier open/configuration error. */
    return off;
}
int x9_camera_sensor_session_open(uint32_t which,uint64_t *epoch,struct x9_camera_probe *r)
{
    int e;
    if(!epoch || !r)return EINVAL;
    lck_mtx_lock(&camera_control_lock);
    if(session.power) {
        /* A standalone caller cannot compete with an adapter's power
         * attempt. Its future internal power callback uses _locked after
         * the core has intentionally journaled session.power=true. */
        *epoch=0;memset(r,0,sizeof(*r));r->version=1;r->sensor=which;
        r->last_errno=EBUSY;e=EBUSY;
    } else e=cam_sensor_session_open_locked(which,epoch,r);
    lck_mtx_unlock(&camera_control_lock);
    return e;
}
int x9_camera_sensor_session_configure(uint64_t epoch,struct x9_camera_config *r)
{
    int e;
    if(!r)return EINVAL;
    lck_mtx_lock(&camera_control_lock);
    e=cam_sensor_session_configure_locked(epoch,r);
    lck_mtx_unlock(&camera_control_lock);
    return e;
}
int x9_camera_sensor_session_prepare_receiver(uint64_t epoch,struct x9_camera_receiver *r)
{
    struct cam_sensor_ap ap;
    uint8_t byte=0;
    int e;
    if(!r)return EINVAL;
    lck_mtx_lock(&camera_control_lock);
    memset(r,0,sizeof(*r));r->version=1;r->sensor=cam_sensor_live.probe.sensor;
    e=cam_sensor_session_match(epoch);
    if(!e && cam_sensor_live.error)e=cam_sensor_live.error;
    if(!e && cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED)e=EBUSY;
    if(!e && (session.capture || session.stream))e=EBUSY;
    if(!e && cam_rx_sensor_held_locked(epoch))e=EALREADY;
    if(!e) {
        ap=(struct cam_sensor_ap){&cam_journal.sensor,
            cam_sensor_slave(cam_sensor_live.probe.sensor)};
        e=cam_sensor_ap_read(&ap,0x0100,&byte);
        if(!e && byte)e=EBUSY;
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);
        if(!e && byte)e=EBUSY;
        if(e)cam_sensor_session_fault(e);
        else e=cam_rx_sensor_prepare_locked(epoch,r);
        /* MM take can wait on several voters. Re-prove the powered sensor
         * and consumed receiver after those waits, not only at admission. */
        if(!e) {
            e=cam_sensor_ap_read(&ap,0x0100,&byte);
            if(!e && byte)e=EBUSY;
            if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);
            if(!e && byte)e=EBUSY;
            if(!e)e=cam_sensor_ap_guard(&ap);
            if(e)cam_sensor_session_fault(e);
        }
    }
    r->last_errno=e;
    lck_mtx_unlock(&camera_control_lock);
    return e;
}
int x9_camera_sensor_session_read8(uint64_t epoch,uint16_t reg,uint8_t *v)
{
    struct cam_sensor_ap ap;
    struct x9_camera_config cfg={0};
    const struct x9_camera_config *controls=&cfg;
    bool permitted=reg==0x0016 || reg==0x0017 || reg==0x0100 || reg==0x0104;
    uint8_t byte=0;
    int e;
    if(!v)return EINVAL;
    lck_mtx_lock(&camera_control_lock);
    e=cam_sensor_session_match(epoch);
    if(!e && cam_sensor_live.error)e=cam_sensor_live.error;
    if(!e && cam_sensor_live.phase!=CAM_SENSOR_STANDBY &&
       cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED)e=EBUSY;
    if(!e && !permitted) {
        if(cam_sensor_live.phase==CAM_SENSOR_CONFIGURED)controls=&cam_sensor_live.config;
        else e=cam_sensor_config_preflight(cam_sensor_live.probe.sensor,&cfg);
        for(unsigned i=0;!e && i<X9_CAM_CFG_SAMPLES && controls->sample[i].reg;i++)
            if(controls->sample[i].reg==reg)permitted=true;
        if(!e && !permitted)e=EPERM;
    }
    if(!e) {
        ap=(struct cam_sensor_ap){&cam_journal.sensor,
            cam_sensor_slave(cam_sensor_live.probe.sensor)};
        e=cam_sensor_ap_read(&ap,reg,&byte);
        if(!e && (reg==0x0100 || reg==0x0104) && byte)e=EBUSY;
        if(!e && (reg==0x0016 || reg==0x0017) &&
           byte!=(uint8_t)(cam_sensor_live.probe.chip_id>>(reg==0x0016?8:0)))e=ENODEV;
        if(!e)e=cam_sensor_ap_guard(&ap);
        if(e)cam_sensor_session_fault(e);
        else *v=byte;
    }
    lck_mtx_unlock(&camera_control_lock);
    return e;
}
int x9_camera_sensor_session_close(uint64_t epoch,struct x9_camera_probe *r)
{
    int e;
    if(!r)return EINVAL;
    lck_mtx_lock(&camera_control_lock);
    e=cam_sensor_session_close_locked(epoch,r);
    lck_mtx_unlock(&camera_control_lock);
    return e;
}
/* Existing diagnostic probe: same engine, ONE immediate unwind. Persistent
 * native callers do not consume the diagnostic once-per-sensor success bit. */
static int cam_sensor_probe(uint32_t which,struct x9_camera_probe *r)
{
    uint64_t epoch=0;
    int e,off=0;
    if(!x9_camera_sensor_valid(which))return EINVAL;
    if(cam_probe_done&(1u<<which)) {
        memset(r,0,sizeof(*r));r->version=1;r->sensor=which;r->last_errno=EALREADY;
        return EALREADY;
    }
    e=cam_sensor_session_open_locked(which,&epoch,r);
    if(epoch)off=cam_sensor_session_close_locked(epoch,r);
    r->last_errno=e?e:off;
    if(!e && !off)cam_probe_done|=1u<<which;
    if(epoch)cam_sensor_cold_log(r);
    return e?e:off;
}
