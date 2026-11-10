/* MT6993 camera sensor/control + bounded RAW10 ingress, default disabled.
 * Live Android DT/log: sensor0 hhmain on i2c4/0x1a; sensor1 hhfront on
 * i2c2/0x10. OPPO src-isp8s + hh{main,front} driver data, NOT redwood MMIO.
 * Platform power/I2C/SENINF/RAW DMA is deliberately not fabricated here.
 * HakiOS userland RAW10 ABI is MIT; platform status/control is X9-specific.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <sys/file_internal.h>
#include <sys/uio.h>
#include <kern/kalloc.h>
#include <miscfs/devfs/devfs.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include "x9_camera.h"
#include "../../../../camera/hakios.camera.h"
#include "../../../generated/x9_camera_sensor_tables.inc"

extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *, int);
LCK_GRP_DECLARE(camera_group, "x9-camera");
LCK_MTX_DECLARE(camera_lock, &camera_group);
/* Controls serialize separately so capture(off) may drain a completion worker
 * without holding the mutex that worker needs to reject its stale frame. */
LCK_MTX_DECLARE(camera_control_lock, &camera_group);
static struct x9_camera_status st = { .version = 1, .sensor = X9_CAMERA_BACK };
static struct x9_camera_platform hw;
static bool opened;
static uint8_t *frame;
static uint32_t frame_bytes;
/* Pin CPU staging across faultable uiomove, as HakiOS pins PLACE_READING.
 * This is NOT a DMA mapping/ownership counter. A busy staging buffer drops
 * the next delivery rather than blocking a hardware completion on user VM. */
static unsigned frame_readers;
static uint64_t consumed;
static struct hakios_camera_frame head;
/* Source completion order is independent of accepted userland frame count.
 * Retire even a backpressured frame so it cannot be replayed after a read. */
static uint64_t last_capture_id, last_capture_time;
_Static_assert(sizeof(struct x9_camera_completed_frame)==64,"kernel completion record");
/* Journal attempted callbacks BEFORE invoking them: an error may follow a
 * partial state change. Kept after failed drain/restore; no retry on close. */
