/* Native Actuator endpoint. Real-time writes only convert/enqueue PCM;
 * MMIO/I2C runs on one bounded worker, never on mediaserverd's audio thread.
 * Readiness means the actual AW chip/configuration was read back, not merely
 * that devfs or an audio UID exists. Generation/sequence fence old sessions.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <pexpert/device_tree.h>
#include "x9_haptic.h"
#include "x9_haptic_convert.h"
#include "x9_haptic_stock_waves.h"
#include "x9_haptic_stock.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *,int);
LCK_GRP_DECLARE(haptic_group,"x9-haptic");
LCK_MTX_DECLARE(haptic_lock,&haptic_group);
LCK_MTX_DECLARE(haptic_write_lock,&haptic_group);
/* diag30: who holds the haptic locks. Camera's audio session restart of the
 * Actuator stalled mediaserverd for 9 s (AQMEIO timeout) before any open
 * reached this driver; record the last acquirer of each lock and log a busy
 * lock or a long wait (state=haptic_lock, write=haptic_write_lock). */
static const char *x9h_holder[2];
static uint64_t x9h_since[2];
static uint64_t x9h_now(void)
{uint64_t t,ns;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);return ns;}
static void x9h_lock(lck_mtx_t *m,unsigned w,const char *fn)
{
    if(lck_mtx_try_lock(m)){x9h_holder[w]=fn;x9h_since[w]=x9h_now();return;}
    uint64_t t0=x9h_now(),held=t0-x9h_since[w];const char *h=x9h_holder[w];
    char name[32]={0};proc_selfname(name,sizeof(name));
    bool media=!strncmp(name,"mediaserverd",12);
    static unsigned busy_logs;
    if((media || held>=50000000ULL) && busy_logs<60){busy_logs++;
        printf("X9 HAPTIC LOCK BUSY: lock=%s caller=%s in %s holder=%s held_ms=%llu\n",
            w?"write":"state",name,fn,h?h:"?",held/1000000);}
    lck_mtx_lock(m);
    uint64_t now=x9h_now(),waited=now-t0;x9h_holder[w]=fn;x9h_since[w]=now;
    static unsigned wait_logs;
    if(waited>=200000000ULL && wait_logs<40){wait_logs++;
        printf("X9 HAPTIC LOCK WAIT: lock=%s caller=%s in %s waited_ms=%llu previous holder=%s\n",
            w?"write":"state",name,fn,waited/1000000,h?h:"?");}
}
static void x9h_call(const char *what,dev_t d,unsigned long cmd)
{
    char name[32]={0};proc_selfname(name,sizeof(name));
    static unsigned logs;
    if(strncmp(name,"mediaserverd",12) || logs>=120)return;
    logs++;printf("X9 HAPTIC CALL: %s minor=%d cmd=%lx by %s\n",what,minor(d),cmd,name);
}
static struct x9_haptic_status hs={.version=X9_HAPTIC_VERSION,
    .input_rate=X9_HAPTIC_INPUT_RATE,.rtp_rate=X9_HAPTIC_RTP_RATE,.f0_millihz=173000};
static int8_t ring[X9_HAPTIC_RING];
static uint32_t head,count,scene_frame,scene_intensity,scene_sharpness;
static uint64_t sequence,on_at,cool_until,last_signal_ns;
static bool writer,draining,scene_active,quiet_stop;
static struct x9_haptic_converter converter;
static struct x9_haptic_pattern pattern;
static const struct x9_haptic_stock_wave *stock_wave;
static struct x9_haptic_stock stock;
static uint32_t feel_flags=X9_HS_FEEL_ALL;
static uint8_t scene_kind; /* 0 none, 1 one-shot stock scene, 2 envelope buzz */
static uint32_t stock_peak_tap=127,stock_peak_light=12,stock_peak_buzz=126;
static uint32_t stock_wave_peak(const struct x9_haptic_stock_wave *w)
{
    if(w==&x9_stock_wave_tap_light)return stock_peak_light;
    if(w==&x9_stock_wave_buzz)return stock_peak_buzz;
    return stock_peak_tap;
}
/* Intensity that lands a stock wave's native peak on a desired output
 * peak (0..127): the worker scales samples by scene_intensity/1000. */
