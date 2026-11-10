/* Actual native sensor 0100 commands, sole control + IRQ mutex owner.
 * ISP8s adaptor streaming_control and hhmain/hhfront reg_addr_stream=0100,
 * reg_addr_frame_count=0005. Use the already-owned AP FIFO transaction,
 * never grant 0100 to the generic table writer or call a no-op backend.
 * Single immutable S1/credit/request; no replay, new credit, reset or map
 * release. Sensor OFF/FF is not complete RAW/AXI/IRQ/worker/map retirement.
 */
static int cam_stream_guard_locked(const struct x9_camera_sensor *s,bool before_on)
{
    boolean_t old;
    int e;
    if(s->sensor!=X9_CAMERA_BACK && s->sensor!=X9_CAMERA_FRONT)return EINVAL;
    if(s!=&sensors[s->sensor-1] || !session.capture || !session.stream ||
       cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED || cam_sensor_live.close_attempted ||
       !cam_source.captured || cam_source.phase!=CAM_SOURCE_ARMED || cam_source.error ||
       cam_source.pending!=CAM_SOURCE_STEPS || cam_source.epoch!=cam_sensor_live.epoch ||
       cam_sv.epoch!=cam_source.epoch || cam_sv.status.sensor!=s->sensor)return ESTALE;
    old=cam_irq_spin_take();
    e=cam_irq_fbc.error?cam_irq_fbc.error:cam_irq_fbc.source_error;
    if(!e && (!cam_irq_fbc.bound || cam_irq_fbc.epoch!=cam_source.epoch ||
       cam_irq.epoch!=cam_source.epoch || !cam_irq.prepared || !cam_irq.owns ||
       cam_irq.close_attempted || cam_irq_fbc.request.frame.sensor!=s->sensor ||
       cam_irq_fbc.request.frame.generation!=session.generation ||
       memcmp(&cam_irq_fbc.request,&cam_sv.request,sizeof(cam_sv.request))))e=ESTALE;
    cam_irq_spin_drop(old);if(e)return e;
    e=cam_sv_request_guard_locked(cam_source.epoch,cam_irq.slot);if(e)return e;
    e=cam_irq_mapping();if(!e)e=cam_irq_power();if(!e)e=cam_irq_wait_rwp();
    if(!e)e=cam_irq_config(false);
    if(!e && before_on)e=cam_irq_credit_guard_locked(cam_source.epoch);
    return e;
}
static int cam_stream_control_note_locked(uint8_t value,bool off)
{
    uint32_t bit=off?CAM_STREAM_OFF:CAM_STREAM_ON;
    uint8_t prior=off?1:0,target=off?0:1;
    boolean_t old=cam_irq_spin_take();
    int e=cam_irq_fbc.source_error;
    cam_irq_fbc.source_control=value;
    cam_irq_fbc.source_sampled|=1;
    if(!e && (!(cam_irq_fbc.source_issued&bit) ||
       cam_irq_fbc.source_phase!=(off?CAM_STREAM_OFF_PENDING:CAM_STREAM_ON_PENDING)))e=ESTALE;
    if(!e && value!=target && ((cam_irq_fbc.source_seen&bit) || value!=prior))e=ESTALE;
    if(!e) {
        if(value==target)cam_irq_fbc.source_seen|=bit;
        cam_irq_fbc.source_samples=value==target?cam_irq_fbc.source_samples+1:0;
    }
    if(e && !cam_irq_fbc.source_error)cam_irq_fbc.source_error=e;
    cam_irq_spin_drop(old);return e;
}
/* Read/record after the real AP transfer, BEFORE any next control guard
 * can observe a later counter or IRQ. No native callback under spin. */
