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
static uint64_t consumed;
static struct hakios_camera_frame head;
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
#include "x9_camera_mt6993.inc"
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
static int table(const struct x9_camera_sensor *s, const uint16_t *p, uint32_t n)
{
    if(!n || n%2)return ENOTSUP;
    for(uint32_t i=0;i<n;i+=2) {
        /* Only pre-parsed init/preview pairs. Stream-on belongs to start().
         * Vendor init tables contain write-only commands: do not pretend
         * they are all readable. Stable control registers use write8(). */
        if(p[i]==0x0100 || p[i+1]>255)return EINVAL;
        int e=hw.write8(s,p[i],(uint8_t)p[i+1]); if(e)return e;
    }
    return 0;
}
static int stop(void)
{
    int e=0,a=0,b=0;
    bool active=st.flags&X9_CAMERA_STREAMING;
    const struct x9_camera_sensor *s=sensor();uint64_t generation=st.generation;
    st.generation++; st.flags&=~X9_CAMERA_STREAMING; frame_bytes=0; consumed=st.frames;
    wakeup(&frame_bytes);
    if(active) {
        lck_mtx_unlock(&camera_lock);
        e=write8(s,0x0100,0); a=hw.capture(s,0,generation);
        b=hw.power(s,0);
        lck_mtx_lock(&camera_lock);
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
    if(!frame)frame=kalloc_data(X9_CAMERA_MAX_BYTES,Z_WAITOK);
    if(!frame)return ENOMEM;
    e=hw.power(s,1);
    if(!e)e=read16(s,0x0016,&id);
    if(!e && id!=s->chip_id)e=ENODEV;
    if(!e)e=table(s,which==1?x9_hhmain_init:x9_hhfront_init,
                 which==1?X9_HHMAIN_INIT_COUNT:X9_HHFRONT_INIT_COUNT);
    if(!e)e=table(s,which==1?x9_hhmain_preview:x9_hhfront_preview,
                 which==1?X9_HHMAIN_PREVIEW_COUNT:X9_HHFRONT_PREVIEW_COUNT);
    /* hhmain mirror=3; hhfront normal=0. Bayer order must match DMA output,
     * mounting/display orientation is separate and not guessed here. */
    if(!e)e=write8(s,0x0101,which==1?3:0);
    if(!e)e=hw.capture(s,1,st.generation);
    if(!e)e=write8(s,0x0100,1);
    if(e) {
        uint64_t generation=st.generation++;
        st.flags|=X9_CAMERA_FAULT;st.last_errno=e;
        lck_mtx_unlock(&camera_lock);
        int off=hw.capture(s,0,generation),power=hw.power(s,0);
        lck_mtx_lock(&camera_lock);
        printf("X9 CAMERA: start sensor=%u errno=%d drain=%d restore=%d\n",which,e,off,power);
        return e;
    }
    st.flags|=X9_CAMERA_STREAMING;st.width=s->width;st.height=s->height;st.last_errno=0;
    printf("X9 CAMERA: start sensor=%u generation=%llu (waiting for physical DMA)\n",which,st.generation);
    return 0;
}
int x9_camera_register_platform(const struct x9_camera_platform *p)
{
    if(!p || p->version!=1 || !p->power || !p->read8 || !p->write8 || !p->capture)return EINVAL;
    lck_mtx_lock(&camera_control_lock);lck_mtx_lock(&camera_lock); int e=0;
    if(st.mode!=2)e=EPERM;
    else if(hw.version)e=EBUSY;
    else if(!X9_HHMAIN_INIT_COUNT || !X9_HHFRONT_INIT_COUNT)e=ENOTSUP;
    else {
        hw=*p;
        for(unsigned i=0;i<2 && !e;i++) {
            uint32_t id=0; e=hw.power(&sensors[i],1);
            if(!e)e=read16(&sensors[i],0x16,&id);
            int off=hw.power(&sensors[i],0);
            if(!e)e=off; if(!e && id!=sensors[i].chip_id)e=ENODEV;
            if(!e)st.chip_id[i]=id;
        }
        if(!e){st.supported_mask=(1u<<1)|(1u<<2);st.flags=X9_CAMERA_READY;}
        else {st.flags=X9_CAMERA_FAULT;st.last_errno=e;}
    }
    lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);return e;
}
int x9_camera_deliver(uint32_t which,uint64_t generation,const void *raw,
                     uint32_t stride,uint32_t bytes,uint64_t time)
{
    lck_mtx_lock(&camera_lock);int e=0;const struct x9_camera_sensor *s=sensor();
    if(!(st.flags&X9_CAMERA_STREAMING) || generation!=st.generation || which!=st.sensor)e=ESTALE;
    else if(!raw || stride<s->width/4*5 || stride>s->width/4*5+4096 ||
            (uint64_t)stride*s->height!=bytes || bytes>X9_CAMERA_MAX_BYTES ||
            !time || time<=head.time || time>mach_absolute_time())e=EINVAL;
    uint32_t exposure=0,gain=0,lines=0;
    if(!e)e=read16(s,0x0202,&exposure);
    if(!e)e=read16(s,0x0204,&gain);
    if(!e)e=read16(s,0x0340,&lines);
    if(!e && (lines<s->frame_lines || lines>s->frame_max || exposure<s->exposure_min ||
              exposure>lines-48 || gain>s->gain_max))e=EIO;
    if(!e) {
        memset(&head,0,sizeof(head));memcpy(head.magic,HAKIOS_CAMERA_MAGIC,8);
        head.header_bytes=sizeof(head);head.width=s->width;head.height=s->height;
        head.stride=stride;head.format=1;head.bayer=s->bayer;head.black=64;
        head.sequence=++st.frames;head.time=time;head.exposure_lines=exposure;
        /* HakiOS ISP expects linear gain. Sony registers encode reciprocal
         * gain: unity/(unity-reg), NOT reg/unity. ABI units are linear Q10. */
        head.line_ns=s->line_ns;head.gain=1024*s->gain_unity/(s->gain_unity-gain);
        head.gain_unity=1024;head.gain_max=65536;
        head.frame_lines=lines;head.frame_lines_min=s->frame_lines;head.frame_lines_max=s->frame_max;
        head.margin=48;head.sensor=which;head.focus=HAKIOS_CAMERA_NO_FOCUS;
        memcpy(frame,raw,bytes);frame_bytes=bytes;wakeup(&frame_bytes);
    } else {st.rejected++;st.last_errno=e;}
    lck_mtx_unlock(&camera_lock);return e;
}
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
    if(frame){kfree_data(frame,X9_CAMERA_MAX_BYTES);frame=NULL;}
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
    if(cmd!=X9_CAMERA_PROBE_SENSOR)return ENOTTY;
    if(st.mode!=3 || !(f&FWRITE) || kauth_cred_getuid(kauth_cred_get()))return EPERM;
    struct x9_camera_probe *r=(struct x9_camera_probe *)data;
    struct x9_camera_probe request={.version=1,.sensor=r->sensor};
    if((r->sensor!=1 && r->sensor!=2) || memcmp(r,&request,sizeof(request)))return EINVAL;
    lck_mtx_lock(&camera_control_lock);
    (void)cam_sensor_probe(request.sensor,r);
    lck_mtx_lock(&camera_lock);
    st.probe_flags=r->flags;st.last_errno=r->last_errno;
    if((r->flags&(X9_CAM_PROBE_MATCHED|X9_CAM_PROBE_RESTORED))==
        (X9_CAM_PROBE_MATCHED|X9_CAM_PROBE_RESTORED) && !r->last_errno) {
        st.chip_id[r->sensor-1]=r->chip_id;st.probe_mask|=1u<<r->sensor;
        st.flags|=X9_CAMERA_PROBED;
    }
    if(r->flags&X9_CAM_PROBE_LATCHED)st.flags|=X9_CAMERA_FAULT;
    /* supported_mask and READY stay zero: a probe cannot capture. */
    lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);
    return 0; /* operation/restore errors are in the copied-out result */
}
static int cam_write(__unused dev_t d,struct uio *uio,__unused int flags)
{
    if(st.mode!=2)return EPERM; /* probe mode has NO sensor register writer */
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
    if(!(st.flags&X9_CAMERA_STREAMING))e=ENXIO;
    else {
        uint64_t interval;nanoseconds_to_absolutetime(1000000000,&interval);
        uint64_t deadline=mach_absolute_time()+interval,generation=st.generation;
        while(!frame_bytes || (uio_resid(uio)!=sizeof(head) && consumed==st.frames)) {
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
        bool peek=uio_resid(uio)==sizeof(head);
        if(!e && !peek && uio_resid(uio)!=(user_ssize_t)(sizeof(head)+frame_bytes))e=EMSGSIZE;
        if(!e)e=uiomove((char*)&head,sizeof(head),uio);
        if(!e && !peek){e=uiomove((char*)frame,frame_bytes,uio);if(!e)consumed=st.frames;}
    }
    lck_mtx_unlock(&camera_lock);return e;
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
    if(mode==3)printf("X9 CAMERA PROBE: root ioctl only, no boot-time MMIO/power, no iOS install; inherited clock/rails required\n");
}