static uint32_t stock_level_intensity(const struct x9_haptic_stock_wave *w,uint32_t level)
{uint32_t pk=stock_wave_peak(w);return pk?(level*1000u)/pk:1000;}
static uint32_t wave_peak_scan(const int8_t *s,uint32_t n)
{uint32_t pk=0;for(uint32_t i=0;i<n;i++){int32_t a=s[i]<0?-(int32_t)s[i]:s[i];
    if((uint32_t)a>pk)pk=(uint32_t)a;}return pk;}
/* Kept off the kernel stack; write_lock protects copyin/conversion. */
static struct x9_haptic_packet packet;
static uint64_t now_ns(void)
{uint64_t t,ns;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);return ns;}
/* diag33 breadcrumb: a SoC hang at camera prepare:smmu resets the board and
 * the USB console loses its last ~1 s. TOPRGU NONRST (0x1c010020, printed by
 * the shim as "x9 prior reset TOPRGU NONRST" on the next boot) receives
 * 0xC5 | camera step FNV16 <<8 | haptic state. State: 0 idle, 0x1n hidle
 * scanning I2C bank n, 0x2c htransfer setup, 0x3c htransfer START/poll. */
static vm_offset_t x9_crumb_rgu;
static volatile uint32_t x9_crumb_cam,x9_crumb_hap,x9_crumb_xfers;
static volatile uint64_t x9_crumb_cam_ns;
static void x9_crumb_write(void)
{
    if(!x9_crumb_rgu)return;
    *(volatile uint32_t *)(x9_crumb_rgu+0x20)=0xC5000000u|((x9_crumb_cam&0xffffu)<<8)|(x9_crumb_hap&0xffu);
    __asm__ volatile("dsb sy":::"memory");
}
void x9_crumb_cam_step(const char *step);
void x9_crumb_cam_step(const char *step)
{
    uint32_t h=2166136261u;
    for(const char *p=step;p && *p;p++)h=(h^(uint8_t)*p)*16777619u;
    x9_crumb_cam=(h^(h>>16))&0xffffu;x9_crumb_cam_ns=now_ns();x9_crumb_write();
}
#define x9_crumb_hap_set(v) do{x9_crumb_hap=(v);x9_crumb_write();}while(0)
#include "x9_haptic_aw8693xs.inc"
_Static_assert(offsetof(struct x9_haptic_packet,samples)==24,"haptic packet ABI");
_Static_assert(sizeof(struct x9_haptic_status)==120,"haptic status ABI");
int sweet_x9_haptic_mode(void)
{return (int)__atomic_load_n(&hs.mode,__ATOMIC_ACQUIRE);}
static bool allowed(struct proc *p)
{
    uid_t uid=kauth_cred_getuid(kauth_cred_get());if(!uid)return true;
    char name[32]={0};proc_name(proc_pid(p),name,sizeof(name));
    return uid==501 && !strcmp(name,"mediaserverd");
}
/* Per-stream aggregate (first 16): proves whether non-zero RTP samples
 * reached the FIFO in RTP_GO, without per-sample serial lines. */
