/* SCP motion admission. The ALS worker is the ONLY mailbox/power owner.
 * LIST gain is units per m/s^2; preserve raw evidence and convert to 16.16 g
 * with integer arithmetic (standard gravity 9.80665, no kernel FP).
 * No external/diagnostic injection API and no local sensor register writes. */
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include "../../iokit/Kernel/arm/X9Motion.h"
static LCK_GRP_DECLARE(motion_group, "x9_motion");
static LCK_MTX_DECLARE(motion_lock, &motion_group);
static struct x9_motion_sample motion_sample;
static struct x9_motion_status motion_state = {
 .version=X9_MOTION_ABI, .bytes=sizeof(struct x9_motion_status),
 .requested_us=20000, .applied_us=20000
};
static uint64_t motion_last_time;
static int motion_valid;
static uint32_t motion_le32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
int x9_motion_init(void)
{
 lck_mtx_lock(&motion_lock);
 int e=motion_state.active?EBUSY:motion_state.error;
 if (!e) {
  motion_state.active=motion_state.desired=1;
  motion_state.configured=motion_state.enabled=motion_valid=0;
  motion_last_time=0;
 }
 lck_mtx_unlock(&motion_lock); return e;
}
void x9_motion_shutdown(void)
{
 lck_mtx_lock(&motion_lock);
 motion_state.active=motion_state.desired=motion_valid=0;
 /* The shared worker still owes a DISABLE ACK if the stream was enabled. */
 lck_mtx_unlock(&motion_lock);
}
int x9_motion_set_interval(uint32_t us)
{
 if (us<10000 || us>1000000) return EINVAL;
 lck_mtx_lock(&motion_lock);
 int e=!motion_state.active?ENXIO:motion_state.error;
 if (!e) motion_state.requested_us=us;
 lck_mtx_unlock(&motion_lock); return e;
}
void x9_motion_copy_status(struct x9_motion_status *out)
{
 if (!out) return;
 lck_mtx_lock(&motion_lock); *out=motion_state;
 out->pending=out->configured && !out->error &&
     (out->desired!=out->enabled ||
      (out->desired && out->requested_us!=out->applied_us));
 lck_mtx_unlock(&motion_lock);
}
int x9_motion_copy_sample(struct x9_motion_sample *out)
{
 if (!out) return EINVAL;
 lck_mtx_lock(&motion_lock);
 int e=motion_state.error?motion_state.error:!motion_state.active?ENXIO:
     !motion_state.enabled || !motion_valid?ENOENT:0;
 if (!e) *out=motion_sample;
 lck_mtx_unlock(&motion_lock);
 if (!e) {
  uint64_t now=mach_absolute_time(),age;
  if (now<out->host_time) return ERANGE;
  absolutetime_to_nanoseconds(now-out->host_time,&age);
  if (age>X9_MOTION_MAX_AGE_NS) e=ETIMEDOUT;
 }
 return e;
}
int x9_motion_native_configure(const void *info,unsigned n)
{
 const uint8_t *p=info;
 if (!p || n!=X9_MOTION_INFO_BYTES) return EINVAL;
 if (p[0]!=X9_MOTION_MTK_TYPE || !motion_le32(p+4)) return EBADMSG;
 lck_mtx_lock(&motion_lock);
 int e=!motion_state.active?ENXIO:motion_state.error;
 if (!e) {
  motion_state.gain=motion_le32(p+4); motion_state.configured=1;
  motion_valid=0; motion_last_time=0;
 }
 lck_mtx_unlock(&motion_lock); return e;
}
void x9_motion_native_control(uint32_t enabled,uint32_t us,int e)
{
 lck_mtx_lock(&motion_lock);
 /* Never clear a previous transport fault with a late successful ACK. */
 if (!motion_state.error) motion_state.error=e;
 motion_state.enabled=motion_state.error?0:enabled;
 if (!motion_state.error) motion_state.applied_us=us;
 if (motion_state.error || !enabled || !motion_state.desired) motion_valid=0;
 lck_mtx_unlock(&motion_lock);
 printf("X9 MOTION: %s ACK interval=%u errno=%d (not DATA)\n",enabled?"ENABLE":"DISABLE",us,e);
}
void x9_motion_native_disconnect(int e)
{
 lck_mtx_lock(&motion_lock);
 motion_state.error=e?e:EPIPE;
 motion_state.configured=motion_state.enabled=motion_valid=0;
 lck_mtx_unlock(&motion_lock);
}
int x9_motion_native_data(const void *record,unsigned n)
{
 const uint8_t *p=record;
 if (!p || n!=X9_MOTION_DATA_BYTES) return EINVAL;
 lck_mtx_lock(&motion_lock);
 int e=motion_state.error?motion_state.error:
     !motion_state.active || !motion_state.configured ||
     !motion_state.enabled || !motion_state.desired?EAGAIN:0;
 uint64_t t=motion_le32(p+4)|(uint64_t)motion_le32(p+8)<<32;
 struct x9_motion_sample s={.version=X9_MOTION_ABI,.bytes=sizeof(s)};
 if (!e && (p[0]!=X9_MOTION_MTK_TYPE || p[1]!=0)) e=ENOTSUP;
 if (!e && (p[2]>3 || !t || t>>63)) e=ERANGE;
 if (!e && t<=motion_last_time) e=EALREADY;
 for (unsigned i=0;!e && i<3;i++) {
  s.raw[i]=(int32_t)motion_le32(p+12+4*i);
  /* Reduce 100000/980665 by 5 before multiplication: even INT32_MIN
   * remains inside int64_t. Rejecting +/-32g AFTER an overflowing product
   * would not protect admission from a malformed FIFO record. */
  int64_t v=-(int64_t)s.raw[i]*65536*20000;
  int64_t den=(int64_t)motion_state.gain*196133;
  v=v<0?-((-v+den/2)/den):(v+den/2)/den;
  /* Reject malformed values outside the accelerometer's +/-32 g bound. */
  if (v>32*65536 || v<-(32*65536)) e=ERANGE;
  else s.fixed_g[i]=(int32_t)v;
 }
 if (!e) {
  s.sequence=motion_sample.sequence+1; s.host_time=mach_absolute_time();s.scp_time=t;
  motion_sample=s;motion_last_time=t;motion_valid=1;motion_state.accepted++;
 } else motion_state.rejected++;
 lck_mtx_unlock(&motion_lock);return e;
}
static int sysctl_motion_sample SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1,arg2)
 struct x9_motion_sample s;int e=x9_motion_copy_sample(&s);
 return e?e:SYSCTL_OUT(req,&s,sizeof(s));
}
static int sysctl_motion_status SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1,arg2)
 struct x9_motion_status s;x9_motion_copy_status(&s);
 return SYSCTL_OUT(req,&s,sizeof(s));
}
SYSCTL_PROC(_kern,OID_AUTO,x9_motion_sample,CTLTYPE_OPAQUE|CTLFLAG_RD|CTLFLAG_LOCKED,
    0,0,sysctl_motion_sample,"S,x9_motion_sample","X9 real accelerometer ABI; stale DATA returns errno");
SYSCTL_PROC(_kern,OID_AUTO,x9_motion_status,CTLTYPE_OPAQUE|CTLFLAG_RD|CTLFLAG_LOCKED,
    0,0,sysctl_motion_status,"S,x9_motion_status","X9 SCP motion admission and matched controls");
static int sysctl_motion_note SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1,arg2)
 char note[192]={0};
 if (!req->newptr) return SYSCTL_OUT(req,note,1);
 uid_t uid=kauth_cred_getuid(kauth_cred_get());
 if (uid!=0 && uid!=501) return EPERM;
 int e=sysctl_handle_string(oidp,note,sizeof(note),req);
 if (e) return e;
 for (unsigned i=0;note[i];i++) if ((uint8_t)note[i]<32 || (uint8_t)note[i]>126) note[i]='?';
 static uint32_t count;
 if (__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)<256) printf("X9 MOTION USER: %s\n",note);
 return 0;
}
SYSCTL_PROC(_kern,OID_AUTO,x9_motion_note,CTLTYPE_STRING|CTLFLAG_RW|CTLFLAG_ANYBODY|CTLFLAG_LOCKED,
    0,0,sysctl_motion_note,"A","Bounded X9 orientation bridge notes (root/mobile)");
