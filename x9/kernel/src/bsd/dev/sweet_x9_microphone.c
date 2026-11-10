/* Bounded PCM capture for MT6993. No hardware access until a real backend is
 * registered and a permitted client explicitly starts recording. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <sys/file_internal.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <miscfs/devfs/devfs.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <pexpert/device_tree.h>
#include "x9_microphone.h"
#include "x9_mic_mt6993.h"

extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *, int);
LCK_GRP_DECLARE(mic_group, "x9-microphone");
LCK_MTX_DECLARE(mic_lock, &mic_group);
LCK_MTX_DECLARE(mic_control_lock, &mic_group);
LCK_MTX_DECLARE(mic_read_lock, &mic_group);
static struct x9_mic_status st = {.version=1, .rate=X9_MIC_RATE,
    .channels=1, .sample_bits=16, .source=X9_MIC_SOURCE_INTERNAL};
static struct x9_mic_backend hw;
static int16_t ring[X9_MIC_RING_FRAMES];
static uint32_t head, count;
static bool opened, hardware_active;

static bool permitted(struct proc *p)
{
    uid_t uid=kauth_cred_getuid(kauth_cred_get());
    char name[32]={0};
    if(!uid)return true;
    proc_name(proc_pid(p),name,sizeof(name));
    return uid==501 && !strcmp(name,"mediaserverd");
}
/* control + PCM locks held; no backend may wait for a worker with PCM locked. */
static int stop_locked(void)
{
    bool active=hardware_active;
    st.flags&=~X9_MIC_RUNNING;
    ++st.generation; head=count=0;
    wakeup(&count);
    int e=0;
    if(active) {
        lck_mtx_unlock(&mic_lock);
        e=hw.stop();
        lck_mtx_lock(&mic_lock);
        if(!e)hardware_active=false;
        else {st.flags|=X9_MIC_FAULT;st.flags&=~X9_MIC_READY;st.last_errno=e;}
    }
    return e;
}
int x9_mic_register_backend(const struct x9_mic_backend *p)
{
    if(!p || p->version!=1 || !p->start || !p->stop)return EINVAL;
    lck_mtx_lock(&mic_control_lock);lck_mtx_lock(&mic_lock);
    int e=st.mode!=2?EPERM:hw.version?EBUSY:0;
    if(!e) {hw=*p;st.supported_sources=X9_MIC_SOURCE_INTERNAL;st.flags=X9_MIC_READY;}
    lck_mtx_unlock(&mic_lock);lck_mtx_unlock(&mic_control_lock);return e;
}
int x9_mic_deliver(uint64_t generation,const int16_t *pcm,uint32_t frames,uint64_t time)
{
    if(!pcm || !frames || frames>X9_MIC_RING_FRAMES || !time)return EINVAL;
    lck_mtx_lock(&mic_lock);int e=0;
    if(!(st.flags&X9_MIC_RUNNING) || generation!=st.generation)e=ESTALE;
    else if(time<=st.last_host_time || time>mach_absolute_time())e=EINVAL;
    else {
        uint32_t lost=count+frames>X9_MIC_RING_FRAMES?count+frames-X9_MIC_RING_FRAMES:0;
        head=(head+lost)%X9_MIC_RING_FRAMES;count-=lost;st.dropped_frames+=lost;
        uint32_t tail=(head+count)%X9_MIC_RING_FRAMES;
        uint32_t n=MIN(frames,X9_MIC_RING_FRAMES-tail);
        memcpy(ring+tail,pcm,n*2);memcpy(ring,pcm+n,(frames-n)*2);
        count+=frames;st.captured_frames+=frames;st.last_host_time=time;
        wakeup(&count);
    }
    lck_mtx_unlock(&mic_lock);return e;
}
void x9_mic_backend_fault(uint64_t generation,int error)
{
    lck_mtx_lock(&mic_lock);
    if(generation==st.generation) {
        st.flags=X9_MIC_FAULT;st.last_errno=error?error:EIO;
        ++st.generation;head=count=0;wakeup(&count);
    }
    lck_mtx_unlock(&mic_lock);
}
void x9_mic_backend_drop(uint64_t generation,uint32_t frames)
{
    lck_mtx_lock(&mic_lock);
    if(generation==st.generation)st.dropped_frames+=frames;
    lck_mtx_unlock(&mic_lock);
}
static int mic_open(dev_t d,int f,__unused int t,struct proc *p)
{
    if(f&FWRITE)return EACCES;
    if(minor(d)==0)return 0; /* separate status node: never starts capture */
    if(minor(d)!=1 || !permitted(p))return EPERM;
    lck_mtx_lock(&mic_control_lock);lck_mtx_lock(&mic_lock);
    int e=opened?EBUSY:0;if(!e)opened=true;
    lck_mtx_unlock(&mic_lock);lck_mtx_unlock(&mic_control_lock);return e;
}
static int mic_close(dev_t d,__unused int f,__unused int t,__unused struct proc *p)
{
    if(minor(d)!=1)return 0;
    lck_mtx_lock(&mic_control_lock);lck_mtx_lock(&mic_lock);
    int e=stop_locked();opened=false;
    /* No retained recording from the previous session. Static storage avoids
     * freeing memory while a canceled read or DMA completion is returning. */
    bzero(ring,sizeof(ring));
    lck_mtx_unlock(&mic_lock);lck_mtx_unlock(&mic_control_lock);return e;
}
static int mic_ioctl(dev_t d,u_long cmd,caddr_t data,__unused int f,struct proc *p)
{
    if(cmd==X9_MIC_GET_HARDWARE) {
        x9_mic_mt6993_get_hardware((void *)data);return 0;
    }
    if(cmd==X9_MIC_GET_BOARD) {
        x9_mic_mt6993_get_board((void *)data);return 0;
    }
    if(cmd==X9_MIC_GET_AFE) {
        x9_mic_mt6993_get_afe((void *)data);return 0;
    }
    if(cmd==X9_MIC_GET_ANALOG) {
        x9_mic_mt6993_get_analog((void *)data);return 0;
    }
    if(cmd==X9_MIC_GET_DIGITAL) {
        x9_mic_mt6993_get_digital((void *)data);return 0;
    }
    if(cmd==X9_MIC_GET_DMA) {
        x9_mic_mt6993_get_dma((void *)data);return 0;
    }
    if(cmd==X9_MIC_POWER_PROBE || cmd==X9_MIC_CALIBRATE || cmd==X9_MIC_ANALOG_L || cmd==X9_MIC_DIGITAL_L) {
        if(minor(d)!=0 || kauth_cred_getuid(kauth_cred_get())!=0)return EPERM;
        /* mode1/status remain strictly passive. No implicit capture backend. */
        /* mode is immutable after devfs publication. The AFE lock serializes
         * probe against registration/capture; don't invert register's
         * AFE -> mic_control lock order here. */
        if(st.mode!=2)return EPERM;
        if(cmd==X9_MIC_DIGITAL_L)return x9_mic_mt6993_digital_l();
        if(cmd==X9_MIC_ANALOG_L)return x9_mic_mt6993_analog_l();
        return cmd==X9_MIC_CALIBRATE?x9_mic_mt6993_calibrate():x9_mic_mt6993_power_probe();
    }
    if(cmd==X9_MIC_GET_STATUS) {
        lck_mtx_lock(&mic_lock);st.queued_frames=count;
        memcpy(data,&st,sizeof(st));lck_mtx_unlock(&mic_lock);return 0;
    }
    if(cmd!=X9_MIC_START && cmd!=X9_MIC_STOP)return ENOTTY;
    if(minor(d)!=1 || !permitted(p))return EPERM;
    lck_mtx_lock(&mic_control_lock);lck_mtx_lock(&mic_lock);int e=0;
    if(cmd==X9_MIC_STOP)e=stop_locked();
    else {
        const struct x9_mic_config *c=(const void *)data;
        if(c->version!=1 || c->rate!=X9_MIC_RATE || c->channels!=1 || c->sample_bits!=16 ||
           c->source!=X9_MIC_SOURCE_INTERNAL || c->reserved[0] || c->reserved[1] || c->reserved[2])e=EINVAL;
        else if(st.mode!=2 || !(st.flags&X9_MIC_READY) || (st.flags&X9_MIC_FAULT))e=ENXIO;
        else if(st.flags&X9_MIC_RUNNING)e=EBUSY;
        else {
            head=count=0;++st.generation;st.last_host_time=0;
            st.captured_frames=st.read_frames=st.dropped_frames=0;st.last_errno=0;
            uint64_t generation=st.generation;
            hardware_active=true;
            lck_mtx_unlock(&mic_lock);e=hw.start(generation);lck_mtx_lock(&mic_lock);
            if(e) {
                st.flags|=X9_MIC_FAULT;st.flags&=~X9_MIC_READY;st.last_errno=e;
                (void)stop_locked();
            } else if(generation!=st.generation || (st.flags&X9_MIC_FAULT)) {
                e=EIO;(void)stop_locked();
            } else st.flags|=X9_MIC_RUNNING;
        }
    }
    lck_mtx_unlock(&mic_lock);lck_mtx_unlock(&mic_control_lock);return e;
}
static int mic_read(dev_t d,struct uio *uio,int flags)
{
    if(minor(d)!=1)return ENOTSUP;
    if(uio_resid(uio)<0 || uio_resid(uio)%2)return EINVAL;
    if(!uio_resid(uio))return 0;
    bool nonblock=flags&IO_NDELAY;
    if(nonblock) {if(!lck_mtx_try_lock(&mic_read_lock))return EAGAIN;}
    else lck_mtx_lock(&mic_read_lock);
    if(nonblock && !lck_mtx_try_lock(&mic_lock)) {
        lck_mtx_unlock(&mic_read_lock);return EAGAIN;
    }
    if(!nonblock)lck_mtx_lock(&mic_lock);
    int e=0;uint64_t generation=st.generation;
    if(!(st.flags&X9_MIC_RUNNING))e=ENXIO;
    else if(!count && !nonblock) {
        struct timespec timeout={0,100000000};
        e=msleep(&count,&mic_lock,PCATCH,"x9-mic",&timeout);
        if(e==EWOULDBLOCK)e=EAGAIN;
    }
    if(!e && (generation!=st.generation || !(st.flags&X9_MIC_RUNNING)))e=ECANCELED;
    if(!e && !count)e=EAGAIN;
    int16_t pcm[512];
    uint32_t n=e?0:(uint32_t)MIN(MIN((uint64_t)uio_resid(uio)/2,count),512u);
    for(uint32_t i=0;i<n;i++)pcm[i]=ring[(head+i)%X9_MIC_RING_FRAMES];
    head=(head+n)%X9_MIC_RING_FRAMES;count-=n;
    lck_mtx_unlock(&mic_lock);
    if(n) {
        user_ssize_t before=uio_resid(uio);e=uiomove((char *)pcm,n*2,uio);
        lck_mtx_lock(&mic_lock);
        if(generation==st.generation) {
            uint32_t transferred=(uint32_t)(before-uio_resid(uio))/2;
            st.read_frames+=transferred;st.dropped_frames+=n-transferred;
        }
        lck_mtx_unlock(&mic_lock);
    }
    bzero(pcm,sizeof(pcm));lck_mtx_unlock(&mic_read_lock);return e;
}
static const struct cdevsw mic_cdev={.d_open=mic_open,.d_close=mic_close,
    .d_read=mic_read,.d_write=eno_rdwrt,.d_ioctl=mic_ioctl,.d_stop=eno_stop,
    .d_reset=eno_reset,.d_select=eno_select,.d_mmap=eno_mmap,.d_strategy=eno_strat};
