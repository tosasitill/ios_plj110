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
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *,int);
LCK_GRP_DECLARE(haptic_group,"x9-haptic");
LCK_MTX_DECLARE(haptic_lock,&haptic_group);
LCK_MTX_DECLARE(haptic_write_lock,&haptic_group);
static struct x9_haptic_status hs={.version=X9_HAPTIC_VERSION,
    .input_rate=X9_HAPTIC_INPUT_RATE,.rtp_rate=X9_HAPTIC_RTP_RATE,.f0_millihz=173000};
static int8_t ring[X9_HAPTIC_RING];
static uint32_t head,count,scene_frame,scene_intensity,scene_sharpness;
static uint64_t sequence,on_at,cool_until,last_signal_ns;
static bool writer,draining,scene_active,quiet_stop;
static struct x9_haptic_converter converter;
static struct x9_haptic_pattern pattern;
/* Kept off the kernel stack; write_lock protects copyin/conversion. */
static struct x9_haptic_packet packet;
static uint64_t now_ns(void)
{uint64_t t,ns;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);return ns;}
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
static int stop_locked(void)
{
    hs.flags&=~X9_HAPTIC_RUNNING;draining=scene_active=quiet_stop=false;
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
    if(hs.mode!=2 || !(hs.flags&X9_HAPTIC_READY))return ENXIO;
    if(now_ns()<cool_until)return EAGAIN;
    if((hs.flags&X9_HAPTIC_RUNNING) || hb.active)return EBUSY;
    hs.flags&=~X9_HAPTIC_THROTTLED;hs.flags|=X9_HAPTIC_RUNNING;
    ++hs.generation;head=count=0;sequence=0;draining=scene_active=false;
    x9_haptic_converter_init(&converter,hs.f0_millihz,s->conversion,s->intensity,s->sharpness);
    hs.last_write_ns=now_ns();s->generation=hs.generation;return 0;
}
static int haptic_open(dev_t d,int f,__unused int t,struct proc *p)
{
    if(minor(d)==0)return f&FWRITE?EACCES:0;
    if(minor(d)!=1 || !(f&FWRITE) || !allowed(p))return EPERM;
    lck_mtx_lock(&haptic_lock);int e=hs.mode!=2?EPERM:writer?EBUSY:0;
    if(!e)writer=true;lck_mtx_unlock(&haptic_lock);return e;
}
static int haptic_close(dev_t d,__unused int f,__unused int t,__unused struct proc *p)
{
    if(minor(d)!=1)return 0;
    lck_mtx_lock(&haptic_write_lock);lck_mtx_lock(&haptic_lock);
    int e=stop_locked();writer=false;
    bzero(&packet,sizeof(packet));lck_mtx_unlock(&haptic_lock);lck_mtx_unlock(&haptic_write_lock);return e;
}
static int haptic_read(dev_t d,struct uio *u,__unused int flags)
{
    if(minor(d)!=0 || uio_offset(u) || uio_resid(u)<sizeof(hs))return EINVAL;
    struct x9_haptic_status s;
    lck_mtx_lock(&haptic_lock);s=hs;s.queued_frames=count;lck_mtx_unlock(&haptic_lock);
    return uiomove((char *)&s,sizeof(s),u);
}
static int haptic_write(dev_t d,struct uio *u,__unused int flags)
{
    if(minor(d)!=1 || !uio_isuserspace(u) || uio_iovcnt(u)!=1 ||
       uio_resid(u)<24 || uio_resid(u)>sizeof(packet))return EINVAL;
    if(!lck_mtx_try_lock(&haptic_write_lock))return EAGAIN;
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
        else if(scene_active || draining)e=EBUSY;
        else if(count+output>X9_HAPTIC_RING)e=EAGAIN;
        if(!e) {
            bool cooling=now_ns()<cool_until;
            if(!cooling)hs.flags&=~X9_HAPTIC_THROTTLED;
            for(unsigned i=0;i<packet.frames;i++) {
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
    if(cmd==X9_HAPTIC_GET_STATUS_NOWAIT) {
        if(!lck_mtx_try_lock(&haptic_lock))return EAGAIN;
        hs.queued_frames=count;memcpy(data,&hs,sizeof(hs));lck_mtx_unlock(&haptic_lock);return 0;
    }
    if(cmd!=X9_HAPTIC_GET_STATUS && (minor(d)!=1 || !allowed(p)))return EPERM;
    /* Control and PCM copyin use one order. STOP cannot retire a session
     * midway through conversion; no worker ever takes write_lock. */
    lck_mtx_lock(&haptic_write_lock);lck_mtx_lock(&haptic_lock);int e=0;
    if(cmd==X9_HAPTIC_GET_STATUS){hs.queued_frames=count;memcpy(data,&hs,sizeof(hs));}
    else if(!writer)e=EPERM;
    else if(cmd==X9_HAPTIC_START)e=start_locked((void *)data);
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
        struct x9_haptic_pattern next;
        if(s->version!=1 || s->reserved || s->intensity>1000 || s->sharpness>1000 ||
            x9_haptic_scene_pattern(s->scene,s->duration_ms,&next))e=EINVAL;
        else {
            struct x9_haptic_start start={.version=1,.conversion=1,.intensity=s->intensity,.sharpness=s->sharpness};
            e=start_locked(&start);
            if(!e){pattern=next;scene_active=true;scene_frame=0;
                scene_intensity=s->intensity;scene_sharpness=s->sharpness;wakeup(&count);}
        }
    } else e=ENOTTY;
    lck_mtx_unlock(&haptic_lock);lck_mtx_unlock(&haptic_write_lock);return e;
}
static void haptic_worker(__unused void *arg,__unused wait_result_t wait)
{
    for(;;) {
        lck_mtx_lock(&haptic_lock);
        uint64_t now=now_ns();
        if(hs.flags&X9_HAPTIC_RUNNING) {
            /* An explicit client must not hold the chip running forever.
             * Changes of amplitude/sequence do not renew continuous-on time. */
            if(on_at && now-on_at>=1000000000ULL) {
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
                if(scene_active) {
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
                if(!scene_active && !draining && !signal &&
                   (!hb.active || (last_signal_ns && now-last_signal_ns>=20000000))) {
                    head=count=0;quiet_stop=true;
                }
                if(!hb.active) {
                    if(count>=120 || (draining && count)) {
                        e=hgo();if(!e){on_at=now_ns();quiet_stop=false;}
                    }
                }
                if(!e && hb.active) {
                    unsigned budget=120;
                    while(count && budget) {
                        unsigned n=MIN(count,MIN(15u,MIN(budget,X9_HAPTIC_RING-head)));
                        e=hfeed(ring+head,n);
                        if(e==EAGAIN){e=0;break;}if(e)break;
                        for(unsigned i=0;i<n;i++)if(ring[head+i]){last_signal_ns=now_ns();break;}
                        head=(head+n)%X9_HAPTIC_RING;count-=n;budget-=n;hs.submitted_frames+=n;
                    }
                    if(!e && !count) {
                        uint8_t fifo;e=hget(4,&fifo);if(!e)e=hstate();
                        if(!e && (fifo&1) && !hs.hw_state) {
                            /* Real FIFO empty + native standby, not elapsed
                             * wall time, proves this hardware run ended. */
                            hb.active=false;on_at=0;
                            if(!draining && !quiet_stop)++hs.underruns;
                        }
                    }
                }
                if(e)fail_locked(e);
                else if(draining && !count && !hb.active) {
                    ++hs.completed_streams;int stopped=stop_locked();if(stopped)fail_locked(stopped);
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
    int mode=sweet_x9_arg("x9_haptic",0);if(mode<0 || mode>2)mode=0;
    hs.mode=(unsigned)mode;if(!mode)return;
    int f0=sweet_x9_arg("x9_haptic_f0",1730);
    if(f0<1600 || f0>1900){hs.last_errno=EINVAL;hs.flags=X9_HAPTIC_FAULT;}
    else hs.f0_millihz=(unsigned)f0*100;
    int major=cdevsw_add(-1,&haptic_cdev);if(major<0)return;
    devfs_make_node(makedev(major,0),DEVFS_CHAR,0,0,0444,"x9-haptic-status");
    if(mode==2) {
        if(!(hs.flags&X9_HAPTIC_FAULT)) {
            int e=hboard_init();if(e){hs.last_errno=e;hs.flags|=X9_HAPTIC_FAULT;}
            else hs.flags=X9_HAPTIC_READY;
        }
        thread_t thread=THREAD_NULL;
        kern_return_t kr=kernel_thread_start(haptic_worker,NULL,&thread);
        if(kr==KERN_SUCCESS)thread_deallocate(thread);
        else {hs.last_errno=ENOMEM;hs.flags=X9_HAPTIC_FAULT;(void)hoff();}
        devfs_make_node(makedev(major,1),DEVFS_CHAR,501,501,0600,"x9-haptic");
    }
    printf("X9 HAPTIC: mode=%u flags=%x errno=%u devfs ready; playback validation separate\n",
        hs.mode,hs.flags,hs.last_errno);
}