static uint64_t st_accepted,st_submitted;static unsigned st_peak,st_gos,st_logs;
static uint8_t st_state_or;
static void stream_summary(void)
{
    if(!(hs.flags&X9_HAPTIC_RUNNING) || st_logs>=48)return;
    st_logs++;
    printf("X9 HAPTIC: stream gen=%llu accepted=%llu submitted=%llu in_peak=%d peak=%u go=%u state_or=%02x sysst=%02x underruns=%llu\n",
        hs.generation,hs.accepted_frames-st_accepted,hs.submitted_frames-st_submitted,
        converter.in_peak,st_peak,st_gos,st_state_or,hs.hw_fault,hs.underruns);
}
static void stream_reset(void)
{st_accepted=hs.accepted_frames;st_submitted=hs.submitted_frames;st_peak=st_gos=0;st_state_or=0;}
static int stop_locked(void)
{
    stream_summary();
    hs.flags&=~X9_HAPTIC_RUNNING;draining=scene_active=quiet_stop=false;stock_wave=NULL;
    scene_kind=0;x9_haptic_stock_init(&stock,0,0);
    ++hs.generation;head=count=0;sequence=0;
    int e=hoff();on_at=last_signal_ns=0;hs.last_stop_ns=now_ns();
    if(e)hs.last_errno=e;
    bzero(ring,sizeof(ring));bzero(&converter,sizeof(converter));wakeup(&count);return e;
}
static void fail_locked(int e)
{
    hs.flags|=X9_HAPTIC_FAULT;hs.flags&=~X9_HAPTIC_READY;hs.last_errno=e;
    (void)stop_locked();
    printf("X9 HAPTIC: fault errno=%d state=%x flags=%x queued retired\n",e,hs.hw_state,hs.flags);
}
static int start_locked(struct x9_haptic_start *s)
{
    if(s->version!=1 || s->conversion>X9_HAPTIC_RETUNE || s->intensity>1000 || s->sharpness>1000)return EINVAL;
    if(hs.mode<2 || !(hs.flags&X9_HAPTIC_READY))return ENXIO;
    if(now_ns()<cool_until)return EAGAIN;
    if(stock.hold){x9_hs_hold_cancel(&stock);(void)stop_locked();}
    if((hs.flags&X9_HAPTIC_RUNNING) || hb.active)return EBUSY;
    hs.flags&=~X9_HAPTIC_THROTTLED;hs.flags|=X9_HAPTIC_RUNNING;stream_reset();
    ++hs.generation;head=count=0;sequence=0;draining=scene_active=false;stock_wave=NULL;
    x9_haptic_converter_init(&converter,hs.f0_millihz,s->conversion,s->intensity,s->sharpness);
    x9_haptic_stock_init(&stock,feel_flags&(X9_HS_FEEL_GRADE|X9_HS_FEEL_HOLD),s->intensity);
    hs.last_write_ns=now_ns();s->generation=hs.generation;return 0;
}
static int haptic_open(dev_t d,int f,__unused int t,struct proc *p)
{
    x9h_call("open",d,(unsigned long)f);
    if(minor(d)==0)return f&FWRITE?EACCES:0;
    if(minor(d)!=1 || !(f&FWRITE) || !allowed(p))return EPERM;
    x9h_lock(&haptic_lock,0,__func__);int e=hs.mode<2?EPERM:writer?EBUSY:0;
    if(!e)writer=true;
    static unsigned opens;
    if(++opens<=8 || e){char name[32]={0};proc_name(proc_pid(p),name,sizeof(name));
        printf("X9 HAPTIC: writer open #%u by %s errno=%d\n",opens,name,e);}
    lck_mtx_unlock(&haptic_lock);return e;
}
static int haptic_close(dev_t d,__unused int f,__unused int t,__unused struct proc *p)
{
    if(minor(d)!=1)return 0;
    x9h_lock(&haptic_write_lock,1,__func__);x9h_lock(&haptic_lock,0,__func__);
    int e=stop_locked();writer=false;
    bzero(&packet,sizeof(packet));lck_mtx_unlock(&haptic_lock);lck_mtx_unlock(&haptic_write_lock);return e;
}
static int haptic_read(dev_t d,struct uio *u,__unused int flags)
{
    if(minor(d)!=0 || uio_offset(u) || uio_resid(u)<sizeof(hs))return EINVAL;
    struct x9_haptic_status s;
    x9h_lock(&haptic_lock,0,__func__);s=hs;s.queued_frames=count;lck_mtx_unlock(&haptic_lock);
    return uiomove((char *)&s,sizeof(s),u);
}
static void stock_start_locked(const struct x9_haptic_stock_wave *w,uint32_t intensity)
{stock_wave=w;scene_frame=0;scene_intensity=intensity;scene_active=true;scene_kind=1;
    x9_hs_hold_cancel(&stock);wakeup(&count);}