static int cam_stream_read_control_locked(struct cam_sensor_ap *ap,bool off)
{
    uint8_t byte;
    int e=cam_sensor_ap_read(ap,0x0100,&byte);
    return e?e:cam_stream_control_note_locked(byte,off);
}
static int cam_stream_set_locked(const struct x9_camera_sensor *s,uint8_t value)
{
    const bool off=value==0;
    const uint32_t bit=off?CAM_STREAM_OFF:CAM_STREAM_ON;
    struct cam_sensor_ap ap;
    uint64_t span,end=0;
    boolean_t old;
    uint8_t byte=0,control=0,frame_count=0,prior_control=0;
    uint32_t issued=0,ack=0,seen=0,samples=0,phase=0,idle_samples=0,sampled=0;
    bool idle_seen=false,command=false;
    int e;
    if(!s || value>1)return EINVAL;
    lck_mtx_lock(&camera_irq_lock);
    e=cam_stream_guard_locked(s,!off);if(e)goto out;
    old=cam_irq_spin_take();
    if(cam_irq_fbc.source_issued&bit)e=EBUSY;
    else if(off) {
        if(cam_irq_fbc.source_phase!=CAM_STREAM_RUNNING ||
           !(cam_irq_fbc.source_ack&CAM_STREAM_ON))e=ESTALE;
    } else if(cam_irq_fbc.source_phase!=CAM_STREAM_IDLE || cam_irq_fbc.source_issued)e=ESTALE;
    cam_irq_spin_drop(old);if(e)goto out;
    ap=(struct cam_sensor_ap){&cam_journal.sensor,s->sensor==X9_CAMERA_BACK?0x1a:0x10};
    /* Fresh actual prior/hold observations. Idle-on additionally checks
     * the vendor frame-count OFF sentinel; no stable arbitrary rebase. */
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_stream_guard_locked(s,!off);if(e)goto out;
        e=cam_sensor_ap_read(&ap,0x0100,&byte);if(e)goto out;
        old=cam_irq_spin_take();cam_irq_fbc.source_prior_control=byte;
        cam_irq_fbc.source_sampled|=4;cam_irq_spin_drop(old);
        if(byte!=(off?1:0)){e=ESTALE;goto out;}
        e=cam_sensor_ap_read(&ap,0x0104,&byte);if(e)goto out;
        if(byte){e=EBUSY;goto out;}
        if(!off) {
            e=cam_sensor_ap_read(&ap,0x0005,&byte);if(e)goto out;
            if(byte!=0xff){e=EBUSY;goto out;}
        }
    }
    e=cam_stream_guard_locked(s,!off);if(e)goto out;
    /* Fixed bounded observation, not a FPS/rate lease inferred from pclk.
     * Budget includes actual AP write/read transactions and last guards. */
    nanoseconds_to_absolutetime(500000000,&span);end=mach_absolute_time()+span;
    old=cam_irq_spin_take();
    if(cam_irq_fbc.source_error || cam_irq_fbc.error || (cam_irq_fbc.source_issued&bit))
        e=cam_irq_fbc.source_error?cam_irq_fbc.source_error:cam_irq_fbc.error?cam_irq_fbc.error:ESTALE;
    if(!e) {
        cam_irq_fbc.source_issued|=bit;cam_irq_fbc.source_samples=0;
        cam_irq_fbc.source_sampled&=4;cam_irq_fbc.source_frame_count=0;
        cam_irq_fbc.source_phase=off?CAM_STREAM_OFF_PENDING:CAM_STREAM_ON_PENDING;
        if(off)cam_irq_fbc.source_off_time=mach_absolute_time();
        else cam_irq_fbc.source_on_time=mach_absolute_time();
        command=true;
    }
    cam_irq_spin_drop(old);if(e)goto out;
    e=cam_sensor_ap_guard(&ap);if(e)goto out;
    if(mach_absolute_time()>=end){e=ETIMEDOUT;goto out;}
    {
        const uint8_t tx[3]={0x01,0x00,value};
        /* Already journaled BEFORE the possibly posted AP START. Generic
         * cam_sensor_ap_write still forbids stream/hold/reset. The existing
         * FIFO engine proves real stopped COMP/NACK retirement; no retry. */
        e=cam_bus_transfer(ap.bus,ap.slave,tx,sizeof(tx),NULL);if(e)goto out;
    }
    for(unsigned poll=0;poll<500;poll++) {
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
        e=cam_stream_guard_locked(s,false);if(e)break;
        e=cam_stream_read_control_locked(&ap,off);if(e)break;
        old=cam_irq_spin_take();samples=cam_irq_fbc.source_samples;cam_irq_spin_drop(old);
        if(samples>=2) {
            if(!off)break;
            e=cam_sensor_ap_read(&ap,0x0005,&byte);if(e)break;
            old=cam_irq_spin_take();cam_irq_fbc.source_frame_count=byte;
            cam_irq_fbc.source_sampled|=2;cam_irq_spin_drop(old);
            if(idle_seen && byte!=0xff){e=ESTALE;break;}
            if(byte==0xff){idle_seen=true;idle_samples++;}
            if(idle_samples==2)break;
        }
        delay(1000);
    }
    if(!e && (samples<2 || (off && idle_samples!=2)))e=ETIMEDOUT;
    if(!e)e=cam_stream_guard_locked(s,false);
    if(!e)e=cam_sensor_ap_guard(&ap);
    if(!e) {
        e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e && byte)e=EBUSY;
    }
    if(!e)e=cam_stream_read_control_locked(&ap,off);
    if(!e)e=cam_stream_guard_locked(s,false);
    if(!e && off) {
        e=cam_sensor_ap_read(&ap,0x0005,&byte);
        if(!e) {
            old=cam_irq_spin_take();cam_irq_fbc.source_frame_count=byte;
            cam_irq_fbc.source_sampled|=2;cam_irq_spin_drop(old);
            if(byte!=0xff)e=ESTALE;
        }
        if(!e)e=cam_stream_guard_locked(s,false);
    }
    if(!e && mach_absolute_time()>=end)e=ETIMEDOUT;
    if(!e) {
        old=cam_irq_spin_take();
        if(cam_irq_fbc.error || cam_irq_fbc.source_error)
            e=cam_irq_fbc.error?cam_irq_fbc.error:cam_irq_fbc.source_error;
        else {
            cam_irq_fbc.source_ack|=bit;
            cam_irq_fbc.source_phase=off?CAM_STREAM_STOPPED:CAM_STREAM_RUNNING;
        }
        cam_irq_spin_drop(old);
    }
