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
#include <kern/thread.h>
#include "x9_camera.h"
/* Failure trace (diagnostic only, no behaviour change): every literal errno
 * returned/assigned in the camera stack is recorded with function+line, so
 * one boot names the exact rejecting check. Lock-free ring; a racing IRQ
 * update can only garble a diagnostic entry. Dumped on start failure. */
#define CAM_FAIL_SLOTS 48u
static struct { const char *fn; unsigned line; int e; unsigned hits; uint64_t seq; } cam_fail_ring[CAM_FAIL_SLOTS];
static uint64_t cam_fail_seq;
static int cam_fail_note(int e,const char *fn,unsigned line)
{
    unsigned victim=0;
    uint64_t seq=++cam_fail_seq;
    for(unsigned i=0;i<CAM_FAIL_SLOTS;i++) {
        if(cam_fail_ring[i].fn==fn && cam_fail_ring[i].line==line) {
            cam_fail_ring[i].e=e;cam_fail_ring[i].hits++;cam_fail_ring[i].seq=seq;return e;
        }
        if(cam_fail_ring[i].seq<cam_fail_ring[victim].seq)victim=i;
    }
    cam_fail_ring[victim].fn=fn;cam_fail_ring[victim].line=line;
    cam_fail_ring[victim].e=e;cam_fail_ring[victim].hits=1;cam_fail_ring[victim].seq=seq;
    return e;
}
#define CAM_FAIL(e) cam_fail_note((e),__func__,__LINE__)
static void cam_fail_reset(void)
{
    memset(cam_fail_ring,0,sizeof(cam_fail_ring));cam_fail_seq=0;
}
static void cam_fail_dump(const char *tag)
{
    uint64_t below=UINT64_MAX;
    for(unsigned k=0;k<16;k++) {
        int best=-1;
        for(unsigned i=0;i<CAM_FAIL_SLOTS;i++)
            if(cam_fail_ring[i].seq && cam_fail_ring[i].seq<below &&
               (best<0 || cam_fail_ring[i].seq>cam_fail_ring[best].seq))best=(int)i;
        if(best<0)break;
        below=cam_fail_ring[best].seq;
        printf("X9 CAMERA FAIL TRACE %s #%u: %s:%u errno=%d hits=%u seq=%llu\n",tag,k,
            cam_fail_ring[best].fn,cam_fail_ring[best].line,cam_fail_ring[best].e,
            cam_fail_ring[best].hits,cam_fail_ring[best].seq);
    }
}
#include "../../../../camera/hakios.camera.h"
#include "../../../generated/x9_camera_sensor_tables.inc"

extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *, int);
extern kern_return_t thread_terminate(thread_t);
LCK_GRP_DECLARE(camera_group, "x9-camera");
LCK_MTX_DECLARE(camera_lock, &camera_group);
/* Controls serialize separately so capture(off) may drain a completion worker
 * without holding the mutex that worker needs to reject its stale frame. */
LCK_MTX_DECLARE(camera_control_lock, &camera_group);
/* Shared by the inactive request publisher and completion adapter. */
LCK_MTX_DECLARE(camera_camsv_lock, &camera_group);
static struct x9_camera_status st = { .version = 1, .sensor = X9_CAMERA_BACK };
static struct x9_camera_platform hw;
static bool opened;
static uint32_t cam_chip_ids[X9_CAMERA_SENSOR_COUNT];
static uint32_t cam_native_mask=X9_CAMERA_DUAL_MASK;
static bool control_stop_issued; /* sealed until a genuinely new cdev open */
static struct x9_camera_stop cam_stop_last; /* historical, not a live lease */
static uint8_t *frame;
static uint32_t frame_bytes;
/* Pin CPU staging across faultable uiomove, as HakiOS pins PLACE_READING.
 * This is NOT a DMA mapping/ownership counter. A busy staging buffer drops
 * the next delivery rather than blocking a hardware completion on user VM. */
static unsigned frame_readers;
/* diag46: per-frame cache-maintenance timing (x9_camera_s1_dma.inc), printed by x9_camera_deliver. */
static uint64_t cam_perf_cpu_ns,cam_perf_dev_ns;
static uint64_t consumed;
static struct hakios_camera_frame head;
/* Source completion order is independent of accepted userland frame count.
 * Retire even a backpressured frame so it cannot be replayed after a read. */
static uint64_t last_capture_id, last_capture_time, last_sof_time;
_Static_assert(sizeof(struct x9_camera_completed_frame)==72,"kernel completion record");
_Static_assert(sizeof(struct hakios_camera_frame)==120,"RAW10 v2 wire layout");
_Static_assert(__builtin_offsetof(struct hakios_camera_frame,sof_time_lo)==100,"RAW10 timing offset");
/* Journal attempted callbacks BEFORE invoking them: an error may follow a
 * partial state change. Kept after failed drain/restore; no retry on close. */
static struct {
    bool power, capture, stream, latched, stopping;
    bool first_raw_timeout_logged;
    uint64_t generation;
    int last_errno;
} session;
/* camera_lock held. Only the actual core STOP's invalidated generation may
 * finish a privately qualified owned DONE without delivering it. This is a
 * cancellation journal, NEVER native authority, a DMA receipt or a lease.
 * Faulted/partial startup, another sensor and arbitrary old frames stay out. */
static bool cam_core_retiring_locked(uint64_t generation,uint32_t which)
{
    return session.stopping && !session.latched && session.power && session.capture && session.stream &&
        !(st.flags&(X9_CAMERA_STREAMING|X9_CAMERA_FAULT)) && st.sensor==which &&
        generation==session.generation && generation!=UINT64_MAX && st.generation==generation+1;
}
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
static const struct x9_camera_power_step tele_power[] = {
    {X9_CAM_MCLK,24,0}, {X9_CAM_RESET,0,1000},
    {X9_CAM_AVDD,2804000,3000}, {X9_CAM_DVDD,1104000,4000},
    {X9_CAM_DOVDD,1800000,3000}, {X9_CAM_AFVDD,3100000,1000},
    {X9_CAM_DRIVE,4,6000}, {X9_CAM_RESET,1,2000}
};
static const struct x9_camera_power_step uw_power[] = {
    {X9_CAM_MCLK,24,2000},{X9_CAM_RESET,0,1000},{X9_CAM_DOVDD,1800000,1000},
    {X9_CAM_AVDD,2204000,1000},{X9_CAM_DVDD,1008000,1000},
    {X9_CAM_DVDD1,1000000,0},{X9_CAM_AFVDD,2800000,0},
    {X9_CAM_RESET,1,2000},{X9_CAM_DRIVE,4,10000}
};
static const struct x9_camera_sensor sensors[] = {
    /* main physical ID 0x9186: vendor maps it to HHMAIN_SENSOR_ID, which is
     * a software ID, NOT the value of the chip ID registers 0x16/0x17. */
    {1,4,0x1a,0x9186,4096,3072,3,4415,7546,65532,2,6,16384,16128,1,3,
     main_power, sizeof(main_power)/sizeof(main_power[0]),
     0x16,3,3,1,3,9696,48,0,0,2196000000ull,3300004166ull},
    {2,2,0x10,0x0615,3264,2448,0,4361,7642,65530,1,16,1024,1008,0,4,
     front_power, sizeof(front_power)/sizeof(front_power[0]),
     0x16,0,0,0,0,3768,48,0,0,864000000ull,2160000000ull},
    /* Banked word tables, byte controls, one cold-owned SID5 DVDD alias.
     * Common wire protocol caps requested gain at64x, not vendor's160x. */
    {3,9,0x2d,0x38e5,4096,3072,1,5200,6408,65535,1,4,32,2048,0,4,
     uw_power,sizeof(uw_power)/sizeof(uw_power[0]),0,0,2,1,3,4784,24,1,1,920000000ull,2848000000ull},
    {4,7,0x1a,0x8202,4096,3072,1,8538,3888,65532,4,8,16384,16128,1,3,
     tele_power, sizeof(tele_power)/sizeof(tele_power[0]),
     0x16,2,1,0,0,7500,56,0,0,878400000ull,1624000000ull}
};
static const struct x9_camera_sensor *cam_sensor_profile(uint32_t which)
{ return x9_camera_sensor_valid(which)?&sensors[which-1]:NULL; }
static uint8_t cam_sensor_slave(uint32_t which)
{ const struct x9_camera_sensor *s=cam_sensor_profile(which);return s?(uint8_t)s->slave:0; }
static uint32_t cam_sensor_port(uint32_t which)
{ const struct x9_camera_sensor *s=cam_sensor_profile(which);return s?s->csi_port:UINT32_MAX; }
static void cam_sensor_id_record(uint32_t which,uint32_t id)
{ cam_chip_ids[which-1]=id;if(which<=X9_CAMERA_FRONT)st.chip_id[which-1]=id; }
static const struct x9_camera_sensor *sensor(void) { return &sensors[st.sensor-1]; }
/* All cold/MM teardown paths consult the actual native controller's export
 * journal even when configure failed before core attempted capture(on). */