static int haptic_write(dev_t d,struct uio *u,__unused int flags)
{
    if(minor(d)!=1 || !uio_isuserspace(u) || uio_iovcnt(u)!=1 ||
       uio_resid(u)<24 || uio_resid(u)>sizeof(packet))return EINVAL;
    if(!lck_mtx_try_lock(&haptic_write_lock))return EAGAIN;
    x9h_holder[1]=__func__;x9h_since[1]=x9h_now();
    size_t bytes=(size_t)uio_resid(u);
    user_addr_t address=0;user_size_t length=0;
    int e=uio_getiov(u,0,&address,&length)?EINVAL:0;
    if(!e && (length!=bytes || address>UINT64_MAX-bytes))e=EINVAL;
    if(!e)e=copyin(address,&packet,24);
    if(!e && (packet.version!=1 || !packet.frames || packet.frames>X9_HAPTIC_MAX_FRAMES ||
        bytes!=24+(size_t)packet.frames*2))e=EINVAL;
    if(!e)e=copyin(address+24,packet.samples,packet.frames*2);
    if(!e && !lck_mtx_try_lock(&haptic_lock))e=EAGAIN;
    else if(!e) {
        uint32_t output=(converter.decimator+packet.frames)/4;
        if(!writer || !(hs.flags&X9_HAPTIC_RUNNING))e=EPIPE;
        else if(packet.generation!=hs.generation)e=ESTALE;
        else if(packet.sequence!=sequence)e=EINVAL;
        else if(hs.mode!=3 && (scene_active || draining))e=EBUSY;
        else if(hs.mode!=3 && count+output>X9_HAPTIC_RING)e=EAGAIN;
        if(!e) {
            bool cooling=now_ns()<cool_until;
            if(!cooling)hs.flags&=~X9_HAPTIC_THROTTLED;
            if(hs.mode==3) {
                /* Stock mode: Apple PCM is an event cue, not drive content.
                 * The engine (x9_haptic_stock.h) detects onsets and grades
                 * them by captured peak; playback/refractory input is
                 * consumed, not queued at the packet layer, so the HAL
                 * packet contract never sees EBUSY mid-stream. */
                for(unsigned i=0;i<packet.frames;i++) {
                    uint32_t ev=x9_haptic_stock_feed(&stock,packet.samples[i],
                        (scene_active || draining)?1:0);
                    if((ev&X9_HS_EV_FIRE) && !cooling) {
                        const struct x9_haptic_stock_wave *w=
                            stock.fire_grade==X9_HS_GRADE_LIGHT?
                            &x9_stock_wave_tap_light:&x9_stock_wave_tap;
                        stock_start_locked(w,stock_level_intensity(w,stock.fire_level));
                    }
                }
                converter.in_peak=stock.in_peak;
            } else for(unsigned i=0;i<packet.frames;i++) {
                int8_t v;if(x9_haptic_convert_sample(&converter,packet.samples[i],&v)) {
                    if(cooling)++hs.throttled_rtp_frames;
                    else {ring[(head+count)%X9_HAPTIC_RING]=v;++count;}
                }
            }
            ++sequence;hs.accepted_frames+=packet.frames;hs.last_write_ns=now_ns();
            /* Commit UIO only AFTER enqueue. EAGAIN must leave resid intact
             * or write(2) can report a false successful/partial PCM packet. */
            uio_update(u,bytes);wakeup(&count);
        } else if(e!=EAGAIN)hs.rejected_frames+=packet.frames;
        lck_mtx_unlock(&haptic_lock);
    }
    lck_mtx_unlock(&haptic_write_lock);return e;
}
static int haptic_ioctl(dev_t d,u_long cmd,caddr_t data,__unused int f,struct proc *p)
{
    if(cmd!=X9_HAPTIC_GET_STATUS_NOWAIT)x9h_call("ioctl",d,cmd);
    if(cmd==X9_HAPTIC_GET_STATUS_NOWAIT) {
        if(!lck_mtx_try_lock(&haptic_lock))return EAGAIN;
        hs.queued_frames=count;memcpy(data,&hs,sizeof(hs));lck_mtx_unlock(&haptic_lock);return 0;
    }
    if(cmd!=X9_HAPTIC_GET_STATUS && (minor(d)!=1 || !allowed(p)))return EPERM;
    /* Control and PCM copyin use one order. STOP cannot retire a session
     * midway through conversion; no worker ever takes write_lock. */
    x9h_lock(&haptic_write_lock,1,__func__);x9h_lock(&haptic_lock,0,__func__);int e=0;
    if(cmd==X9_HAPTIC_GET_STATUS){hs.queued_frames=count;memcpy(data,&hs,sizeof(hs));}
    else if(!writer)e=EPERM;
    else if(cmd==X9_HAPTIC_START) {
        e=start_locked((void *)data);
        static unsigned starts;
        if(++starts<=8 || e)printf("X9 HAPTIC: START #%u errno=%d flags=%x\n",starts,e,hs.flags);
    }
    else if(cmd==X9_HAPTIC_STOP)e=stop_locked();
    else if(cmd==X9_HAPTIC_DRAIN) {
        if(!(hs.flags&X9_HAPTIC_RUNNING))e=EPIPE;
        else {
            /* A final partial decimation group is explicitly zero-padded,
             * never silently discarded at an arbitrary packet boundary. */
            if(converter.decimator && count==X9_HAPTIC_RING)e=EAGAIN;
            else {
                while(converter.decimator) {
                    int8_t v;if(x9_haptic_convert_sample(&converter,0,&v))
                        {ring[(head+count)%X9_HAPTIC_RING]=v;++count;}
                }
                draining=true;wakeup(&count);
            }
        }
    } else if(cmd==X9_HAPTIC_EFFECT) {
        struct x9_haptic_effect *s=(void *)data;
        if(s->version!=1 || s->reserved || s->intensity>1000 || s->sharpness>1000)e=EINVAL;
        else if(hs.mode==3) {
            /* Stock mode: fixed stock waveforms per scene class; sharpness has
             * no meaning for a recorded transient and is accepted but unused. */
            const struct x9_haptic_stock_wave *w=&x9_stock_wave_tap;
            if(s->scene<X9_HAPTIC_SELECTION || s->scene>=X9_HAPTIC_SCENE_COUNT)e=EINVAL;
            else if(s->scene==X9_HAPTIC_NOTIFICATION)w=&x9_stock_wave_notify;
            else if(s->scene==X9_HAPTIC_RINGTONE || s->scene==X9_HAPTIC_ALARM ||
                    s->scene==X9_HAPTIC_GAME_CONTINUOUS)w=&x9_stock_wave_buzz;
            if(!e) {
                struct x9_haptic_start start={.version=1,.conversion=1,.intensity=s->intensity,.sharpness=s->sharpness};
                e=start_locked(&start);
                if(!e)stock_start_locked(w,s->intensity);
            }
        } else {
            struct x9_haptic_pattern next;
            if(x9_haptic_scene_pattern(s->scene,s->duration_ms,&next))e=EINVAL;
            else {
                struct x9_haptic_start start={.version=1,.conversion=1,.intensity=s->intensity,.sharpness=s->sharpness};
                e=start_locked(&start);
                if(!e){pattern=next;scene_active=true;scene_frame=0;
                    scene_intensity=s->intensity;scene_sharpness=s->sharpness;wakeup(&count);}
            }
        }
    } else e=ENOTTY;
    lck_mtx_unlock(&haptic_lock);lck_mtx_unlock(&haptic_write_lock);return e;
}
static void haptic_worker(__unused void *arg,__unused wait_result_t wait)
{
    for(;;) {
        x9h_lock(&haptic_lock,0,__func__);
        uint64_t now=now_ns();
        if(hs.flags&X9_HAPTIC_RUNNING) {
            /* An explicit client must not hold the chip running forever.
             * Changes of amplitude/sequence do not renew continuous-on time. */
            /* Scenes only: OPPO plays RTP streams (ringtones) without a cap, and
             * log 327 patterns were cut into restarts by this 1 s limit. */
            if(stock.hold && !scene_active && !draining &&
               x9_hs_hold_expired(&stock,now)) {
                /* Hold elapsed with no follow-up event: retire exactly
                 * like the drained stream would have at its end. */
                x9_hs_hold_cancel(&stock);
                int stopped=stop_locked();if(stopped)fail_locked(stopped);
            } else if(scene_active && on_at && now-on_at>=1000000000ULL) {
                hs.flags|=X9_HAPTIC_THROTTLED;cool_until=now+1000000000ULL;
                hs.throttled_rtp_frames+=count;head=count=0;
                int e=hoff();on_at=0;quiet_stop=true;
                if(e)fail_locked(e);
                /* Keep an armed PCM engine usable after a long continuous
                 * haptic: generation stays stable and input during cooling
                 * advances conversion time, with explicit throttled counts.
                 * We do not replay that interval after cooling ends. */
                if(scene_active){scene_active=false;draining=true;}
            } else if(!scene_active && !draining && now-hs.last_write_ns>100000000ULL) {
                /* A long-lived Actuator engine may stop producing PCM while
                 * idle between UIKit taps. Park the physical chip, not the
                 * client's generation. A stale nonempty queue is different:
                 * it must not be replayed as a new feedback later. */
                if(count){++hs.underruns;fail_locked(ETIMEDOUT);}
                else if(hb.active){int e=hoff();on_at=0;quiet_stop=true;if(e)fail_locked(e);}
            } else {
                if(scene_active && hs.mode==3) {
                    while(count<X9_HAPTIC_RING && scene_frame<stock_wave->len) {
                        int32_t v=(int32_t)stock_wave->samples[scene_frame]*(int32_t)scene_intensity/1000;
                        ring[(head+count)%X9_HAPTIC_RING]=(int8_t)v;++scene_frame;++count;
                    }
                    if(scene_frame==stock_wave->len){scene_active=false;draining=true;}
                } else if(scene_active) {
                    while(count<240 && scene_frame<pattern.duration_ms*12) {
                        ring[(head+count)%X9_HAPTIC_RING]=x9_haptic_scene_sample(&pattern,
                            scene_frame,converter.phase,scene_intensity,scene_sharpness);
                        converter.phase+=converter.step;++scene_frame;++count;
                    }
                    if(scene_frame==pattern.duration_ms*12){scene_active=false;draining=true;}
                }
                int e=0;
                /* Whole silent PCM blocks don't drive an idle motor. Keep
                 * in-block leading zeros once there is any signal, retaining
                 * their time relative to subsequent transients. */
                bool signal=false;
                for(unsigned i=0;i<count;i++)if(ring[(head+i)%X9_HAPTIC_RING]){signal=true;break;}
                /* While running, keep streaming silence for 200 ms after the
                 * last signal (OPPO streams continuously): parking 20 ms
                 * after each tap made every keystroke pay GO + boost
                 * startup again (log 329/330 "delayed"). */
                if(!scene_active && !draining && !signal &&
                   (!hb.active || (last_signal_ns && now-last_signal_ns>=200000000))) {
                    head=count=0;quiet_stop=true;
                }
                if(!hb.active) {
                    /* 20 ms prefill: 10 ms (log 327) ran the FIFO dry between
                     * mediaserverd writes; 30 ms (log 329) was felt as delay. */
                    if(count>=240 || (draining && count)) {
                        e=hgo();st_state_or|=(uint8_t)hs.hw_state;
                        if(!e){on_at=now_ns();quiet_stop=false;++st_gos;}
                    }
                }
                if(!e && hb.active) {
                    unsigned budget=120;
                    while(count && budget) {
                        unsigned n=MIN(count,MIN(15u,MIN(budget,X9_HAPTIC_RING-head)));
                        e=hfeed(ring+head,n);
                        if(e==EAGAIN){e=0;break;}if(e)break;
                        for(unsigned i=0;i<n;i++){int a=ring[head+i]<0?-ring[head+i]:ring[head+i];
                            if((unsigned)a>st_peak)st_peak=(unsigned)a;}
                        for(unsigned i=0;i<n;i++)if(ring[head+i]){last_signal_ns=now_ns();break;}
                        head=(head+n)%X9_HAPTIC_RING;count-=n;budget-=n;hs.submitted_frames+=n;
                    }
                    if(!e && !count) {
                        uint8_t fifo;e=hget(4,&fifo);if(!e)e=hstate();
                        if(!e && (fifo&1) && !(hs.hw_state&15)) { /* rtp_play: STATE_MASK */
                            /* Real FIFO empty + native standby, not elapsed
                             * wall time, proves this hardware run ended. */
                            hb.active=false;on_at=0;
                            if(!draining && !quiet_stop)++hs.underruns;
                        }
                    }
                }
                if(e)fail_locked(e);
                else if(draining && !count && !hb.active) {
                    ++hs.completed_streams;
                    if(hs.mode==3 && (feel_flags&X9_HS_FEEL_HOLD)) {
                        /* Feel: a 15 ms tap must not retire the session.
                         * Keep generation + RUNNING and hold for 200 ms
                         * (the PCM path's silence keepalive window); the
                         * next HAL packet continues this stream instead of
                         * dying on EPIPE until the 500-tick reopen poll.
                         * completed_streams still counts the finished
                         * hardware run. */
                        stream_summary();stream_reset();
                        draining=false;scene_kind=0;
                        x9_hs_hold_begin(&stock,now);
                    } else {int stopped=stop_locked();if(stopped)fail_locked(stopped);}
                }
            }
        }
        lck_mtx_unlock(&haptic_lock);
        /* Bounded 1ms wakeup, no busy-spin on the audio thread. */
        uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);
        assert_wait_deadline((event_t)&count,THREAD_UNINT,deadline);
        thread_block(THREAD_CONTINUE_NULL);
    }
}
static struct cdevsw haptic_cdev={.d_open=haptic_open,.d_close=haptic_close,
    .d_read=haptic_read,.d_write=haptic_write,.d_ioctl=haptic_ioctl,
    .d_stop=(stop_fcn_t *)nulldev,.d_reset=(reset_fcn_t *)nulldev,.d_ttys=NULL,
    .d_select=(select_fcn_t *)seltrue,.d_mmap=(mmap_fcn_t *)eno_mmap,
    .d_strategy=(strategy_fcn_t *)eno_strat,.d_type=0};
