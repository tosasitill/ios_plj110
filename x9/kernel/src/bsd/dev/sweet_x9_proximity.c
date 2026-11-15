/* OPPO sensor/2.0: front PROXIMITY=8, share_mem_info=40, data=36.
 * transceiver.c preserves value[0..2], unlike LIGHT's calibrated lux.
 * Android binary PS convention: oneshot 0=near, 1=far; keep steps separate.
 * Do not apply the public 24081 TCS3760 register recipe: PLJ110 live DT
 * names tcs3720 and uses different calibration/power configuration.
 * No AP I2C, MMIO or SCP reset here: one shared SCP transport owns power. */
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include "../../iokit/Kernel/arm/X9Proximity.h"
static LCK_GRP_DECLARE(prox_group, "x9_proximity");
static LCK_MTX_DECLARE(prox_lock, &prox_group);
static struct x9_prox_sample latest;
static struct x9_prox_status state;
static struct x9_prox_transport transport;
static int active, valid;
static uint64_t last_scp_time, request_time;
static uint32_t pending_enabled, pending_interval;
#define PROX_CONTROL_TIMEOUT_NS 1000000000ULL
static uint32_t prox_le32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t prox_le64(const uint8_t *p)
{ return prox_le32(p)|(uint64_t)prox_le32(p+4)<<32; }
static void expire_locked(void)
{
 if (!state.pending || state.error) return;
 uint64_t ns=0; absolutetime_to_nanoseconds(mach_absolute_time()-request_time,&ns);
 if (ns>PROX_CONTROL_TIMEOUT_NS) {
  state.error=ETIMEDOUT; state.pending=state.enabled=valid=0;
  /* Remote state uncertain; no retry and no false "hardware off". */
 }
}
static int configure_locked(const void *info, unsigned length)
{
 const uint8_t *p=info;
 if (!p || length!=X9_PROX_INFO_BYTES) return EINVAL;
 if (p[0]!=X9_PROX_MTK_TYPE) return ENOTSUP;
 if (!prox_le32(p+4)) return EBADMSG;
 state.configured=1; valid=0; last_scp_time=0;
 return 0;
}
static int receive_locked(const void *record, unsigned length, int diagnostic)
{
 const uint8_t *p=record;
 if (!p || length!=X9_PROX_DATA_BYTES) return EINVAL;
 if (p[0]!=X9_PROX_MTK_TYPE || p[1]!=0) return ENOTSUP;
 int32_t oneshot=(int32_t)prox_le32(p+12);
 uint64_t t=prox_le64(p+4);
 /* Ignore RAW/FLUSH/BIAS/CALI, rear proximity=105, and malformed PS.
  * No analog threshold synthesized from steps or ALS intensity. */
 if (oneshot<0 || oneshot>1 || p[2]>3 || !t || t>>63) return ERANGE;
 if (!diagnostic && t<=last_scp_time) return EALREADY;
 latest.sequence++; latest.host_time=mach_absolute_time();
 latest.scp_time_ns=diagnostic?0:t; latest.oneshot=oneshot;
 latest.steps=(int32_t)prox_le32(p+16); latest.auxiliary=(int32_t)prox_le32(p+20);
 latest.near=oneshot==0; latest.diagnostic=diagnostic;
 last_scp_time=t; valid=1; state.accepted++;
 return 0;
}
int x9_prox_init(int mode)
{
 if (mode<1 || mode>3) return EINVAL;
 lck_mtx_lock(&prox_lock);
 int error=active || state.pending?EBUSY:0;
 if (!error) {
  active=1; valid=0; last_scp_time=0;
  /* Keep uncertain native control across software restarts. */
  int prior=mode==3?state.error:0; bzero(&state,sizeof(state)); state.error=prior;
  state.mode=mode; state.interval_us=200000;
 }
 lck_mtx_unlock(&prox_lock); return error;
}
void x9_prox_shutdown(void)
{
 /* Request sensor DISABLE only, never shut off another SCP user's power. */
 (void)x9_prox_set_enabled(0,200000);
 lck_mtx_lock(&prox_lock);
 active=valid=0; last_scp_time=0;
 lck_mtx_unlock(&prox_lock);
}
int x9_prox_copy_sample(struct x9_prox_sample *out)
{
 if (!out) return EINVAL;
 lck_mtx_lock(&prox_lock); expire_locked();
 /* PS is on-change: a valid near/far level does not expire after 5 s.
  * Remote STOP/reset/disable/fault invalidates it, not absence of changes. */
 int error=!active?ENXIO:state.error?state.error:!valid?ENOENT:0;
 if (!error) *out=latest;
 lck_mtx_unlock(&prox_lock); return error;
}
void x9_prox_copy_status(struct x9_prox_status *out)
{
 if (!out) return;
 lck_mtx_lock(&prox_lock); expire_locked(); *out=state;
 if (!active) out->error=ENXIO;
 lck_mtx_unlock(&prox_lock);
}
int x9_prox_set_enabled(uint32_t enabled, uint32_t us)
{
 if (enabled>1 || us<20000 || us>1000000) return EINVAL;
 int (*request)(uint32_t,uint32_t)=NULL;
 lck_mtx_lock(&prox_lock); expire_locked();
 int error=!active?ENXIO:state.error?state.error:0;
 if (!error && state.mode==3 && (!transport.request || !state.ready || !state.configured)) {
  /* Keep iOS's desired mode until LIST/READY, without faking an ACK. */
  state.desired=enabled; state.interval_us=us; error=EAGAIN;
 } else if (!error && state.pending) {
  /* Coalesce iOS mode changes while the real ACK is in flight. The
   * callback compares with pending_enabled, then submits the latest mode. */
  state.desired=enabled; state.interval_us=us; valid=0;
 } else if (!error) {
  if (state.enabled==enabled && state.interval_us==us) {
   state.desired=enabled;
  } else {
   state.desired=enabled; state.interval_us=us;
   if (state.mode==3) {
    request=transport.request; state.pending=1; request_time=mach_absolute_time();
    pending_enabled=enabled; pending_interval=us;
    valid=0;
   } else { state.enabled=enabled; if (!enabled) valid=0; }
  }
 }
 lck_mtx_unlock(&prox_lock);
 if (request) {
  error=request(enabled,us);
  /* A submission refusal is definitive: backend promised no send on error. */
  if (error) {
   lck_mtx_lock(&prox_lock); state.pending=0; state.error=error; valid=0;
   lck_mtx_unlock(&prox_lock);
  }
 }
 return error;
}
int x9_prox_bind_transport(const struct x9_prox_transport *ops)
{
 if (!ops || !ops->request) return EINVAL;
 lck_mtx_lock(&prox_lock);
 int error=transport.request && transport.request!=ops->request?EBUSY:0;
 if (!error) transport=*ops;
 lck_mtx_unlock(&prox_lock); return error;
}
int x9_prox_native_configure(const void *info, unsigned length)
{
 uint32_t enabled=0,us=0;
 lck_mtx_lock(&prox_lock);
 int error=!active?ENXIO:state.mode!=3?EBUSY:state.error?state.error:configure_locked(info,length);
 if (!error) { state.ready=1; enabled=state.desired; us=state.interval_us; }
 lck_mtx_unlock(&prox_lock);
 /* Default off. iOS DetectionMode is the activation input. */
 if (!error && enabled) error=x9_prox_set_enabled(enabled,us);
 return error;
}
void x9_prox_native_control(uint32_t enabled, int error)
{
 int (*request)(uint32_t,uint32_t)=NULL;
 uint32_t next=0,us=0;
 lck_mtx_lock(&prox_lock);
 /* A service stop still needs to complete its already-issued DISABLE. */
 if (state.mode==3 && state.pending) {
  if (!error && enabled!=pending_enabled) error=EPROTO;
  state.pending=0; state.error=error; state.enabled=error?0:enabled;
  if (error || !enabled) valid=0;
  if (!error && (state.desired!=enabled ||
      (enabled && state.interval_us!=pending_interval))) {
   next=state.desired; us=state.interval_us; request=transport.request;
   pending_enabled=next; pending_interval=us;
   state.pending=1; request_time=mach_absolute_time(); valid=0;
  }
 }
 lck_mtx_unlock(&prox_lock);
 printf("X9 PROX: native %s ACK errno=%d (not a DATA event)\n",enabled?"ENABLE":"DISABLE",error);
 if (request) {
  error=request(next,us);
  if (error) {
   lck_mtx_lock(&prox_lock); state.pending=0; state.error=error; valid=0;
   lck_mtx_unlock(&prox_lock);
  }
 }
}
void x9_prox_native_disconnect(int error)
{
 lck_mtx_lock(&prox_lock);
 valid=state.ready=state.configured=state.enabled=state.pending=0;
 state.error=error; last_scp_time=0;
 lck_mtx_unlock(&prox_lock);
}
int x9_prox_native_data(const void *record, unsigned length)
{
 lck_mtx_lock(&prox_lock); expire_locked();
 int error=!active?ENXIO:state.mode!=3?EBUSY:state.error?state.error:
     !state.ready || !state.configured || !state.enabled || state.pending?EAGAIN:
     receive_locked(record,length,0);
 if (error) state.rejected++;
 lck_mtx_unlock(&prox_lock); return error;
}
int x9_prox_configure_mtk(const void *info, unsigned length)
{
 lck_mtx_lock(&prox_lock);
 int error=!active?ENXIO:state.mode!=1?EBUSY:configure_locked(info,length);
 lck_mtx_unlock(&prox_lock); return error;
}
int x9_prox_receive_mtk(const void *record, unsigned length)
{
 lck_mtx_lock(&prox_lock);
 int error=!active?ENXIO:state.mode!=1?EBUSY:!state.configured || !state.enabled?EAGAIN:
     receive_locked(record,length,0);
 if (error) state.rejected++;
 lck_mtx_unlock(&prox_lock); return error;
}
/* Explicit test mode only, root-write sysctl. 1=near, 0=far, -1=invalid.
 * Never conflated with Android oneshot (which has opposite polarity). */
static int sysctl_prox_test SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1,arg2)
 struct x9_prox_sample sample;
 int value=x9_prox_copy_sample(&sample)==0?(int)sample.near:-1;
 int error=sysctl_handle_int(oidp,&value,0,req);
 if (error || !req->newptr) return error;
 lck_mtx_lock(&prox_lock);
 if (!active || state.mode!=2) error=EPERM;
 else if (value==-1) valid=0;
 else if (value<0 || value>1) error=ERANGE;
 else if (!state.enabled) error=EAGAIN;
 else {
  uint8_t record[X9_PROX_DATA_BYTES]={8,0,3,0,1}; record[12]=!value;
  error=receive_locked(record,sizeof(record),1);
 }
 lck_mtx_unlock(&prox_lock); return error;
}
SYSCTL_PROC(_debug,OID_AUTO,x9_prox_test_near,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED,
    0,0,sysctl_prox_test,"I","X9 proximity DIAGNOSTIC only: -1 invalid, 0 far, 1 near (x9_prox=2)");