void sweet_x9_microphone_init(void)
{
    if(!sweet_early_is_x9())return;
    /* ADT is a mode-only fallback, not a readiness/ownership assertion.
     * Explicit x9_mic=0 also overrides it. Reject malformed properties. */
    int fallback=0;DTEntry chosen;const void *p=NULL;unsigned size=0;
    if(SecureDTLookupEntry(NULL,"/chosen",&chosen)==kSuccess &&
       SecureDTGetProperty(chosen,"x9-microphone-mode",&p,&size)==kSuccess && size==4) {
        uint32_t value=0;memcpy(&value,p,sizeof(value));
        if(value<=2)fallback=(int)value;
    }
    int mode=sweet_x9_arg("x9_mic",fallback);if(mode!=1 && mode!=2)return;
    if(st.mode)return;st.mode=mode;
    int major=cdevsw_add(-1,&mic_cdev);
    if(major<0){st.last_errno=ENXIO;return;}
    if(!devfs_make_node(makedev(major,0),DEVFS_CHAR,501,501,0644,"x9-mic-status") ||
       !devfs_make_node(makedev(major,1),DEVFS_CHAR,501,501,0600,"x9-mic"))st.last_errno=ENOMEM;
    printf("X9 MIC: mode=%d PCM=48000/mono/S16; backend=%u; no synthetic samples\n",mode,hw.version);
}