void sweet_x9_haptic_init(void)
{
    if(!sweet_early_is_x9())return;
    /* Default on (user: good features default-on); x9_haptic=0 disables.
     * The log-296 hang after haptic init was io_submap exhaustion, not the
     * haptic itself; its stage-12 busy bank is now logged/handled. */
    int mode=sweet_x9_arg("x9_haptic",3);if(mode<0 || mode>3)mode=0;
    int feel=sweet_x9_arg("x9_haptic_feel",15);if(feel<0 || feel>15)feel=15;
    feel_flags=(uint32_t)feel;
    hs.mode=(unsigned)mode;if(!mode)return;
    int f0=sweet_x9_arg("x9_haptic_f0",1730);
    if(f0<1600 || f0>1900){hs.last_errno=EINVAL;hs.flags=X9_HAPTIC_FAULT;}
    else hs.f0_millihz=(unsigned)f0*100;
    int major=cdevsw_add(-1,&haptic_cdev);if(major<0)return;
    devfs_make_node(makedev(major,0),DEVFS_CHAR,0,0,0444,"x9-haptic-status");
    if(mode>=2) {
        if(!(hs.flags&X9_HAPTIC_FAULT)) {
            int e=hboard_init();if(e){hs.last_errno=e;hs.flags|=X9_HAPTIC_FAULT;}
            else hs.flags=X9_HAPTIC_READY;
        }
        thread_t thread=THREAD_NULL;
        kern_return_t kr=kernel_thread_start(haptic_worker,NULL,&thread);
        if(kr==KERN_SUCCESS)thread_deallocate(thread);
        else {hs.last_errno=ENOMEM;hs.flags=X9_HAPTIC_FAULT;(void)hoff();}
        devfs_make_node(makedev(major,1),DEVFS_CHAR,501,501,0600,"x9-haptic");
        if(mode==3) {
            stock_peak_tap=wave_peak_scan(x9_stock_tap,x9_stock_wave_tap.len);
            stock_peak_light=wave_peak_scan(x9_stock_tap_light,x9_stock_wave_tap_light.len);
            stock_peak_buzz=wave_peak_scan(x9_stock_buzz,x9_stock_wave_buzz.len);
            printf("X9 HAPTIC: stock waves tap=%u notify=%u buzz=%u light=%u samples @12k feel=%u peaks=%u/%u/%u\n",
                x9_stock_wave_tap.len,x9_stock_wave_notify.len,x9_stock_wave_buzz.len,
                x9_stock_wave_tap_light.len,feel_flags,
                stock_peak_tap,stock_peak_light,stock_peak_buzz);
        }
    }
    x9_crumb_rgu=ml_io_map(0x1c010000,0x1000);
    if(x9_crumb_rgu)*(volatile uint32_t *)(x9_crumb_rgu+0x20)=0;
    printf("X9 HAPTIC: crumb TOPRGU NONRST mapped=%d (cleared)\n",x9_crumb_rgu!=0);
    printf("X9 HAPTIC: mode=%u flags=%x errno=%u devfs ready; playback validation separate\n",
        hs.mode,hs.flags,hs.last_errno);
}
