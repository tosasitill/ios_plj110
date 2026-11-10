/* Included after x9_i2c6_native.inc in x9_tfa_plj110.c. Use that actual
 * controller/mutex/state, never a fabricated x9_tfa_bus.verified. */
#include "x9_pa_native.h"
_Static_assert(sizeof(X9AudioPANativeStatus)==64,"native PA cached v1");
static struct {
    X9AudioPANativeStatus status;
    uint64_t ticket,lease,epoch;
    uint32_t attempted_identity;
    bool bound;
} np;
static int np_fault(unsigned side,int e)
{
    if(!e)return 0;
    if(e==EAGAIN)e=EIO;
    if(!np.status.error)np.status.error=(uint32_t)e;
    if(side<2&&!np.status.side_error[side])np.status.side_error[side]=(uint32_t)e;
    x9_audio_resources_fault(X9AudioResourceSpeaker,np.ticket,e);return e;
}
static int np_parent(uint64_t ticket,uint64_t lease,bool create)
{
    int e=n_match(ticket,lease);if(e)return e;
    if(!ni.configured||ni.teardown||ni.pending||ni.uncertain)return ENXIO;
    e=n_idle(false);if(e)return n_fault(e);
    if(!np.bound||np.ticket!=ticket||np.lease!=lease) {
        if(!create)return ESTALE;
        /* No status/context replacement can turn a failed epoch into a new
         * transaction. Parent validation above proves this actual epoch. */
        if(np.bound&&(ni.policy.reset_epoch<=np.epoch||
                      ni.policy.generation<=np.status.generation))return ESTALE;
        e=n_online();if(e)return e;
        memset(&np.status,0,sizeof(np.status));np.status.version=X9_PA_NATIVE_STATUS_VERSION;
        np.status.generation=ni.policy.generation;np.ticket=ticket;np.lease=lease;
        np.epoch=ni.policy.reset_epoch;np.attempted_identity=0;np.bound=true;
    } else if(np.epoch!=ni.policy.reset_epoch||np.status.generation!=ni.policy.generation)return ESTALE;
    if(np.status.checks!=UINT64_MAX)np.status.checks++;
    return 0;
}
static int np_transfer(unsigned side,uint8_t reg,uint16_t *value,bool reading)
{
    int e=n_transfer((uint8_t)(0x34+side),reg,value,reading);
    /* Static transport entry, so preserve the same native controller fault
     * bookkeeping as its public read/write wrappers before proceeding. */
    return e?n_fault(e):0;
}
static int np_identity(unsigned side,bool initial)
{
    uint32_t bit=1u<<side;
    if(initial) {
        if(np.attempted_identity&bit)return np.status.revision_verified&bit?0:ENXIO;
        np.attempted_identity|=bit; /* BEFORE any potentially incomplete read */
    } else if(!(np.status.revision_verified&bit))return ENXIO;
    np.status.revision_verified&=~bit;np.status.muted&=~bit;
    uint16_t value=0;int e=np_transfer(side,3,&value,true);
    if(e)return e;
    if(!x9_pa_native_revision_supported(value)||
       (!initial&&value!=np.status.revision[side])) {
        np.status.revision[side]=value;
        np.status.revision_verified&=~bit;np.status.muted&=~bit;return ENXIO;
    }
    np.status.revision[side]=value;np.status.revision_verified|=bit;return 0;
}
static int np_read_mute(unsigned side,bool expected,uint16_t wanted)
{
    uint32_t bit=1u<<side;np.status.muted&=~bit;
    int e=np_identity(side,false);if(e)return e;
    uint16_t value=0;e=np_transfer(side,X9_PA_NATIVE_SYSCTRL,&value,true);if(e)return e;
    np.status.sysctrl[side]=value;
    if((value&(X9_PA_NATIVE_I2CR_MASK|X9_PA_NATIVE_AMPE_MASK))||(expected&&value!=wanted))return EIO;
    np.status.muted|=bit;return 0;
}
static int np_mute_side(unsigned side)
{
    uint32_t bit=1u<<side;np.status.muted&=~bit;
    /* Even a definite failed PA write is not automatically repeated here.
     * Read-only muted_validate may prove safe AMPE-off, but does not clear
     * this retained uncertainty or license another value write. */
    if(np.status.mute_uncertain&bit)return EBUSY;
    int e=np_identity(side,false);if(e)return e;
    uint16_t old=0;e=np_transfer(side,X9_PA_NATIVE_SYSCTRL,&old,true);if(e)return e;
    np.status.sysctrl[side]=old;
    if(old&X9_PA_NATIVE_I2CR_MASK)return EBUSY; /* never echo the auto-clear action */
    uint16_t wanted=(uint16_t)(old&~X9_PA_NATIVE_AMPE_MASK);
    if(old&X9_PA_NATIVE_AMPE_MASK) {
        if(np.status.mute_attempts[side]==UINT64_MAX)return EOVERFLOW;
        np.status.mute_attempts[side]++;np.status.mute_uncertain|=bit;
        e=np_transfer(side,X9_PA_NATIVE_SYSCTRL,&wanted,false);if(e)return e;
    }
    /* Whole-word readback, and fresh full identity again, not ACK alone. */
    e=np_read_mute(side,true,wanted);if(!e)np.status.mute_uncertain&=~bit;
    return e;
}
int x9_pa_native_mute(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&i2c6_lock);int e=np_parent(ticket,lease,true);
    if(!e) {
        int first=0;np.status.muted=0;
        for(unsigned i=0;i<2;i++) {
            int r=np_identity(i,true);if(r){np_fault(i,r);if(!first)first=r;}
        }
        /* A known side is still isolated if the other definite transaction
         * failed. n_transfer blocks following activity after unknown effects. */
        for(unsigned i=0;i<2;i++) {
            int r=np_mute_side(i);if(r){np_fault(i,r);if(!first)first=r;}
        }
        if(!first)for(unsigned i=0;i<2;i++) {
            int r=np_read_mute(i,true,np.status.sysctrl[i]);
            if(r){np_fault(i,r);if(!first)first=r;}
        }
        e=first;
    }
    if(e&&np.bound&&np.ticket==ticket&&np.lease==lease)np_fault(2,e);
    lck_mtx_unlock(&i2c6_lock);return e;
}
int x9_pa_native_muted_validate(uint64_t ticket,uint64_t lease)
{
    lck_mtx_lock(&i2c6_lock);int e=np_parent(ticket,lease,false);
    if(!e) {
        int first=0;
        for(unsigned i=0;i<2;i++) {
            int r=np_read_mute(i,true,np.status.sysctrl[i]);
            if(r){np_fault(i,r);if(!first)first=r;}
        }
        e=first;
    }
    if(e&&np.bound&&np.ticket==ticket&&np.lease==lease)np_fault(2,e);
    lck_mtx_unlock(&i2c6_lock);return e;
}
int x9_pa_native_status(X9AudioPANativeStatus *out)
{
    if(!out)return EINVAL;lck_mtx_lock(&i2c6_lock);*out=np.status;
    out->version=X9_PA_NATIVE_STATUS_VERSION;
    lck_mtx_unlock(&i2c6_lock);return 0;
}