static int cam_native_sensor_retirement_guard_locked(uint64_t epoch);
static void cam_native_capture_stop_locked(uint64_t epoch);
/* Log 382: start() hung after NATIVE CONFIG configured with no further
 * camera output while the boot stalled at the logo. Every capture-arm
 * step prints PROGRESS, and a separate watchdog thread prints STALL with
 * the last step from another CPU if one step lasts longer than 2 s. */
static const char *volatile cam_progress_step;
static volatile uint64_t cam_progress_t0;
static bool cam_watchdog_started;
/* Log 384: hang inside cam_native_match's SMMU register read. The current
 * MMIO region/offset is published before each read so STALL can name it. */
static volatile unsigned cam_mmio_region,cam_mmio_off,cam_mmio_reads;
/* A long or stuck sensor start must not park mediaserverd's other opens /
 * writes on camera_control_lock during boot: they get EBUSY at once. */
#include <pexpert/pexpert.h>
static volatile bool cam_start_busy;
extern void x9_mm_power_smmu_dump(const char *tag);
extern void x9_mm_power_stall_dump(void);
/* Logs 383-386: a sensor start issued by mediaserverd at ~11s ends in an
 * MM SMMU read that never returns, and boot stays on the logo. Refuse
 * starts until uptime >= x9_cam_defer seconds (default 0 = off) so boot
 * completes first; a later Camera open starts normally. */
/* mach_absolute_time() keeps counting from before this kernel (log 404 read
 * 70.6 s at 11.2 s uptime, and warm resets do not clear it), so measure from
 * camera platform registration, which happens ~3 s into boot. */
static uint64_t cam_boot_t0;
#define CAM_BOOT_T0_UPTIME_NS (3ull*NSEC_PER_SEC)
static bool cam_boot_deferred(void)
{
    static int defer_s=-1;
    if(defer_s<0){int v=0;if(!PE_parse_boot_argn("x9_cam_defer",&v,sizeof(v)))v=0;defer_s=v<0?0:v;}
    if(!defer_s)return false;
    if(!cam_boot_t0)return true; /* not registered yet: certainly early boot */
    uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time()-cam_boot_t0,&ns);ns+=CAM_BOOT_T0_UPTIME_NS;
    if(ns>=(uint64_t)defer_s*NSEC_PER_SEC)return false;
    static unsigned said;
    if(said++<4)printf("X9 CAMERA: start deferred until %ds uptime (now %llums, x9_cam_defer)\n",defer_s,ns/1000000ull);
    return true;
}
/* The 20 ms post-mark drain in cam_progress() below was a diag34-era USB
 * console flush aid. Log 421-diag46 (2026-11-26) shows ~45 prepare:/smmu:/ft:
 * marks per cold start, i.e. ~0.9 s of the black-screen time is this delay,
 * not hardware work. It is now opt-in: x9_cam_drain=1 restores the flush
 * behaviour for hang diagnosis; the default path pays nothing. The stop
 * path keeps its own unconditional drain (cam_drain_mark): close-path
 * resets are still an active diagnosis target. */
static bool cam_drain_enabled(void)
{
    static int on=-1;
    if(on<0){int v=0;if(!PE_parse_boot_argn("x9_cam_drain",&v,sizeof(v)))v=0;on=v?1:0;}
    return on!=0;
}
static void cam_slow_summary(const char *step);
extern void x9_crumb_cam_step(const char *step);
static void cam_progress(const char *step)
{
    x9_crumb_cam_step(step);
    if(!strncmp(step,"configure:",10) || !strncmp(step,"start:",6) || !strncmp(step,"prepare:smmu-start",18))cam_slow_summary(step);
    cam_progress_t0=mach_absolute_time();cam_progress_step=step;
    /* diag44: the per-check storage marks (st:, s1:) ran ~160x/s while
     * streaming (4 console lines per lease check); keep them as crumbs and
     * stall-watchdog steps only. */
    if(strncmp(step,"st:",3) && strncmp(step,"s1:",3))printf("X9 CAMERA PROGRESS: %s\n",step);
    /* diag34: the first-open freeze (diag32/32b/33) lands in the ~1 KiB the
     * USB console loses after prepare:controller. Flush 20 ms after each
     * prepare/SMMU bring-up mark, as cam_drain_mark does on the stop path,
     * so the last visible mark is the step that hung. Logging only -- and,
     * since log 421-diag46 priced it at ~0.9 s per cold start, only with
     * x9_cam_drain=1 (see cam_drain_enabled). */
    if(cam_drain_enabled() &&
       (!strncmp(step,"prepare:",8) || !strncmp(step,"smmu:",5) || !strncmp(step,"ft:",3)))delay(20000);
}
/* Stop-path step mark: the USB console loses ~1 KiB at a SoC reset, so wait
 * 20 ms after each mark for the host to drain it; the last visible mark is
 * then the step that hung (diag8/diag9 resets in close). */
static void cam_drain_mark(const char *step)
{
    x9_crumb_cam_step(step);
    printf("X9 CAMERA STOP MARK: %s\n",step);
    delay(20000);
}
static void __attribute__((noreturn)) cam_watchdog(void *arg,wait_result_t result)
{
    const char *last=NULL;unsigned reports=0;
    (void)arg;(void)result;
    for(;;) {
        assert_wait_timeout((event_t)&cam_watchdog_started,THREAD_UNINT,2000,NSEC_PER_MSEC);
        thread_block(THREAD_CONTINUE_NULL);
        const char *step=cam_progress_step;uint64_t ns;
        if(!step){last=NULL;continue;}
        absolutetime_to_nanoseconds(mach_absolute_time()-cam_progress_t0,&ns);
        if(step!=last){last=step;reports=0;}
        if(ns>=2000000000ull && reports<30) {
            reports++;
            if(reports<=3)x9_mm_power_stall_dump();
            printf("X9 CAMERA STALL: step=%s age_ms=%llu (#%u) mmio=%s+%x reads=%u\n",step,ns/1000000ull,reports,
                cam_mmio_region==1?"smmu30800000":cam_mmio_region==2?"wp309e0000":cam_mmio_region==3?"smmu64":"-",
                cam_mmio_off,cam_mmio_reads);
        }
    }
}
static void cam_watchdog_start(void)
{
    thread_t t;
    if(cam_watchdog_started)return;
    if(kernel_thread_start(cam_watchdog,NULL,&t)==KERN_SUCCESS){thread_deallocate(t);cam_watchdog_started=true;}
}
/* Budget for acceptance loops that re-run guards each poll. The old 1 s
 * assumed 3-90 ms guards (log 376); 390-393 measured 0.1-0.65 s per sweep,
 * so every capture-arm stage expired after 2-3 polls with HW already at
 * target. Hardware-paced waits (SMMU CMDQ, I2C) keep their own limits. */