out:
    old=cam_irq_spin_take();
    if(e && command && !cam_irq_fbc.source_error)cam_irq_fbc.source_error=e;
    issued=cam_irq_fbc.source_issued;ack=cam_irq_fbc.source_ack;seen=cam_irq_fbc.source_seen;
    samples=cam_irq_fbc.source_samples;phase=cam_irq_fbc.source_phase;
    sampled=cam_irq_fbc.source_sampled;
    prior_control=cam_irq_fbc.source_prior_control;
    control=cam_irq_fbc.source_control;frame_count=cam_irq_fbc.source_frame_count;
    cam_irq_spin_drop(old);
    /* Failure cannot be repaired with an unissued opposite command. Keep
     * the original stream attempt, RAW slot, credit, map and power pinned. */
    if(e && command)e=cam_irq_fault(e);
    printf("X9 CAMERA SENSOR STREAM: epoch=%llu sensor=%u off=%u phase=%u issued=%x ack=%x "
        "seen=%x prior_control=%x control=%x frame_count=%x sampled=%x samples=%u idle_samples=%u errno=%d "
        "(real AP 0100; NOT RAW/AXI/IRQ/worker/map drain)\n",
        cam_source.epoch,s->sensor,off,phase,issued,ack,seen,prior_control,control,frame_count,sampled,samples,idle_samples,e);
    lck_mtx_unlock(&camera_irq_lock);return e;
}
