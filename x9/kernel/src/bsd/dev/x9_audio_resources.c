/* Shared AP AUDIO/AO/INFRA transition lease. A private driver mutex cannot
 * protect a borrowed parent against a different driver's power_off. No
 * hardware is configured here and claiming this lease never sets Ready. */
#include <sys/errno.h>
#include <kern/locks.h>
#include "x9_audio_resources.h"
extern int sweet_early_is_x9(void);
LCK_GRP_DECLARE(resource_group,"x9-audio-resources");
LCK_MTX_DECLARE(resource_lock,&resource_group);
static X9AudioResourceStatus state;
/* audio46 (log 304): iOS recording/Voice Memos run output and input at the
 * same time, as Android does on one AFE. The mic therefore has its own slot
 * beside the speaker; it is exclusive only against the ADSP owner and a
 * second mic. The speaker AFE base is held first and the mic borrows it. */
static struct { uint64_t ticket; uint32_t held_error; } mic;
_Static_assert(sizeof(X9AudioResourceStatus)==64,"resource status v1 size");
int x9_audio_resources_claim(uint32_t owner,uint64_t *ticket)
{
    if(!ticket)return EINVAL;
    *ticket=0;
    if(!sweet_early_is_x9())return ENXIO;
    if(owner<X9AudioResourceADSP || owner>X9AudioResourceSpeaker)return EINVAL;
    lck_mtx_lock(&resource_lock);
    int e=0;
    if(owner==X9AudioResourceMic) {
        if(mic.ticket || state.owner==X9AudioResourceADSP)e=EBUSY;
        else if(state.claims==UINT64_MAX)e=EOVERFLOW;
        else {mic.ticket=*ticket=++state.claims;mic.held_error=0;}
        if(e==EBUSY && state.refusals!=UINT32_MAX)state.refusals++;
        lck_mtx_unlock(&resource_lock);return e;
    }
    if(state.owner || (owner==X9AudioResourceADSP && mic.ticket)) {
        if(state.refusals!=UINT32_MAX)state.refusals++;
        e=EBUSY;
    } else if(state.claims==UINT64_MAX)e=EOVERFLOW; /* never reuse a ticket */
    else {
        state.owner=state.last_owner=owner;state.held_error=0;
        state.ticket=++state.claims;*ticket=state.ticket;
    }
    lck_mtx_unlock(&resource_lock);return e;
}
int x9_audio_resources_release(uint32_t owner,uint64_t ticket)
{
    lck_mtx_lock(&resource_lock);
    int e=0;
    if(owner==X9AudioResourceMic) {
        if(!ticket || ticket!=mic.ticket) {
            if(state.rejected_releases!=UINT64_MAX)state.rejected_releases++;
            e=EPERM;
        } else {mic.ticket=0;mic.held_error=0;state.releases++;}
        lck_mtx_unlock(&resource_lock);return e;
    }
    if(!ticket || !state.owner || owner!=state.owner || ticket!=state.ticket) {
        if(state.rejected_releases!=UINT64_MAX)state.rejected_releases++;
        state.release_error=EPERM;e=EPERM; /* stale/foreign release changes no ownership */
    } else {
        state.owner=X9AudioResourceNone;state.ticket=0;state.held_error=0;
        state.release_error=0;state.releases++;
    }
    lck_mtx_unlock(&resource_lock);return e;
}
void x9_audio_resources_fault(uint32_t owner,uint64_t ticket,int error)
{
    if(!error)return;
    lck_mtx_lock(&resource_lock);
    if(owner==X9AudioResourceMic) {
        if(ticket && ticket==mic.ticket && !mic.held_error){mic.held_error=(uint32_t)error;state.last_error=(uint32_t)error;}
    } else if(ticket && owner==state.owner && ticket==state.ticket) {
        if(!state.held_error)state.held_error=state.last_error=(uint32_t)error;
    }
    lck_mtx_unlock(&resource_lock);
}
static enum {AfeInitNone,AfeInitIssuing,AfeInitDone,AfeInitFailed} afe_init;
int x9_audio_afe_init_begin(void)
{
    lck_mtx_lock(&resource_lock);
    int e=afe_init==AfeInitDone?EALREADY:afe_init==AfeInitFailed?EIO:
          afe_init==AfeInitIssuing?EBUSY:0;
    if(!e)afe_init=AfeInitIssuing;
    lck_mtx_unlock(&resource_lock);return e;
}
void x9_audio_afe_init_end(int error)
{
    lck_mtx_lock(&resource_lock);
    if(afe_init==AfeInitIssuing)afe_init=error?AfeInitFailed:AfeInitDone;
    lck_mtx_unlock(&resource_lock);
}
int x9_audio_resources_status(X9AudioResourceStatus *s)
{
    if(!s)return EINVAL;
    lck_mtx_lock(&resource_lock);*s=state;s->version=X9_AUDIO_RESOURCE_STATUS_VERSION;
    lck_mtx_unlock(&resource_lock);return 0; /* no MMIO or provider callback */
}
