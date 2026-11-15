/* Real AP group-held controls at the repeat CPU/no-credit boundary. Neither
 * queued commands nor register ACKs change the applied frame metadata. The
 * hhmain/hhfront modes declare ae_effective_frame=2, frame_time_delay_frame=3:
 * after release/readback, observe THREE actual sensor frame-counter advances
 * with no available credit before binding these controls to a later request.
 * Settling packets never become RAW frames. Source stays genuinely running;
 * no estimated SOF, fake completion, stream restart or blind group retry.
 */
#include "x9_camera_native_ae.h"
struct cam_native_ae_controls { uint32_t lines,gain,frame_lines; };
static struct {
    uint64_t epoch,queued_serial,applied_serial;
    uint32_t sensor,writes,advances;
    uint8_t counter;
    bool seeded,pending,hold_attempted,release_attempted,hold_ack,release_ack;
    int error;
    struct cam_native_ae_controls applied,wanted;
} cam_native_ae;

static int cam_native_ae_fault(int e)
{
    if(!cam_native_ae.error)cam_native_ae.error=e;
    return cam_native_ae.error;
}
static int cam_native_ae_match(uint64_t epoch)
{
    int e=cam_sensor_session_match(epoch);
    if(!e && (!cam_native_ae.seeded || epoch!=cam_native_ae.epoch ||
       cam_sensor_live.probe.sensor!=cam_native_ae.sensor ||
       cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED))e=ESTALE;
    return e?e:cam_native_ae.error;
}
static int cam_native_ae_validate(const struct x9_camera_sensor *s,
    struct cam_native_ae_controls c)
{
    struct x9_camera_completed_frame f={.exposure_lines=c.lines,
        .gain_code=c.gain,.frame_lines=c.frame_lines};
    return cam_completed_controls(s,&f);
}
/* Coherent actual control samples, not config.sample.expected. */
static int cam_native_ae_read(struct cam_sensor_ap *ap,
    struct cam_native_ae_controls *out,uint8_t stream)
{
    const uint16_t regs[]={0x0340,0x0341,0x0202,0x0203,0x0204,0x0205};
    uint8_t v[6],byte;
    int e=cam_sensor_ap_read(ap,0x0100,&byte);
    if(!e && byte!=stream)e=ESTALE;
    if(!e)e=cam_sensor_ap_read(ap,0x0104,&byte);
    if(!e && byte)e=EBUSY;
    for(unsigned i=0;i<6 && !e;i++)e=cam_sensor_ap_read(ap,regs[i],&v[i]);
    if(!e)e=cam_sensor_ap_read(ap,0x0104,&byte);
    if(!e && byte)e=EBUSY;
    if(!e) *out=(struct cam_native_ae_controls){
        .frame_lines=((uint32_t)v[0]<<8)|v[1],.lines=((uint32_t)v[2]<<8)|v[3],
        .gain=((uint32_t)v[4]<<8)|v[5]};
    return e;
}
static int cam_native_ae_read_stable(struct cam_sensor_ap *ap,
    struct cam_native_ae_controls *out,uint8_t stream)
{
    struct cam_native_ae_controls a,b;
    int e=cam_native_ae_read(ap,&a,stream);
    if(!e)e=cam_native_ae_read(ap,&b,stream);
    if(!e && memcmp(&a,&b,sizeof(a)))e=ESTALE;
    if(!e)*out=a;
    return e;
}
static int cam_native_ae_seed_locked(uint64_t epoch,const struct x9_camera_config *cfg)
{
    struct cam_native_ae_controls c;
    uint32_t which=cam_sensor_live.probe.sensor;
    struct cam_sensor_ap ap={&cam_journal.sensor,which==X9_CAMERA_BACK?0x1a:0x10};
    int e=cam_sensor_session_match(epoch);
    if(!e && (!cfg || which<X9_CAMERA_BACK || which>X9_CAMERA_FRONT ||
       cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED || session.stream ||
       !(cfg->flags&X9_CAM_CFG_VERIFIED) || cfg->last_errno ||
       memcmp(cfg,&cam_sensor_live.config,sizeof(*cfg))))e=ESTALE;
    if(!e && cam_native_ae.seeded && cam_native_ae.epoch==epoch)e=EALREADY;
    if(e)return e;
    e=cam_native_ae_read_stable(&ap,&c,0);
    if(!e)e=cam_native_ae_validate(&sensors[which-1],c);
    if(e)return e;
    /* A new physically admitted cold epoch, never a retry of ambiguous AE. */
    bzero(&cam_native_ae,sizeof(cam_native_ae));
    cam_native_ae.epoch=epoch;cam_native_ae.sensor=which;
    cam_native_ae.applied=cam_native_ae.wanted=c;cam_native_ae.seeded=true;
    return 0;
}
static int cam_native_ae_snapshot_locked(uint64_t epoch,struct x9_camera_completed_frame *f)
{
    int e=cam_native_ae_match(epoch);
    if(!f)return EINVAL;
    if(e)return e;
    f->exposure_lines=cam_native_ae.applied.lines;
    f->gain_code=cam_native_ae.applied.gain;
    f->frame_lines=cam_native_ae.applied.frame_lines;
    return 0; /* No identity, time, generation or request cookie edits. */
}
static int cam_native_ae_queue_locked(uint64_t epoch,uint32_t lines,uint32_t gain,
    uint32_t frame_lines)
{
    const struct x9_camera_sensor *s;
    struct cam_native_ae_controls c;
    int e=cam_native_ae_match(epoch);
    if(e)return e;
    if(!session.capture || !session.stream)return ENXIO;
    if(gain<1024 || gain>65536)return EINVAL;
    s=&sensors[cam_native_ae.sensor-1];
    c=(struct cam_native_ae_controls){.lines=lines,.frame_lines=frame_lines,
        .gain=(uint32_t)((uint64_t)s->gain_unity*(gain-1024)/gain)};
    if(cam_native_ae_validate(s,c))return EINVAL;
    if(cam_native_ae.queued_serial==UINT64_MAX)return EOVERFLOW;
    cam_native_ae.wanted=c;cam_native_ae.queued_serial++;cam_native_ae.pending=true;
    return 0; /* Admission/coalescing only; applied remains unchanged. */
}
static int cam_native_ae_boundary(uint64_t epoch,uint64_t end)
{
    int e=cam_native_ae_match(epoch);
    if(!e && mach_absolute_time()>=end)e=ETIMEDOUT;
    if(!e)e=cam_repeat_cpu_boundary_locked(epoch);
    return e;
}
/* Narrow owned AE transaction; generic table write retains its 0104 ban. */
static int cam_native_ae_write(struct cam_sensor_ap *ap,uint64_t epoch,uint64_t end,
    uint16_t reg,uint8_t value)
{
    uint8_t tx[3]={(uint8_t)(reg>>8),(uint8_t)reg,value};
    int e;
    if(reg!=0x0104 && reg!=0x0340 && reg!=0x0341 &&
       reg!=0x0202 && reg!=0x0203 && reg!=0x0204 && reg!=0x0205)return EPERM;
    e=cam_native_ae_boundary(epoch,end);
    if(!e)e=cam_sensor_ap_guard(ap);
    if(e)return e;
    /* BEFORE possible posted START; failures retain the exact partial group. */
    if(reg==0x0104) {
        if(value==1)cam_native_ae.hold_attempted=true;
        else if(!value)cam_native_ae.release_attempted=true;
        else return EINVAL;
    }
    cam_native_ae.writes++;
    e=cam_bus_transfer(ap->bus,ap->slave,tx,sizeof(tx),NULL);
    if(!e && reg==0x0104) {
        if(value)cam_native_ae.hold_ack=true;
        else cam_native_ae.release_ack=true;
    }
    if(!e)e=cam_native_ae_boundary(epoch,end);
    return e;
}
static int cam_native_ae_next_locked(uint64_t epoch,struct x9_camera_completed_frame *f)
{
    struct cam_native_ae_controls observed,c;
    struct cam_sensor_ap ap={&cam_journal.sensor,cam_native_ae.sensor==X9_CAMERA_BACK?0x1a:0x10};
    const uint16_t regs[]={0x0340,0x0341,0x0202,0x0203,0x0204,0x0205};
    uint64_t span,end;
    uint8_t values[6],byte=0,previous=0;
    int e;
    if(!f)return EINVAL;
    nanoseconds_to_absolutetime(2000000000ull,&span);end=mach_absolute_time()+span;
    e=cam_native_ae_boundary(epoch,end);
    if(!e)e=cam_native_ae_read_stable(&ap,&observed,1);
    if(!e && memcmp(&observed,&cam_native_ae.applied,sizeof(observed)))e=ESTALE;
    if(e)return cam_native_ae_fault(e);
    if(!cam_native_ae.pending)return cam_native_ae_snapshot_locked(epoch,f);
    c=cam_native_ae.wanted;
    if(!memcmp(&c,&cam_native_ae.applied,sizeof(c))) {
        cam_native_ae.pending=false;cam_native_ae.applied_serial=cam_native_ae.queued_serial;
        return cam_native_ae_snapshot_locked(epoch,f);
    }
    cam_native_ae.writes=cam_native_ae.advances=0;
    cam_native_ae.hold_attempted=cam_native_ae.release_attempted=false;
    cam_native_ae.hold_ack=cam_native_ae.release_ack=false;
    values[0]=(uint8_t)(c.frame_lines>>8);values[1]=(uint8_t)c.frame_lines;
    values[2]=(uint8_t)(c.lines>>8);values[3]=(uint8_t)c.lines;
    values[4]=(uint8_t)(c.gain>>8);values[5]=(uint8_t)c.gain;
    e=cam_native_ae_write(&ap,epoch,end,0x0104,1);
    if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);
    if(!e && byte!=1)e=ESTALE;
    for(unsigned i=0;i<6 && !e;i++)e=cam_native_ae_write(&ap,epoch,end,regs[i],values[i]);
    if(!e)e=cam_native_ae_write(&ap,epoch,end,0x0104,0);
    if(!e)e=cam_native_ae_read_stable(&ap,&observed,1);
    if(!e && memcmp(&observed,&c,sizeof(c)))e=ESTALE;
    if(!e)e=cam_sensor_ap_read(&ap,0x0005,&previous);
    if(!e && previous==0xff)e=ESTALE;
    while(!e && cam_native_ae.advances<3) {
        e=cam_native_ae_boundary(epoch,end);if(e)break;
        e=cam_sensor_ap_read(&ap,0x0005,&byte);if(e)break;
        if(byte==0xff){e=ESTALE;break;} /* OFF sentinel, not a frame. */
        if(byte!=previous) {
            /* Count real observed forward transitions, not a guessed rate.
             * Permit FF-skipping wrap; reject backwards/ambiguous half-wrap. */
            unsigned delta=byte>previous?byte-previous:255u+byte-previous;
            if(!delta || delta>=128){e=ESTALE;break;}
            cam_native_ae.advances++;previous=byte;cam_native_ae.counter=byte;
        }
        if(cam_native_ae.advances<3)delay(1000); /* IRQs remain enabled. */
    }
    if(!e)e=cam_native_ae_read_stable(&ap,&observed,1);
    if(!e && memcmp(&observed,&c,sizeof(c)))e=ESTALE;
    if(!e)e=cam_native_ae_boundary(epoch,end);
    if(!e) {
        cam_native_ae.applied=c;cam_native_ae.pending=false;
        cam_native_ae.applied_serial=cam_native_ae.queued_serial;
        e=cam_native_ae_snapshot_locked(epoch,f);
    }
    printf("X9 CAMERA AE: epoch=%llu serial=%llu writes=%u hold=%u/%u release=%u/%u "
        "advances=%u counter=%x lines=%u gain_code=%u frame_lines=%u errno=%d "
        "(no-credit settling; request metadata only after actual counter progress)\n",
        epoch,cam_native_ae.queued_serial,cam_native_ae.writes,cam_native_ae.hold_attempted,
        cam_native_ae.hold_ack,cam_native_ae.release_attempted,cam_native_ae.release_ack,
        cam_native_ae.advances,cam_native_ae.counter,c.lines,c.gain,c.frame_lines,e);
    return e?cam_native_ae_fault(e):0;
}