#define CAM_GUARD_BUDGET_NS 10000000000ull
#include "x9_camera_pipeline.inc"
#include "x9_camera_s1.inc"
#include "x9_camera_smmu.inc"
#include "x9_camera_sensor.inc"
#include "x9_camera_mt6993.inc"
#include "x9_camera_resources.inc"
#include "x9_camera_mm.inc"
#include "x9_camera_sensor_session.inc"
#include "x9_camera_standby.inc"
#include "x9_camera_receiver.inc"
#include "x9_camera_native_source.inc"
_Static_assert(sizeof(struct x9_camera_status)==88,"camera status ABI");
_Static_assert(sizeof(struct x9_camera_stop)==64,"camera stop ABI");
static int read16(const struct x9_camera_sensor *s, uint16_t reg, uint32_t *v)
{
    uint8_t hi,lo; int e=hw.read8(s,reg,&hi);
    if(!e)e=hw.read8(s,reg+1,&lo); if(!e)*v=((uint32_t)hi<<8)|lo; return e;
}
/* Defined after the IRQ/FBC binding; control mutex is already held. */
static int cam_stream_set_locked(const struct x9_camera_sensor *,uint8_t);
static int write8(const struct x9_camera_sensor *s, uint16_t reg, uint8_t v)
{
    int e;
    uint8_t back;
    /* Stream commands belong to the bound native AP/request journal, not
     * an arbitrary platform write callback or the generated sensor tables. */
    if(reg==0x0100)return cam_stream_set_locked(s,v);
    e=hw.write8(s,reg,v);
    if(!e)e=hw.read8(s,reg,&back); return e?e:back==v?0:EIO;
}
static int cam_drain_readers(void)
{
    uint64_t span;nanoseconds_to_absolutetime(1000000000,&span);
    uint64_t deadline=mach_absolute_time()+span;
    while(frame_readers) {
        uint64_t now=mach_absolute_time(),ns;
        if(now>=deadline)return CAM_FAIL(ETIMEDOUT);
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
    if(session.latched)return session.last_errno?session.last_errno:EIO; /* no retry */
    if(st.generation==UINT64_MAX) {
        session.latched=true;session.last_errno=EOVERFLOW;
        st.flags|=X9_CAMERA_FAULT|X9_CAMERA_RETAINED;st.last_errno=EOVERFLOW;
        return CAM_FAIL(EOVERFLOW); /* cannot invalidate in-flight generation by wrapping */
    }
    session.stopping=true; /* before invalidating delivery, not a drained claim */
    st.generation++; st.flags&=~X9_CAMERA_STREAMING; frame_bytes=0; consumed=st.frames;
    /* Close next-request admission even if the following source-off fails
     * before core can invoke capture(off). This hook never claims drain. */
    cam_native_capture_stop_locked(cam_sensor_live.epoch);
    thread_wakeup((event_t)&st.generation); /* queued first-frame IRQ worker */
    wakeup(&frame_bytes);
    if(session.power || session.capture || session.stream) {
        lck_mtx_unlock(&camera_lock);
        cam_drain_mark("stop:stream-off");
        if(session.stream)e=write8(s,0x0100,0);
        /* An uncertain source-off is not permission for a later callback
         * to report capture retired or remove power. Retain the same
         * attempted capture/source even if no SOF has reached the worker. */
        if(e)a=e;
        else if(session.capture){cam_drain_mark("stop:capture-off");a=hw.capture(s,0,session.generation);}
        /* Publish confirmed capture retirement before power(false). Native
         * sensor unwind must still see capture=true after failed/partial
         * drain; it cannot rely on a caller-supplied "drained" boolean. */
        lck_mtx_lock(&camera_lock);
        if(!a)session.capture=false;
        lck_mtx_unlock(&camera_lock);
        /* Never remove sensor/MM/translation dependencies below a DMA
         * engine whose stop/drain failed. Provider retains its own journal. */
        if(!a && session.power){cam_drain_mark("stop:power-off");b=hw.power(s,0);cam_drain_mark("stop:power-off-returned");}
        lck_mtx_lock(&camera_lock);
        if(!a && !b){session.power=false;session.stream=false;}
        if(a || b) {
            session.latched=true;session.last_errno=a?a:b;
            st.flags|=X9_CAMERA_RETAINED;
        }
        printf("X9 CAMERA DRAIN: sensor=%u generation=%llu stream=%d drain=%d restore=%d "
            "retained=%u/%u/%u latched=%u\n",s->sensor,session.generation,e,a,b,
            session.power,session.capture,session.stream,session.latched);
        /* diag28: name the exact CAM_FAIL site (func:line) behind a failed close. */
        if(a || b)cam_fail_dump("drain");
    }
    cam_drain_mark("stop:drain-readers");
    int copy=cam_drain_readers();
    if(copy) {
        session.latched=true;if(!session.last_errno)session.last_errno=copy;
        st.flags|=X9_CAMERA_RETAINED;
        printf("X9 CAMERA COPY DRAIN: readers=%u errno=%d (CPU staging retained, no retry)\n",
            frame_readers,copy);
        if(!e && !a && !b)e=copy;
    }
    if(e||a||b){st.flags|=X9_CAMERA_FAULT;st.last_errno=e?e:a?a:b;}
    else session.stopping=false;
    return e?e:a?a:b;
}
/* Both control/frame mutexes held. No MMIO on cached receipt reads or close
 * after explicit STOP; a dup fd cannot restart/reissue within this open. */
static uint32_t cam_stop_owned(void)
{
    return (session.power?X9_CAM_STOP_POWER:0) |
        (session.capture?X9_CAM_STOP_CAPTURE:0) |
        (session.stream?X9_CAM_STOP_STREAM:0);
}
static void cam_stop_latch(int e)
{
    session.latched=true;if(!session.last_errno)session.last_errno=e;
    st.flags|=X9_CAMERA_FAULT|X9_CAMERA_RETAINED;st.last_errno=session.last_errno;
}
static void cam_stop_issue(void)
{
    int e;
    struct x9_camera_stop r={.version=1,.sensor=st.sensor,
        .generation_before=st.generation,.owned_before=cam_stop_owned(),
        .readers_before=frame_readers,.state_before=st.flags};
    control_stop_issued=true; /* BEFORE physical callbacks; never reissue */
    e=stop();
    if(!e && (cam_stop_owned() || frame_readers || session.latched ||
        (st.flags&(X9_CAMERA_STREAMING|X9_CAMERA_RETAINED))))e=CAM_FAIL(EIO);
    if(e)cam_stop_latch(e);
    r.generation_after=st.generation;r.owned_after=cam_stop_owned();
    r.readers_after=frame_readers;r.state_after=st.flags;r.last_errno=e;
    r.flags=e?X9_CAM_STOP_RETAINED:
        r.owned_before?X9_CAM_STOP_RETIRED:X9_CAM_STOP_IDLE;
    if(session.latched)r.flags|=X9_CAM_STOP_LATCHED;
    cam_stop_last=r;
}
static int cam_stop_cached_close(void)
{
    /* Preserve the original stop failure even if a retained CPU reader has
     * since returned. A successful receipt must still match actual scope. */
    if(cam_stop_last.last_errno)return cam_stop_last.last_errno;
    if(cam_stop_last.version!=1 || cam_stop_last.sensor!=st.sensor ||
        cam_stop_last.generation_after!=st.generation ||
        cam_stop_last.state_after!=st.flags || cam_stop_owned() ||
        frame_readers || session.latched) {
        cam_stop_latch(ESTALE);return CAM_FAIL(ESTALE);
    }
    return 0;
}
static int start(uint32_t which, uint32_t mode)
{
    const char *step="sensor-power";
    if(!x9_camera_sensor_valid(which))return CAM_FAIL(EINVAL);
    if(mode && mode!=UINT32_MAX)return CAM_FAIL(ENOTSUP);
    if(!(st.supported_mask&(1u<<which)))return CAM_FAIL(ENOTSUP); /* before retiring an active lens */
    if(!(st.flags&X9_CAMERA_READY) || (st.flags&X9_CAMERA_FAULT))return CAM_FAIL(ENXIO);
    if((st.flags&X9_CAMERA_STREAMING) && which==st.sensor)return 0;
    int e=stop(); if(e)return e;
    st.sensor=which; const struct x9_camera_sensor *s=sensor(); uint32_t id;
    struct x9_camera_config config={.version=1,.sensor=which};
    e=cam_sensor_config_preflight(which,&config);
    if(e){st.last_errno=e;return e;} /* reject missing/mismatched tables before power */
    if(!frame)frame=kalloc_data(X9_CAMERA_MAX_BYTES,Z_WAITOK);
    if(!frame)return CAM_FAIL(ENOMEM);
    session.generation=st.generation;
    session.first_raw_timeout_logged=false;
    last_capture_id=last_capture_time=last_sof_time=0;
    session.power=true;
    /* Worker completions can enter camera_deliver during capture setup.
     * Keep control serialization, but never hold the worker's frame lock
     * across power/I2C/capture callbacks. */
    cam_fail_reset();
    cam_start_busy=true;
    lck_mtx_unlock(&camera_lock);
    cam_watchdog_start();
    cam_progress("start:sensor-power");
    e=hw.power(s,1);
    if(!e) {
        step="sensor-id";
        e=read16(s,s->id_reg,&id);
        if(!e && id!=s->chip_id)e=CAM_FAIL(ENODEV);
    }
    if(!e) {
        step="configure";cam_progress("start:configure");
        e=hw.configure(s,&config);
    }
    if(!e) {
        step="capture-arm";cam_progress("start:capture-arm");
        session.capture=true;
        e=hw.capture(s,1,session.generation);
    }
    if(!e) {
        step="sensor-stream-on";cam_progress("start:stream-on");
        session.stream=true;
        e=write8(s,0x0100,1);
        printf("X9 CAMERA STREAM-ON RETURN: sensor=%u errno=%d\n",which,e);
    }
    cam_progress_step=NULL;
    lck_mtx_lock(&camera_lock);
    cam_start_busy=false;
    if(e) {
        st.flags|=X9_CAMERA_FAULT;st.last_errno=e;
        int cleanup=stop();st.last_errno=e;
        cam_slow_summary("start-end");
        printf("X9 CAMERA: start sensor=%u errno=%d cleanup=%d retained=%u step=%s\n",
            which,e,cleanup,session.latched,step);
        cam_fail_dump(step);
        return e;
    }
    st.flags|=X9_CAMERA_STREAMING;st.width=s->width;st.height=s->height;st.last_errno=0;
    thread_wakeup((event_t)&st.generation); /* actual stream-on ACK published */
    cam_slow_summary("start-end");
    printf("X9 CAMERA: start sensor=%u generation=%llu (waiting for physical DMA)\n",which,st.generation);
    return 0;
}
int x9_camera_register_platform(const struct x9_camera_platform *p)
{
    if(!cam_boot_t0)cam_boot_t0=mach_absolute_time();
    if(!p || p->version!=X9_CAMERA_PLATFORM_VERSION || !p->power || !p->read8 ||
       !p->write8 || !p->configure || !p->exposure || !p->capture)return CAM_FAIL(EINVAL);
    struct x9_camera_config config={0};
    int tables=0;
    for(uint32_t i=1;i<=X9_CAMERA_SENSOR_COUNT && !tables;i++)
        if(cam_native_mask&(1u<<i))tables=cam_sensor_config_preflight(i,&config);
    if(tables)return tables; /* including preview/PLL, BEFORE any power callback */
    lck_mtx_lock(&camera_control_lock);lck_mtx_lock(&camera_lock); int e=0;
    if(st.mode!=2)e=CAM_FAIL(EPERM);
    else if(opened || session.latched || cam_stop_owned() || frame_readers ||
        (st.flags&(X9_CAMERA_FAULT|X9_CAMERA_RETAINED|X9_CAMERA_STREAMING)))e=CAM_FAIL(EBUSY);
    else if(cam_sensor_session_active)e=CAM_FAIL(EBUSY);
    else if(hw.version)e=CAM_FAIL(EBUSY);
    else if(!X9_HHMAIN_INIT_COUNT || !X9_HHFRONT_INIT_COUNT)e=CAM_FAIL(ENOTSUP);
    else {
        hw=*p;int first=0;uint32_t probed=0;
        for(unsigned i=0;i<X9_CAMERA_SENSOR_COUNT && !e;i++) {
            if(!(cam_native_mask&(1u<<sensors[i].sensor)))continue;
            st.sensor=sensors[i].sensor;
            lck_mtx_unlock(&camera_lock);
            uint32_t id=0; e=hw.power(&sensors[i],1);
            if(!e)e=read16(&sensors[i],sensors[i].id_reg,&id);
            int off=hw.power(&sensors[i],0);
            lck_mtx_lock(&camera_lock);
            if(off) {
                session.power=true;session.latched=true;session.last_errno=off;
                st.flags|=X9_CAMERA_RETAINED;
            }
            if(!e)e=off; if(!e && id!=sensors[i].chip_id)e=CAM_FAIL(ENODEV);
            if(!e){cam_sensor_id_record(sensors[i].sensor,id);probed|=1u<<sensors[i].sensor;}
            else if(!off && (e==ENXIO || e==ENODEV) &&
                    !(X9_CAMERA_DUAL_MASK&(1u<<sensors[i].sensor))) {
                /* Optional lens (ultra-wide/telephoto) whose ID stage failed
                 * with a clean power cycle: exclude only this lens from the
                 * effective mask instead of faulting the whole camera, so
                 * x9_camera_lenses=1 degrades to the proven dual pair when
                 * an optional module is absent or a variant. A dirty
                 * power-off or a power-stage error still fails everything:
                 * the WL rails and buses are shared, so their state is not
                 * per-sensor. st.supported_mask below carries the effective
                 * mask; GET_LENSES and the userland backend check both read
                 * it, so the exclusion is consistent end to end. */
                printf("X9 CAMERA REGISTER: optional sensor=%u ID failed errno=%d; excluded, remaining lenses continue\n",sensors[i].sensor,e);
                e=0;
            }
            else if(e==ENXIO && !off) {
                /* Clean NACK with a clean power-off: still probe the next
                 * sensor (logs 335-345: back NACKs; front shares WL/DOVDD
                 * but not the module or i2c4, so its result splits shared
                 * supply from rear-module causes). Report the first error. */
                printf("X9 CAMERA REGISTER: sensor=%u NACK, continuing to next sensor\n",sensors[i].sensor);
                if(!first)first=e;
                e=0;
            }
        }
        if(!e && first)e=first;
        /* The dual pair is the minimum viable camera (the userland backend
         * check requires both bits); an admitted-but-unprobed back or front
         * sensor fails registration exactly as before. */
        if(!e && (probed&X9_CAMERA_DUAL_MASK)!=X9_CAMERA_DUAL_MASK)
            e=first?first:CAM_FAIL(ENODEV);
        if(!e){st.sensor=X9_CAMERA_BACK;st.supported_mask=probed;st.flags=X9_CAMERA_READY;}
        else {st.flags|=X9_CAMERA_FAULT;st.last_errno=e;}
    }
    lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);return e;
}
static int cam_completed_layout(const struct x9_camera_sensor *s,
    const struct x9_camera_completed_frame *f)
{
    return f->version!=X9_CAMERA_COMPLETED_VERSION || f->reserved || !f->raw ||
        f->stride!=((s->width/4*5+63)&~63u) ||
        (uint64_t)f->stride*s->height!=f->bytes || f->bytes>X9_CAMERA_MAX_BYTES ? EINVAL:0;
}
static int cam_completed_controls(const struct x9_camera_sensor *s,
    const struct x9_camera_completed_frame *f)
{
    return f->frame_lines<s->frame_lines || f->frame_lines>s->frame_max ||
        f->frame_lines%s->exposure_step || f->exposure_lines<s->exposure_min ||
        f->exposure_lines>f->frame_lines-s->margin || f->exposure_lines%s->exposure_step ||
        f->gain_code>s->gain_max || (s->gain_linear && f->gain_code<s->gain_unity) ? EIO:0;
}
int x9_camera_deliver(const struct x9_camera_completed_frame *completed)
{
    if(!completed)return CAM_FAIL(EINVAL);
    /* Snapshot the record, not a mutable provider structure during delivery.
     * raw itself must remain immutable/cache-visible through this call. */
    const struct x9_camera_completed_frame f=*completed;
    lck_mtx_lock(&camera_lock);int e=0;const struct x9_camera_sensor *s=sensor();
    const char *step="identity";
    if(!(st.flags&X9_CAMERA_STREAMING) || f.generation!=st.generation || f.sensor!=st.sensor)e=CAM_FAIL(ESTALE);
    if(!e) {
        step="layout-time";
        if(cam_completed_layout(s,&f) || !f.capture_id || !f.time || !f.sof_time ||
            f.sof_time>f.time || f.time>mach_absolute_time())e=CAM_FAIL(EINVAL);
    }
    if(!e) {
        step="sequence";
        if(f.capture_id<=last_capture_id || f.time<=last_capture_time || f.sof_time<=last_sof_time)e=CAM_FAIL(ESTALE);
    }
    if(!e){step="controls";e=cam_completed_controls(s,&f);}
    if(!e) {
        last_capture_id=f.capture_id;last_capture_time=f.time;last_sof_time=f.sof_time;
        step="reader-busy";
        if(frame_readers)e=CAM_FAIL(EBUSY); /* retire, never overwrite/replay a pinned CPU copy */
    }
    if(!e) {
        bool first_frame=!frame_bytes;
        memset(&head,0,sizeof(head));memcpy(head.magic,HAKIOS_CAMERA_MAGIC,8);
        head.header_bytes=sizeof(head);head.width=s->width;head.height=s->height;
        head.stride=f.stride;head.format=1;head.bayer=s->bayer;head.black=64;
        head.sequence=++st.frames;head.time=f.time;head.exposure_lines=f.exposure_lines;
        head.sof_time_lo=(uint32_t)f.sof_time;head.sof_time_hi=(uint32_t)(f.sof_time>>32);
        head.timing_flags=HAKIOS_CAMERA_TIMING_SOF;
        /* HakiOS ISP expects linear gain. Sony registers encode reciprocal
         * gain: unity/(unity-reg), NOT reg/unity. ABI units are linear Q10. */
        head.line_ns=s->line_ns;head.gain=s->gain_linear?
            1024*f.gain_code/s->gain_unity:1024*s->gain_unity/(s->gain_unity-f.gain_code);
        head.gain_unity=1024;head.gain_max=65536;
        head.frame_lines=f.frame_lines;head.frame_lines_min=s->frame_lines;head.frame_lines_max=s->frame_max;
        head.margin=s->margin;head.sensor=f.sensor;head.focus=HAKIOS_CAMERA_NO_FOCUS;
        { /* diag46: per-frame timing and MIPI receive errors, every 32 frames.
           * CSIRX_MAC_CSI2_IRQ_STATUS latches without IRQ_EN; Android treats
           * anything outside 0x324 (ECC ok, CRC ok, FS, FE) as an error and
           * clears it with 0xffffffff (phy_3_0.c:4813). */
            static uint64_t n,copy_ns,copy_max,last_time,gap_ns,err_frames,err_or,crc,ecc2,ecc1,sync;
            uint64_t t0=mach_absolute_time(),ns;
            memcpy(frame,f.raw,f.bytes);
            absolutetime_to_nanoseconds(mach_absolute_time()-t0,&ns);copy_ns+=ns;if(ns>copy_max)copy_max=ns;
            if(last_time && f.time>last_time){absolutetime_to_nanoseconds(f.time-last_time,&ns);gap_ns+=ns;}
            last_time=f.time;
            if(cam_mac.held && cam_mac.bank[CAM_MAC_A]) {
                uint32_t v=cam_rr(cam_mac.bank[CAM_MAC_A],0xc8),bad=v&~0x324u;
                if(bad){err_frames++;err_or|=bad;}
                if(v&(1u<<6))crc++;
                if(v&(1u<<4))ecc2++;
                if(v&(1u<<3))ecc1++;
                if(v&((1u<<0)|(1u<<7)|(1u<<13)))sync++;
                if(v)cam_rw(cam_mac.bank[CAM_MAC_A],0xc8,0xffffffffu);
            }
            if(++n==32) {
                printf("X9 CAMERA FRAME PERF: frames=32 interval_ms=%llu copy_us=%llu/%llu cpu_inval_us=%llu dev_flush_us=%llu "
                    "csi_err_frames=%llu err_or=%llx crc=%llu ecc2=%llu ecc1=%llu sync=%llu\n",
                    gap_ns/31/1000000,copy_ns/32/1000,copy_max/1000,cam_perf_cpu_ns/32/1000,cam_perf_dev_ns/32/1000,
                    err_frames,err_or,crc,ecc2,ecc1,sync);
                n=copy_ns=copy_max=gap_ns=err_frames=err_or=crc=ecc2=ecc1=sync=0;
                cam_perf_cpu_ns=cam_perf_dev_ns=0;
            }
        }
        frame_bytes=f.bytes;wakeup(&frame_bytes);
        { /* Black-preview triage: is the RAW itself dark/empty? RAW10 packed,
           * bytes 0..3 of each 5-byte group are the pixel MSBs (black=64/4). */
            static unsigned stats_logs;
            if(stats_logs<6 && f.stride>=5 && s->height) {
                uint64_t sum=0;uint32_t n=0,lo=255,hi=0,zero=0,sat=0;
                for(unsigned r=0;r<64;r++) {
                    const uint8_t *row=frame+(size_t)((r*2+1)*s->height/128)*f.stride;
                    for(unsigned g=0;g<64;g++) {
                        const uint8_t *p=row+(size_t)((g*2+1)*(f.stride/5)/128)*5;
                        for(unsigned k=0;k<4;k++){uint8_t v=p[k];sum+=v;n++;if(v<lo)lo=v;if(v>hi)hi=v;if(!v)zero++;if(v==255)sat++;}
                    }
                }
                stats_logs++;
                printf("X9 CAMERA RAW STATS: seq=%llu samples=%u msb_mean=%llu.%02llu min=%u max=%u zero=%u sat=%u "
                    "(10-bit ~= msb*4, black=64) exp_lines=%u gain_code=%u frame_lines=%u\n",
                    head.sequence,n,sum/n,(sum*100/n)%100,lo,hi,zero,sat,f.exposure_lines,f.gain_code,f.frame_lines);
            }
        }
        if(first_frame)
            printf("X9 CAMERA RAW PUBLISH: sensor=%u generation=%llu capture_id=%llu "
                "seq=%llu width=%u height=%u stride=%u bytes=%u sof=%llu done=%llu\n",
                f.sensor,f.generation,f.capture_id,head.sequence,s->width,s->height,
                f.stride,f.bytes,f.sof_time,f.time);
    } else {
        st.rejected++;st.last_errno=e;
        printf("X9 CAMERA RAW REJECT: step=%s errno=%d sensor=%u current_sensor=%u "
            "generation=%llu current_generation=%llu capture_id=%llu stride=%u bytes=%u "
            "sof=%llu done=%llu readers=%u rejected=%llu\n",step,e,f.sensor,st.sensor,
            f.generation,st.generation,f.capture_id,f.stride,f.bytes,f.sof_time,f.time,
            frame_readers,st.rejected);
    }
    lck_mtx_unlock(&camera_lock);return e;
}
#include "x9_camera_native_ae.h"
static struct cam_native_ae_state cam_native_ae;
#include "x9_camera_camsv.inc"
#include "x9_camera_irq.inc"
#include "x9_camera_native_ae.inc"
#include "x9_camera_native_capture.inc"
static int cam_open(dev_t d,int f,__unused int t,struct proc *p)
{
    /* Independent vnode/minor for non-mutating capability observers. A
     * mediaserver vendor lookup may run while the RAW/control fd is owned;
     * it must not acquire that open or close/STOP its active generation. */
    if(minor(d)==1)return (f&FWRITE)?CAM_FAIL(EPERM):0;
    if(minor(d)!=0)return CAM_FAIL(ENXIO);
    if(f&FWRITE) {
        char name[32];proc_name(proc_pid(p),name,sizeof(name));
        uid_t uid=kauth_cred_getuid(kauth_cred_get());
        if(st.mode==3) { if(uid)return CAM_FAIL(EPERM); }
        else if(st.mode!=2 || (uid && (uid!=501 || strcmp(name,"mediaserverd"))))return CAM_FAIL(EPERM);
    }
    if(cam_start_busy)return CAM_FAIL(EBUSY);
    lck_mtx_lock(&camera_control_lock);lck_mtx_lock(&camera_lock);
    /* An asynchronous real platform registration needs opened=false while
     * it admits/probes/restores. Reject an early mode2 controller open
     * without taking that ownership; status observers and mode3 are separate.
     * Callback presence alone is not READY, and fault/retained state cannot
     * become a usable controller by reopening its cdev. */
    int e=0;
    if(st.mode==2 && (hw.version!=X9_CAMERA_PLATFORM_VERSION ||
        !(st.flags&X9_CAMERA_READY)))e=CAM_FAIL(ENXIO);
    else if(st.mode==2 && (st.flags&(X9_CAMERA_FAULT|X9_CAMERA_RETAINED)))
        e=st.last_errno?st.last_errno:EIO;
    else if(opened)e=CAM_FAIL(EBUSY);
    if(!e && st.mode==2 && (session.latched || cam_stop_owned() || frame_readers ||
        (st.flags&X9_CAMERA_RETAINED)))e=session.last_errno?session.last_errno:EBUSY;
    if(!e){opened=true;control_stop_issued=false;}
    lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);return e;
}
static int cam_close(dev_t d,__unused int f,__unused int t,__unused struct proc *p)
{
    if(minor(d)==1)return 0;
    if(minor(d)!=0)return CAM_FAIL(ENXIO);
    lck_mtx_lock(&camera_control_lock);lck_mtx_lock(&camera_lock);
    int e=control_stop_issued?cam_stop_cached_close():stop();opened=false;
    if(frame && !session.latched && !cam_stop_owned() && !frame_readers &&
        !(st.flags&(X9_CAMERA_STREAMING|X9_CAMERA_RETAINED))) {
        kfree_data(frame,X9_CAMERA_MAX_BYTES);frame=NULL;
    }
    lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);return e;
}
static int cam_ioctl(dev_t d,u_long cmd,caddr_t data,int f,__unused struct proc *p)
{
    if(minor(d)>1)return CAM_FAIL(ENXIO);
    if(minor(d)==1 && cmd!=X9_CAMERA_GET_STATUS && cmd!=X9_CAMERA_GET_STOP &&
        cmd!=X9_CAMERA_GET_LENSES && cmd!=X9_CAMERA_GET_SWITCH_CAPACITY && cmd!=X9_CAMERA_SAY)return CAM_FAIL(EPERM);
    if(cmd==X9_CAMERA_SAY) {
        static unsigned say_lines;
        struct x9_camera_say *m=(struct x9_camera_say *)data;
        size_t n;
        m->text[sizeof(m->text)-1]=0;n=strlen(m->text);
        while(n && (m->text[n-1]=='\n' || m->text[n-1]=='\r'))m->text[--n]=0;
        if(say_lines<400){say_lines++;printf("X9 CAMERA BRIDGE: %s\n",m->text);}
        return 0;
    }
    if(cmd==X9_CAMERA_GET_STATUS) {
        lck_mtx_lock(&camera_lock);memcpy(data,&st,sizeof(st));lck_mtx_unlock(&camera_lock);return 0;
    }
    if(cmd==X9_CAMERA_GET_LENSES) {
        struct x9_camera_lenses r={.version=1,.count=X9_CAMERA_SENSOR_COUNT};
        lck_mtx_lock(&camera_lock);r.supported_mask=st.supported_mask;
        for(unsigned i=0;i<X9_CAMERA_SENSOR_COUNT;i++) {
            r.lens[i].sensor=sensors[i].sensor;r.lens[i].chip_id=cam_chip_ids[i];
            if(r.lens[i].chip_id==sensors[i].chip_id)r.lens[i].flags|=X9_CAMERA_PROBED;
            if((st.flags&X9_CAMERA_READY) && (st.supported_mask&(1u<<sensors[i].sensor)))
                r.lens[i].flags|=X9_CAMERA_READY;
        }
        lck_mtx_unlock(&camera_lock);memcpy(data,&r,sizeof(r));return 0;
    }
    if(cmd==X9_CAMERA_GET_STOP) {
        lck_mtx_lock(&camera_control_lock);memcpy(data,&cam_stop_last,sizeof(cam_stop_last));
        lck_mtx_unlock(&camera_control_lock);return 0;
    }
    if(cmd==X9_CAMERA_GET_SWITCH_CAPACITY) {
        struct x9_camera_switch_capacity r={.version=1,.flags=X9_CAMERA_SWITCH_SERIAL,
            .limit=CAM_NATIVE_MAX_PINNED_EPOCHS};
        /* All native epoch writers hold this control lock. No MMIO, new
         * reservation, map mutation or lease validation is performed here. */
        lck_mtx_lock(&camera_control_lock);
        r.retired=cam_native_epoch_count;
        if(r.retired+1<r.limit)r.remaining=r.limit-r.retired-1;
        lck_mtx_unlock(&camera_control_lock);
        memcpy(data,&r,sizeof(r));return 0;
    }
    if(cmd==X9_CAMERA_STOP) {
        int e=0;
        struct x9_camera_stop r,request={.version=1};
        if(st.mode!=2 || !(f&FWRITE))return CAM_FAIL(EPERM); /* cam_open enforces identity */
        memcpy(&r,data,sizeof(r));request.sensor=r.sensor;
        request.generation_before=r.generation_before;
        if((!x9_camera_sensor_valid(r.sensor)) || memcmp(&r,&request,sizeof(r)))return CAM_FAIL(EINVAL);
        lck_mtx_lock(&camera_control_lock);lck_mtx_lock(&camera_lock);
        if(!opened)e=CAM_FAIL(ENXIO);
        else if(control_stop_issued)e=CAM_FAIL(EALREADY);
        else if(r.sensor!=st.sensor || r.generation_before!=st.generation)e=CAM_FAIL(ESTALE);
        else {cam_stop_issue();memcpy(data,&cam_stop_last,sizeof(cam_stop_last));}
        lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);
        return e; /* accepted failures live in receipt; ioctl0 is NOT retirement */
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
        if(st.mode!=3 || !(f&FWRITE) || kauth_cred_getuid(kauth_cred_get()))return CAM_FAIL(EPERM);
        struct x9_camera_config r,request={.version=1};
        memcpy(&r,data,sizeof(r));request.sensor=r.sensor;
        if((!x9_camera_sensor_valid(r.sensor)) || memcmp(&r,&request,sizeof(r)))return CAM_FAIL(EINVAL);
        lck_mtx_lock(&camera_control_lock);(void)cam_standby(request.sensor,&r);
        lck_mtx_lock(&camera_lock);st.last_errno=r.last_errno;
        if(r.flags&X9_CAM_CFG_ATTEMPTED)st.probe_flags=r.probe.flags;
        if(!r.last_errno && (r.probe.flags&X9_CAM_PROBE_MATCHED)) {
            cam_sensor_id_record(r.sensor,r.probe.chip_id);st.probe_mask|=1u<<r.sensor;
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
        if(memcmp(&r,&request,sizeof(request)))return CAM_FAIL(EINVAL);
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
        if(st.mode!=3 || !(f&FWRITE) || kauth_cred_getuid(kauth_cred_get()))return CAM_FAIL(EPERM);
        struct x9_camera_resources r,request={.version=1};
        memcpy(&r,data,sizeof(r));if(memcmp(&r,&request,sizeof(r)))return CAM_FAIL(EINVAL);
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
        if(st.mode!=3 || !(f&FWRITE) || kauth_cred_getuid(kauth_cred_get()))return CAM_FAIL(EPERM);
        struct x9_camera_mm_status r,request={.version=1};
        memcpy(&r,data,sizeof(r));request.sensor=r.sensor;
        if((!x9_camera_sensor_valid(r.sensor)) || memcmp(&r,&request,sizeof(r)))return CAM_FAIL(EINVAL);
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
        if(st.mode!=3 || !(f&FWRITE) || kauth_cred_getuid(kauth_cred_get()))return CAM_FAIL(EPERM);
        struct x9_camera_receiver r,request={.version=1};
        memcpy(&r,data,sizeof(r));request.sensor=r.sensor;
        if((!x9_camera_sensor_valid(r.sensor)) || memcmp(&r,&request,sizeof(r)))return CAM_FAIL(EINVAL);
        lck_mtx_lock(&camera_control_lock);(void)cam_rx_probe(request.sensor,&r);
        lck_mtx_lock(&camera_lock);st.last_errno=r.last_errno;
        if(r.flags&X9_CAM_RX_LATCHED)st.flags|=X9_CAMERA_FAULT;
        /* Neither disabled receiver controls nor MM clocks claim resources. */
        lck_mtx_unlock(&camera_lock);
        memcpy(data,&r,sizeof(r));lck_mtx_unlock(&camera_control_lock);return 0;
    }
    if(cmd!=X9_CAMERA_PROBE_SENSOR)return CAM_FAIL(ENOTTY);
    if(st.mode!=3 || !(f&FWRITE) || kauth_cred_getuid(kauth_cred_get()))return CAM_FAIL(EPERM);
    struct x9_camera_probe *r=(struct x9_camera_probe *)data;
    struct x9_camera_probe request={.version=1,.sensor=r->sensor};
    if((!x9_camera_sensor_valid(r->sensor)) || memcmp(r,&request,sizeof(request)))return CAM_FAIL(EINVAL);
    lck_mtx_lock(&camera_control_lock);
    if(cam_resources_latched) {
        memset(r,0,sizeof(*r));r->version=1;r->sensor=request.sensor;
        r->last_errno=EIO;r->flags=X9_CAM_PROBE_LATCHED;
    } else (void)cam_sensor_probe(request.sensor,r);
    lck_mtx_lock(&camera_lock);
    st.probe_flags=r->flags;st.last_errno=r->last_errno;
    if((r->flags&(X9_CAM_PROBE_MATCHED|X9_CAM_PROBE_RESTORED))==
        (X9_CAM_PROBE_MATCHED|X9_CAM_PROBE_RESTORED) && !r->last_errno) {
        cam_sensor_id_record(r->sensor,r->chip_id);st.probe_mask|=1u<<r->sensor;
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
static int cam_write(dev_t d,struct uio *uio,__unused int flags)
{
    if(minor(d)!=0)return CAM_FAIL(EPERM);
    if(st.mode!=2)return CAM_FAIL(EPERM); /* mode3 has only fixed standby table ioctl, no raw writer */
    struct hakios_camera_command c;if(uio_resid(uio)!=sizeof(c))return CAM_FAIL(EINVAL);
    int e=uiomove((char*)&c,sizeof(c),uio);if(e)return e;if(c.bytes)return CAM_FAIL(ENOTSUP);
    if(cam_start_busy)return CAM_FAIL(EBUSY);
    /* Log 404: returning EBUSY lost the start (mediaserverd never retried).
     * Wait, holding no lock, so the open/close lands outside the 17-24 s
     * early-boot window where diag8-11 reset (logs 383-386, 420, 421). */
    if(c.command==HAKIOS_CAMERA_SENSOR && cam_boot_deferred()) {
        printf("X9 CAMERA: start waits for x9_cam_defer before touching hardware\n");
        while(cam_boot_deferred()) {
            assert_wait_timeout((event_t)&cam_start_busy,THREAD_UNINT,500,NSEC_PER_MSEC);
            thread_block(THREAD_CONTINUE_NULL);
        }
        printf("X9 CAMERA: deferred start proceeds\n");
    }
    lck_mtx_lock(&camera_control_lock);lck_mtx_lock(&camera_lock);
    if(!opened)e=CAM_FAIL(ENXIO);
    else if(control_stop_issued)e=ESHUTDOWN;
    else if(c.command==HAKIOS_CAMERA_SENSOR)e=start(c.a,c.b);
    else if(c.command==HAKIOS_CAMERA_EXPOSURE && (st.flags&X9_CAMERA_STREAMING)) {
        const struct x9_camera_sensor *s=sensor();uint32_t lines=c.c?c.c:s->frame_lines;
        if(!c.c && (uint64_t)c.a+s->margin>lines) {
            if(c.a>s->frame_max-s->margin){
                lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);return CAM_FAIL(EINVAL);
            }
            lines=c.a+s->margin;
        }
        if(lines>s->frame_max || lines<s->frame_lines || c.a<s->exposure_min ||
           c.a>lines-s->margin || c.a%s->exposure_step || lines%s->exposure_step || c.b<1024 || c.b>65536)e=CAM_FAIL(EINVAL);
        else {
            /* Worker SOF/DONE completion must not wait for the frame mutex
             * across AP I2C/engine scheduling. Control serialization still
             * pins this sensor/generation through the whole AE request. */
            lck_mtx_unlock(&camera_lock);
            e=hw.exposure(s,c.a,c.b,lines);
            lck_mtx_lock(&camera_lock);
            if(e){st.flags|=X9_CAMERA_FAULT;st.last_errno=e;(void)stop();}
        }
    } else e=CAM_FAIL(ENOTSUP); /* no fake focus, KEEP host export or arbitrary writes */
    lck_mtx_unlock(&camera_lock);lck_mtx_unlock(&camera_control_lock);
    { static unsigned write_logs;
      if(write_logs<24){write_logs++;
        printf("X9 CAMERA WRITE CALL: command=%u a=%u b=%u c=%u errno=%d\n",c.command,c.a,c.b,c.c,e);} }
    return e;
}
static int cam_read(dev_t d,struct uio *uio,__unused int flags)
{
    if(minor(d)!=0)return CAM_FAIL(ENOTSUP);
    if(uio_resid(uio)<sizeof(head))return EMSGSIZE;
    lck_mtx_lock(&camera_lock);int e=0;
    bool peek=uio_resid(uio)==sizeof(head),pinned=false;
    struct hakios_camera_frame snapshot;uint8_t *raw=NULL;uint32_t bytes=0;
    uint64_t generation=st.generation;
    if(!(st.flags&X9_CAMERA_STREAMING))e=CAM_FAIL(ENXIO);
    else {
        uint64_t interval;nanoseconds_to_absolutetime(1000000000,&interval);
        uint64_t deadline=mach_absolute_time()+interval;
        while(!frame_bytes || (!peek && (consumed==st.frames || frame_readers))) {
            uint64_t now=mach_absolute_time(),ns;
            if(now>=deadline){e=CAM_FAIL(EAGAIN);break;}
            absolutetime_to_nanoseconds(deadline-now,&ns);
            struct timespec timeout={(time_t)(ns/1000000000),(long)(ns%1000000000)};
            e=msleep(&frame_bytes,&camera_lock,PCATCH,"x9-camera",&timeout);
            if(e || generation!=st.generation || !(st.flags&X9_CAMERA_STREAMING))break;
        }
        if(e==EWOULDBLOCK)e=CAM_FAIL(EAGAIN);
        if(e==EAGAIN && generation==st.generation && (st.flags&X9_CAMERA_STREAMING) &&
           !frame_bytes && !session.first_raw_timeout_logged) {
            session.first_raw_timeout_logged=true;
            printf("X9 CAMERA RAW WAIT: sensor=%u generation=%llu errno=%d "
                "(first RAW unavailable after read deadline)\n",st.sensor,generation,e);
        }
        if(!e && generation!=st.generation)e=CAM_FAIL(ESTALE);
        if(!e && (!(st.flags&X9_CAMERA_STREAMING)||!frame_bytes))e=CAM_FAIL(ENXIO);
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
        if(!e && generation!=st.generation)e=CAM_FAIL(ESTALE);
        if(!e)consumed=snapshot.sequence; /* not a later st.frames */
        { static unsigned read_logs;
          if(read_logs<6){char name[32]={0};read_logs++;proc_selfname(name,sizeof(name));
            printf("X9 CAMERA RAW READ: by=%s seq=%llu bytes=%u errno=%d\n",name,snapshot.sequence,bytes,e);} }
        frame_readers--;wakeup(&frame_readers);wakeup(&frame_bytes);
        lck_mtx_unlock(&camera_lock);
    }
    { static unsigned call_logs;
      if(call_logs<24){char name[32]={0};call_logs++;proc_selfname(name,sizeof(name));
        printf("X9 CAMERA READ CALL: by=%s peek=%d bytes=%u seq=%llu errno=%d\n",
            name,peek,bytes,e?0ull:snapshot.sequence,e);} }
    return e;
}
static const struct cdevsw cam_cdev={.d_open=cam_open,.d_close=cam_close,.d_read=cam_read,
    .d_write=cam_write,.d_ioctl=cam_ioctl,.d_stop=eno_stop,.d_reset=eno_reset,
    .d_select=eno_select,.d_mmap=eno_mmap,.d_strategy=eno_strat};
#include "x9_camera_native_sensor.inc"
static void cam_native_register_worker(void *arg,wait_result_t result)
{
    (void)arg;(void)result;
    /* A getter is not admission. This single ordinary thread performs the
     * real engine admission and both physical power/ID/close probes. No
     * retry after a partial probe, no boot-time stream or artificial READY.
     * Configure later checks the live UFS floor and actual MM suppliers. */
    int e=x9_camera_native_sensor_register(x9_camera_native_capture_engine());
    lck_mtx_lock(&camera_lock);
    if(e) {
        st.flags|=X9_CAMERA_FAULT;
        if(!st.last_errno)st.last_errno=e;
    }
    printf("X9 CAMERA NATIVE REGISTER: errno=%d platform=%u flags=%x IDs=%04x/%04x "
        "(physical ID admission only; no RAW/preview claim)\n",
        e,hw.version,st.flags,st.chip_id[0],st.chip_id[1]);
    lck_mtx_unlock(&camera_lock);
    (void)thread_terminate(current_thread());
}
void sweet_x9_camera_init(void)
{
    /* Additional physical lenses are opt-in pending device acceptance. */
    if(sweet_x9_arg("x9_camera_tele",0)==1)cam_native_mask|=1u<<X9_CAMERA_TELEPHOTO;
    if(sweet_x9_arg("x9_camera_uw",0)==1)cam_native_mask|=1u<<X9_CAMERA_ULTRAWIDE;
    if(sweet_x9_arg("x9_camera_lenses",0)==1)cam_native_mask=X9_CAMERA_ALL_MASK;
    if(!sweet_early_is_x9())return;
    int mode=sweet_x9_arg("x9_camera",0);if(mode<1 || mode>3)return;
    if(st.mode)return;st.mode=mode;
    int major=cdevsw_add(-1,&cam_cdev);if(major<0){st.last_errno=ENXIO;return;}
    /* Captured 20H392 launchd plist runs mediaserverd as mobile (501:501),
     * not macOS _mediaserverd (32). The status device is
     * NOT evidence of capture capability. Mode2 power/ID admission runs
     * only in the separate one-shot registration thread below. */
    if(!devfs_make_node(makedev(major,0),DEVFS_CHAR,mode==3?0:501,mode==3?0:501,0600,"x9-camera") ||
       !devfs_make_node(makedev(major,1),DEVFS_CHAR,mode==3?0:501,mode==3?0:501,0400,"x9-camera-status"))
        st.last_errno=ENOMEM;
    printf("X9 CAMERA: mode=%d hhmain(i2c4/1a) hhfront(i2c2/10); platform=%u, no fake frames\n",mode,hw.version);
    if(mode==3)printf("X9 CAMERA PROBE: root probe/standby ioctl only, no boot-time MMIO/power, no iOS install; shared providers required\n");
    if(mode==2 && !st.last_errno) {
        thread_t thread;
        if(kernel_thread_start(cam_native_register_worker,NULL,&thread)==KERN_SUCCESS)
            thread_deallocate(thread);
        else {
            lck_mtx_lock(&camera_lock);
            st.last_errno=ENOMEM;st.flags|=X9_CAMERA_FAULT;
            lck_mtx_unlock(&camera_lock);
        }
    }
}