static struct {
    bool power, capture, stream, latched;
    uint64_t generation;
    int last_errno;
} session;
static const struct x9_camera_power_step main_power[] = {
    {X9_CAM_MCLK,24,0}, {X9_CAM_RESET,0,1000},
    {X9_CAM_AVDD,2804000,3000}, {X9_CAM_AVDD1,1804000,3000},
    {X9_CAM_AFVDD,3000000,3000}, {X9_CAM_DVDD,1160000,4000},
    {X9_CAM_DOVDD,1800000,3000}, {X9_CAM_DRIVE,4,6000}, {X9_CAM_RESET,1,4000}
};
static const struct x9_camera_power_step front_power[] = {
    {X9_CAM_MCLK,24,0}, {X9_CAM_SCL_AP,0,0}, {X9_CAM_SDA_AP,0,0},
    {X9_CAM_RESET,0,0}, {X9_CAM_AVDD,2900000,1000},
    {X9_CAM_DOVDD,1800000,1000}, {X9_CAM_DVDD,1104000,1000},
    {X9_CAM_DRIVE,4,1000}, {X9_CAM_RESET,1,4000}
};
static const struct x9_camera_sensor sensors[] = {
    /* main physical ID 0x9186: vendor maps it to HHMAIN_SENSOR_ID, which is
     * a software ID, NOT the value of the chip ID registers 0x16/0x17. */
    {1,4,0x1a,0x9186,4096,3072,3,4415,7546,65532,2,6,16384,16128,1,3,
     main_power, sizeof(main_power)/sizeof(main_power[0])},
    {2,2,0x10,0x0615,3264,2448,0,4361,7642,65530,1,16,1024,1008,0,4,
     front_power, sizeof(front_power)/sizeof(front_power[0])}
};
#include "x9_camera_pipeline.inc"
#include "x9_camera_s1.inc"
#include "x9_camera_smmu.inc"
#include "x9_camera_sensor.inc"
#include "x9_camera_mt6993.inc"
#include "x9_camera_resources.inc"
#include "x9_camera_sensor_session.inc"
#include "x9_camera_standby.inc"
#include "x9_camera_mm.inc"
#include "x9_camera_receiver.inc"
_Static_assert(sizeof(struct x9_camera_status)==88,"camera status ABI");
static const struct x9_camera_sensor *sensor(void) { return &sensors[st.sensor-1]; }
static int read16(const struct x9_camera_sensor *s, uint16_t reg, uint32_t *v)
{
    uint8_t hi,lo; int e=hw.read8(s,reg,&hi);
    if(!e)e=hw.read8(s,reg+1,&lo); if(!e)*v=((uint32_t)hi<<8)|lo; return e;
}
static int write8(const struct x9_camera_sensor *s, uint16_t reg, uint8_t v)
{
    int e=hw.write8(s,reg,v); uint8_t back;
    if(!e)e=hw.read8(s,reg,&back); return e?e:back==v?0:EIO;
}
static int write16(const struct x9_camera_sensor *s, uint16_t reg, uint32_t v)
{
    int e=write8(s,reg,(uint8_t)(v>>8)); return e?e:write8(s,reg+1,(uint8_t)v);
}
static int cam_sensor_hw_read(void *s, uint16_t reg, uint8_t *v)
{ return hw.read8(s,reg,v); }
static int cam_sensor_hw_write(void *s, uint16_t reg, uint8_t v)
{ return hw.write8(s,reg,v); }
static int cam_drain_readers(void)
{
    uint64_t span;nanoseconds_to_absolutetime(1000000000,&span);
    uint64_t deadline=mach_absolute_time()+span;
    while(frame_readers) {
        uint64_t now=mach_absolute_time(),ns;
        if(now>=deadline)return ETIMEDOUT;
        absolutetime_to_nanoseconds(deadline-now,&ns);
        struct timespec timeout={(time_t)(ns/1000000000),(long)(ns%1000000000)};
        int e=msleep(&frame_readers,&camera_lock,PZERO,"x9-camera-copy",&timeout);
        if(e && frame_readers)return e==EWOULDBLOCK?ETIMEDOUT:e;
    }
    return 0;
}
static int stop(void)
{
    int e=0,a=0,b=0;
    const struct x9_camera_sensor *s=sensor();
    if(session.latched)return session.last_errno; /* no stale generation/retry */
    st.generation++; st.flags&=~X9_CAMERA_STREAMING; frame_bytes=0; consumed=st.frames;
    wakeup(&frame_bytes);
    if(session.power || session.capture || session.stream) {
        lck_mtx_unlock(&camera_lock);
        if(session.stream)e=write8(s,0x0100,0);
        if(session.capture)a=hw.capture(s,0,session.generation);
        /* Publish confirmed capture retirement before power(false). Native
         * sensor unwind must still see capture=true after failed/partial
         * drain; it cannot rely on a caller-supplied "drained" boolean. */
        lck_mtx_lock(&camera_lock);
        if(!a)session.capture=false;
        lck_mtx_unlock(&camera_lock);
        /* Never remove sensor/MM/translation dependencies below a DMA
         * engine whose stop/drain failed. Provider retains its own journal. */
        if(!a && session.power)b=hw.power(s,0);
        lck_mtx_lock(&camera_lock);
        if(!a && !b){session.power=false;session.stream=false;}
        if(a || b) {
            session.latched=true;session.last_errno=a?a:b;
            st.flags|=X9_CAMERA_RETAINED;
        }
        printf("X9 CAMERA DRAIN: sensor=%u generation=%llu stream=%d drain=%d restore=%d "
            "retained=%u/%u/%u latched=%u\n",s->sensor,session.generation,e,a,b,
            session.power,session.capture,session.stream,session.latched);
    }
    int copy=cam_drain_readers();
    if(copy) {
        session.latched=true;if(!session.last_errno)session.last_errno=copy;
        st.flags|=X9_CAMERA_RETAINED;
        printf("X9 CAMERA COPY DRAIN: readers=%u errno=%d (CPU staging retained, no retry)\n",
            frame_readers,copy);
        if(!e && !a && !b)e=copy;
    }
    if(e||a||b){st.flags|=X9_CAMERA_FAULT;st.last_errno=e?e:a?a:b;}
    return e?e:a?a:b;
}
static int start(uint32_t which, uint32_t mode)
{
    if(which!=1 && which!=2)return EINVAL;
    if(mode && mode!=UINT32_MAX)return ENOTSUP;
    if(!(st.flags&X9_CAMERA_READY) || (st.flags&X9_CAMERA_FAULT))return ENXIO;
    if((st.flags&X9_CAMERA_STREAMING) && which==st.sensor)return 0;
    int e=stop(); if(e)return e;
    st.sensor=which; const struct x9_camera_sensor *s=sensor(); uint32_t id;
    struct x9_camera_config config={.version=1,.sensor=which};
    e=cam_sensor_config_preflight(which,&config);
    if(e){st.last_errno=e;return e;} /* reject missing/mismatched tables before power */
    if(!frame)frame=kalloc_data(X9_CAMERA_MAX_BYTES,Z_WAITOK);
    if(!frame)return ENOMEM;
    session.generation=st.generation;
    last_capture_id=last_capture_time=0;
    session.power=true;
    /* Worker completions can enter camera_deliver during capture setup.
     * Keep control serialization, but never hold the worker's frame lock
     * across power/I2C/capture callbacks. */
    lck_mtx_unlock(&camera_lock);
    e=hw.power(s,1);
    if(!e)e=read16(s,0x0016,&id);
    if(!e && id!=s->chip_id)e=ENODEV;
    struct cam_sensor_io io={.context=(void *)s,.read=cam_sensor_hw_read,.write=cam_sensor_hw_write};
    if(!e)e=cam_sensor_configure(&io,which,&config);
    if(!e){session.capture=true;e=hw.capture(s,1,session.generation);}
    if(!e){session.stream=true;e=write8(s,0x0100,1);}
    lck_mtx_lock(&camera_lock);
    if(e) {
        st.flags|=X9_CAMERA_FAULT;st.last_errno=e;
        int cleanup=stop();st.last_errno=e;
        printf("X9 CAMERA: start sensor=%u errno=%d cleanup=%d retained=%u\n",
            which,e,cleanup,session.latched);
        return e;
    }
    st.flags|=X9_CAMERA_STREAMING;st.width=s->width;st.height=s->height;st.last_errno=0;
    printf("X9 CAMERA: start sensor=%u generation=%llu (waiting for physical DMA)\n",which,st.generation);
    return 0;
}
int x9_camera_register_platform(const struct x9_camera_platform *p)
{
    if(!p || p->version!=X9_CAMERA_PLATFORM_VERSION || !p->power || !p->read8 || !p->write8 || !p->capture)return EINVAL;
    struct x9_camera_config config={0};
    int tables=cam_sensor_config_preflight(1,&config);
    if(!tables)tables=cam_sensor_config_preflight(2,&config);
    if(tables)return tables; /* including preview/PLL, BEFORE any power callback */
    lck_mtx_lock(&camera_control_lock);lck_mtx_lock(&camera_lock); int e=0;
    if(st.mode!=2)e=EPERM;
    else if(cam_sensor_session_active)e=EBUSY;
    else if(hw.version)e=EBUSY;
    else if(!X9_HHMAIN_INIT_COUNT || !X9_HHFRONT_INIT_COUNT)e=ENOTSUP;
    else {
        hw=*p;
        for(unsigned i=0;i<2 && !e;i++) {
            st.sensor=sensors[i].sensor;
            lck_mtx_unlock(&camera_lock);
            uint32_t id=0; e=hw.power(&sensors[i],1);
            if(!e)e=read16(&sensors[i],0x16,&id);
            int off=hw.power(&sensors[i],0);
            lck_mtx_lock(&camera_lock);
            if(off) {
                session.power=true;session.latched=true;session.last_errno=off;
                st.flags|=X9_CAMERA_RETAINED;
            }
            if(!e)e=off; if(!e && id!=sensors[i].chip_id)e=ENODEV;
            if(!e)st.chip_id[i]=id;
        }
        if(!e){st.sensor=X9_CAMERA_BACK;st.supported_mask=(1u<<1)|(1u<<2);st.flags=X9_CAMERA_READY;}
        else {st.flags|=X9_CAMERA_FAULT;st.last_errno=e;}
    }
    lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);return e;
}
static int cam_completed_layout(const struct x9_camera_sensor *s,
    const struct x9_camera_completed_frame *f)
{
    return f->version!=X9_CAMERA_PLATFORM_VERSION || f->reserved || !f->raw ||
        f->stride!=((s->width/4*5+63)&~63u) ||
        (uint64_t)f->stride*s->height!=f->bytes || f->bytes>X9_CAMERA_MAX_BYTES ? EINVAL:0;
}
static int cam_completed_controls(const struct x9_camera_sensor *s,
    const struct x9_camera_completed_frame *f)
{
    return f->frame_lines<s->frame_lines || f->frame_lines>s->frame_max ||
        f->frame_lines%s->exposure_step || f->exposure_lines<s->exposure_min ||
        f->exposure_lines>f->frame_lines-48 || f->exposure_lines%s->exposure_step ||
        f->gain_code>s->gain_max ? EIO:0;
}
int x9_camera_deliver(const struct x9_camera_completed_frame *completed)
{
    if(!completed)return EINVAL;
    /* Snapshot the record, not a mutable provider structure during delivery.
     * raw itself must remain immutable/cache-visible through this call. */
    const struct x9_camera_completed_frame f=*completed;
    lck_mtx_lock(&camera_lock);int e=0;const struct x9_camera_sensor *s=sensor();
    if(!(st.flags&X9_CAMERA_STREAMING) || f.generation!=st.generation || f.sensor!=st.sensor)e=ESTALE;
    else if(cam_completed_layout(s,&f) ||
            !f.capture_id || !f.time || f.time>mach_absolute_time())e=EINVAL;
    else if(f.capture_id<=last_capture_id || f.time<=last_capture_time)e=ESTALE;
    else e=cam_completed_controls(s,&f);
    if(!e) {
        last_capture_id=f.capture_id;last_capture_time=f.time;
        if(frame_readers)e=EBUSY; /* retire, never overwrite/replay a pinned CPU copy */
    }
    if(!e) {
        memset(&head,0,sizeof(head));memcpy(head.magic,HAKIOS_CAMERA_MAGIC,8);
        head.header_bytes=sizeof(head);head.width=s->width;head.height=s->height;
        head.stride=f.stride;head.format=1;head.bayer=s->bayer;head.black=64;
        head.sequence=++st.frames;head.time=f.time;head.exposure_lines=f.exposure_lines;
        /* HakiOS ISP expects linear gain. Sony registers encode reciprocal
         * gain: unity/(unity-reg), NOT reg/unity. ABI units are linear Q10. */
        head.line_ns=s->line_ns;head.gain=1024*s->gain_unity/(s->gain_unity-f.gain_code);
        head.gain_unity=1024;head.gain_max=65536;
        head.frame_lines=f.frame_lines;head.frame_lines_min=s->frame_lines;head.frame_lines_max=s->frame_max;
        head.margin=48;head.sensor=f.sensor;head.focus=HAKIOS_CAMERA_NO_FOCUS;
        memcpy(frame,f.raw,f.bytes);frame_bytes=f.bytes;wakeup(&frame_bytes);
    } else {st.rejected++;st.last_errno=e;}
    lck_mtx_unlock(&camera_lock);return e;
}
#include "x9_camera_camsv.inc"
static int cam_open(__unused dev_t d,int f,__unused int t,struct proc *p)
{
    if(f&FWRITE) {
        char name[32];proc_name(proc_pid(p),name,sizeof(name));
        uid_t uid=kauth_cred_getuid(kauth_cred_get());
        if(st.mode==3) { if(uid)return EPERM; }
        else if(st.mode!=2 || (uid && (uid!=501 || strcmp(name,"mediaserverd"))))return EPERM;
    }
    lck_mtx_lock(&camera_control_lock);lck_mtx_lock(&camera_lock);
    int e=opened?EBUSY:0;if(!e)opened=true;
    lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);return e;
}
static int cam_close(__unused dev_t d,__unused int f,__unused int t,__unused struct proc *p)
{
    lck_mtx_lock(&camera_control_lock);lck_mtx_lock(&camera_lock);int e=stop();opened=false;
    if(frame && !session.latched){kfree_data(frame,X9_CAMERA_MAX_BYTES);frame=NULL;}
    lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);return e;
}
static int cam_ioctl(__unused dev_t d,u_long cmd,caddr_t data,int f,__unused struct proc *p)
{
    if(cmd==X9_CAMERA_GET_STATUS) {
        lck_mtx_lock(&camera_lock);memcpy(data,&st,sizeof(st));lck_mtx_unlock(&camera_lock);return 0;
    }
    if(cmd==X9_CAMERA_GET_PROBE) {
        lck_mtx_lock(&camera_control_lock);memcpy(data,&cam_probe_last,sizeof(cam_probe_last));
        lck_mtx_unlock(&camera_control_lock);return 0;
    }
    if(cmd==X9_CAMERA_GET_CONFIG) {
        lck_mtx_lock(&camera_control_lock);memcpy(data,&cam_config_last,sizeof(cam_config_last));
        lck_mtx_unlock(&camera_control_lock);return 0;
    }
    if(cmd==X9_CAMERA_CONFIG_STANDBY) {
        if(st.mode!=3 || !(f&FWRITE) || kauth_cred_getuid(kauth_cred_get()))return EPERM;
        struct x9_camera_config r,request={.version=1};
        memcpy(&r,data,sizeof(r));request.sensor=r.sensor;
        if((r.sensor!=1 && r.sensor!=2) || memcmp(&r,&request,sizeof(r)))return EINVAL;
        lck_mtx_lock(&camera_control_lock);(void)cam_standby(request.sensor,&r);
        lck_mtx_lock(&camera_lock);st.last_errno=r.last_errno;
        if(r.flags&X9_CAM_CFG_ATTEMPTED)st.probe_flags=r.probe.flags;
        if(!r.last_errno && (r.probe.flags&X9_CAM_PROBE_MATCHED)) {
            st.chip_id[r.sensor-1]=r.probe.chip_id;st.probe_mask|=1u<<r.sensor;
            st.flags|=X9_CAMERA_PROBED;
        }
        if(r.flags&X9_CAM_CFG_LATCHED)st.flags|=X9_CAMERA_FAULT;
        if(r.restore_errno)st.flags|=X9_CAMERA_RETAINED;
        if(r.flags&X9_CAM_CFG_ATTEMPTED) {
            st.cold_retained=cam_cold_retained(&cam_journal);
            st.cold_restore_step=r.restore_errno?cam_journal.restore_step:0;
        }
        /* Verified sensor standby does not confer receiver/translation ownership. */
        lck_mtx_unlock(&camera_lock);memcpy(data,&r,sizeof(r));
        lck_mtx_unlock(&camera_control_lock);return 0;
    }
    if(cmd==X9_CAMERA_GET_PIPELINE) {
        struct x9_camera_pipeline r;
        memcpy(&r,data,sizeof(r));
        struct x9_camera_pipeline request={.version=1,.sensor=r.sensor};
        if(memcmp(&r,&request,sizeof(request)))return EINVAL;
        lck_mtx_lock(&camera_lock);
        int e=cam_pipeline_plan(request.sensor,&r);
        lck_mtx_unlock(&camera_lock);
        if(!e)memcpy(data,&r,sizeof(r));return e;
    }
    if(cmd==X9_CAMERA_GET_RESOURCES) {
        lck_mtx_lock(&camera_control_lock);
        memcpy(data,&cam_resources_last,sizeof(cam_resources_last));
        lck_mtx_unlock(&camera_control_lock);return 0;
    }
    if(cmd==X9_CAMERA_PROBE_RESOURCES) {
        if(st.mode!=3 || !(f&FWRITE) || kauth_cred_getuid(kauth_cred_get()))return EPERM;
        struct x9_camera_resources r,request={.version=1};
        memcpy(&r,data,sizeof(r));if(memcmp(&r,&request,sizeof(r)))return EINVAL;
        lck_mtx_lock(&camera_control_lock);
        (void)cam_resources_probe(&r);
        lck_mtx_lock(&camera_lock);
        st.last_errno=r.last_errno;
        if(r.flags&X9_CAM_RES_LATCHED)st.flags|=X9_CAMERA_FAULT;
        /* Resource power isn't sensor identity or physical DMA readiness. */
        lck_mtx_unlock(&camera_lock);
        memcpy(data,&r,sizeof(r));lck_mtx_unlock(&camera_control_lock);return 0;
    }
    if(cmd==X9_CAMERA_GET_MM_CLOCKS) {
        lck_mtx_lock(&camera_control_lock);
        memcpy(data,&cam_mm_last,sizeof(cam_mm_last));
        lck_mtx_unlock(&camera_control_lock);return 0;
    }
    if(cmd==X9_CAMERA_PROBE_MM_CLOCKS) {
        if(st.mode!=3 || !(f&FWRITE) || kauth_cred_getuid(kauth_cred_get()))return EPERM;
        struct x9_camera_mm_status r,request={.version=1};
        memcpy(&r,data,sizeof(r));request.sensor=r.sensor;
        if((r.sensor!=1 && r.sensor!=2) || memcmp(&r,&request,sizeof(r)))return EINVAL;
        lck_mtx_lock(&camera_control_lock);
        (void)cam_mm_probe(request.sensor,&r);
        lck_mtx_lock(&camera_lock);st.last_errno=r.last_errno;
        if(r.flags&X9_CAM_MM_LATCHED)st.flags|=X9_CAMERA_FAULT;
        /* No identity, stream-on, translation or frame readiness change. */
        lck_mtx_unlock(&camera_lock);
        memcpy(data,&r,sizeof(r));lck_mtx_unlock(&camera_control_lock);return 0;
    }
    if(cmd==X9_CAMERA_GET_RECEIVER) {
        lck_mtx_lock(&camera_control_lock);memcpy(data,&cam_rx_last,sizeof(cam_rx_last));
        lck_mtx_unlock(&camera_control_lock);return 0;
    }
    if(cmd==X9_CAMERA_PROBE_RECEIVER) {
        if(st.mode!=3 || !(f&FWRITE) || kauth_cred_getuid(kauth_cred_get()))return EPERM;
        struct x9_camera_receiver r,request={.version=1};
        memcpy(&r,data,sizeof(r));request.sensor=r.sensor;
        if((r.sensor!=1 && r.sensor!=2) || memcmp(&r,&request,sizeof(r)))return EINVAL;
        lck_mtx_lock(&camera_control_lock);(void)cam_rx_probe(request.sensor,&r);
        lck_mtx_lock(&camera_lock);st.last_errno=r.last_errno;
        if(r.flags&X9_CAM_RX_LATCHED)st.flags|=X9_CAMERA_FAULT;
        /* Neither disabled receiver controls nor MM clocks claim resources. */
        lck_mtx_unlock(&camera_lock);
        memcpy(data,&r,sizeof(r));lck_mtx_unlock(&camera_control_lock);return 0;
    }
    if(cmd!=X9_CAMERA_PROBE_SENSOR)return ENOTTY;
    if(st.mode!=3 || !(f&FWRITE) || kauth_cred_getuid(kauth_cred_get()))return EPERM;
    struct x9_camera_probe *r=(struct x9_camera_probe *)data;
    struct x9_camera_probe request={.version=1,.sensor=r->sensor};
    if((r->sensor!=1 && r->sensor!=2) || memcmp(r,&request,sizeof(request)))return EINVAL;
    lck_mtx_lock(&camera_control_lock);
    if(cam_resources_latched) {
        memset(r,0,sizeof(*r));r->version=1;r->sensor=request.sensor;
        r->last_errno=EIO;r->flags=X9_CAM_PROBE_LATCHED;
    } else (void)cam_sensor_probe(request.sensor,r);
    lck_mtx_lock(&camera_lock);
    st.probe_flags=r->flags;st.last_errno=r->last_errno;
    if((r->flags&(X9_CAM_PROBE_MATCHED|X9_CAM_PROBE_RESTORED))==
        (X9_CAM_PROBE_MATCHED|X9_CAM_PROBE_RESTORED) && !r->last_errno) {
        st.chip_id[r->sensor-1]=r->chip_id;st.probe_mask|=1u<<r->sensor;
        st.flags|=X9_CAMERA_PROBED;
    }
    if(r->flags&X9_CAM_PROBE_LATCHED)st.flags|=X9_CAMERA_FAULT;
    if(r->restore_errno)st.flags|=X9_CAMERA_RETAINED;
    if(r->flags&X9_CAM_PROBE_ATTEMPTED) {
        st.cold_retained=cam_cold_retained(&cam_journal);
        st.cold_restore_step=r->restore_errno?cam_journal.restore_step:0;
    }
    /* supported_mask and READY stay zero: a probe cannot capture. */
    lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);
    return 0; /* operation/restore errors are in the copied-out result */
}
static int cam_write(__unused dev_t d,struct uio *uio,__unused int flags)
{
    if(st.mode!=2)return EPERM; /* mode3 has only fixed standby table ioctl, no raw writer */
    struct hakios_camera_command c;if(uio_resid(uio)!=sizeof(c))return EINVAL;
    int e=uiomove((char*)&c,sizeof(c),uio);if(e)return e;if(c.bytes)return ENOTSUP;
    lck_mtx_lock(&camera_control_lock);lck_mtx_lock(&camera_lock);
    if(c.command==HAKIOS_CAMERA_SENSOR)e=start(c.a,c.b);
    else if(c.command==HAKIOS_CAMERA_EXPOSURE && (st.flags&X9_CAMERA_STREAMING)) {
        const struct x9_camera_sensor *s=sensor();uint32_t lines=c.c?c.c:s->frame_lines;
        if(!c.c && (uint64_t)c.a+48>lines) {
            if(c.a>s->frame_max-48){
                lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);return EINVAL;
            }
            lines=c.a+48;
        }
        if(lines>s->frame_max || lines<s->frame_lines || c.a<s->exposure_min ||
           c.a>lines-48 || c.a%s->exposure_step || lines%s->exposure_step || c.b<1024 || c.b>65536)e=EINVAL;
        else {
            e=write8(s,0x0104,1);
            if(!e)e=write16(s,0x0340,lines);
            if(!e)e=write16(s,0x0202,c.a);
            if(!e)e=write16(s,0x0204,s->gain_unity-(s->gain_unity*1024/c.b));
            int release=write8(s,0x0104,0);if(!e)e=release;
            if(e){st.flags|=X9_CAMERA_FAULT;st.last_errno=e;(void)stop();}
        }
    } else e=ENOTSUP; /* no fake focus, KEEP host export or arbitrary writes */
    lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);return e;
}
static int cam_read(__unused dev_t d,struct uio *uio,__unused int flags)
{
    if(uio_resid(uio)<sizeof(head))return EMSGSIZE;
    lck_mtx_lock(&camera_lock);int e=0;
    bool peek=uio_resid(uio)==sizeof(head),pinned=false;
    struct hakios_camera_frame snapshot;uint8_t *raw=NULL;uint32_t bytes=0;
    uint64_t generation=st.generation;
    if(!(st.flags&X9_CAMERA_STREAMING))e=ENXIO;
    else {
        uint64_t interval;nanoseconds_to_absolutetime(1000000000,&interval);
        uint64_t deadline=mach_absolute_time()+interval;
        while(!frame_bytes || (!peek && (consumed==st.frames || frame_readers))) {
            uint64_t now=mach_absolute_time(),ns;
            if(now>=deadline){e=EAGAIN;break;}
            absolutetime_to_nanoseconds(deadline-now,&ns);
            struct timespec timeout={(time_t)(ns/1000000000),(long)(ns%1000000000)};
            e=msleep(&frame_bytes,&camera_lock,PCATCH,"x9-camera",&timeout);
            if(e || generation!=st.generation || !(st.flags&X9_CAMERA_STREAMING))break;
        }
        if(e==EWOULDBLOCK)e=EAGAIN;
        if(!e && generation!=st.generation)e=ESTALE;
        if(!e && (!(st.flags&X9_CAMERA_STREAMING)||!frame_bytes))e=ENXIO;
        if(!e && !peek && uio_resid(uio)!=(user_ssize_t)(sizeof(head)+frame_bytes))e=EMSGSIZE;
        if(!e) {
            snapshot=head;bytes=frame_bytes;raw=frame;
            if(!peek){frame_readers++;pinned=true;}
        }
    }
    lck_mtx_unlock(&camera_lock);
    /* User page faults must not hold the completion/stop mutex. Header and
     * raw are from one pinned frame, not a live head after a newer delivery. */
    if(!e)e=uiomove((char*)&snapshot,sizeof(snapshot),uio);
    if(!e && pinned)e=uiomove((char*)raw,bytes,uio);
    if(pinned) {
        lck_mtx_lock(&camera_lock);
        if(!e && generation!=st.generation)e=ESTALE;
        if(!e)consumed=snapshot.sequence; /* not a later st.frames */
        frame_readers--;wakeup(&frame_readers);wakeup(&frame_bytes);
        lck_mtx_unlock(&camera_lock);
    }
    return e;
}
static const struct cdevsw cam_cdev={.d_open=cam_open,.d_close=cam_close,.d_read=cam_read,
    .d_write=cam_write,.d_ioctl=cam_ioctl,.d_stop=eno_stop,.d_reset=eno_reset,
    .d_select=eno_select,.d_mmap=eno_mmap,.d_strategy=eno_strat};
void sweet_x9_camera_init(void)
{
    if(!sweet_early_is_x9())return;
    int mode=sweet_x9_arg("x9_camera",0);if(mode<1 || mode>3)return;
    if(st.mode)return;st.mode=mode;
    int major=cdevsw_add(-1,&cam_cdev);if(major<0){st.last_errno=ENXIO;return;}
    /* Captured 20H392 launchd plist runs mediaserverd as mobile (501:501),
     * not macOS _mediaserverd (32). The status device is
     * NOT evidence of capture capability. No MMIO or rail changes at init. */
    if(!devfs_make_node(makedev(major,0),DEVFS_CHAR,mode==3?0:501,mode==3?0:501,0600,"x9-camera"))st.last_errno=ENOMEM;
    printf("X9 CAMERA: mode=%d hhmain(i2c4/1a) hhfront(i2c2/10); platform=%u, no fake frames\n",mode,hw.version);
    if(mode==3)printf("X9 CAMERA PROBE: root probe/standby ioctl only, no boot-time MMIO/power, no iOS install; shared providers required\n");
}
