/* Included after x9_i2c6_native.inc in x9_tfa_plj110.c. Use that actual
 * controller/mutex/state, never a fabricated x9_tfa_bus.verified. */
#include "x9_pa_native.h"
_Static_assert(sizeof(X9AudioPANativeStatus)==64,"native PA cached v1");
_Static_assert(sizeof(struct x9_pa_re25_snapshot)==48,"native Re25 observation48");
static struct {
    X9AudioPANativeStatus status;
    uint64_t ticket,lease,epoch;
    uint32_t attempted_identity;
    bool bound,re25_attempted;
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
        np.epoch=ni.policy.reset_epoch;np.attempted_identity=0;
        np.re25_attempted=false;np.bound=true;
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
static int np_bytes_overlap(uint64_t a,uint32_t an,uint64_t b,uint32_t bn)
{return a<=b?b-a<an:a-b<bn;}
/* Pmap metadata only, no off-domain access. Compare byte intervals, not
 * whole RAM pages: another non-overlapping stack object is legal. */
static int np_output_disjoint(const void *out,uint32_t bytes,const void *object,uint32_t n)
{
    if(np_bytes_overlap((uintptr_t)out,bytes,(uintptr_t)object,n))return EINVAL;
    for(uint32_t a=0;a<bytes;) {
        addr64_t av=(addr64_t)(uintptr_t)out+a;
        ppnum_t ap=pmap_find_phys(kernel_pmap,av);if(!ap)return EFAULT;
        uint32_t ac=(uint32_t)(PAGE_SIZE-(av&PAGE_MASK));if(ac>bytes-a)ac=bytes-a;
        uint64_t pa=ptoa_64(ap)+(av&PAGE_MASK);
        for(uint32_t b=0;b<n;) {
            addr64_t bv=(addr64_t)(uintptr_t)object+b;
            ppnum_t bp=pmap_find_phys(kernel_pmap,bv);if(!bp)return EFAULT;
            uint32_t bc=(uint32_t)(PAGE_SIZE-(bv&PAGE_MASK));if(bc>n-b)bc=n-b;
            if(np_bytes_overlap(pa,ac,ptoa_64(bp)+(bv&PAGE_MASK),bc))return EINVAL;
            b+=bc;
        }
        a+=ac;
    }
    return 0;
}
static int np_re25_output(const struct x9_pa_re25_snapshot *out)
{
    if(!out || ((uintptr_t)out&7u) || (uintptr_t)out>UINTPTR_MAX-sizeof(*out) || PAGE_SIZE!=0x4000)return EINVAL;
    int e=np_output_disjoint(out,sizeof(*out),&np,sizeof(np));
    if(!e)e=np_output_disjoint(out,sizeof(*out),&ni,sizeof(ni));
    if(!e)e=np_output_disjoint(out,sizeof(*out),&j,sizeof(j));
    if(!e)e=np_output_disjoint(out,sizeof(*out),&s,sizeof(s));
    if(!e)e=np_output_disjoint(out,sizeof(*out),&native_bus_held,sizeof(native_bus_held));
    if(!e)e=np_output_disjoint(out,sizeof(*out),&i2c6_lock,sizeof(i2c6_lock));
    if(e)return e;
    const uint64_t base[]={0x1002c000,0x129d0000,0x16640000,0x12990000,
        0x16410000,0x1c210000,0x1c018000};
    const uint32_t bytes[]={0x4000,0x4000,0x4000,0x20000,0x10000,0x4000,0x4000};
    for(uint32_t a=0;a<sizeof(*out);) {
        addr64_t va=(addr64_t)(uintptr_t)out+a;
        ppnum_t pp=pmap_find_phys(kernel_pmap,va);if(!pp)return EFAULT;
        uint32_t n=(uint32_t)(PAGE_SIZE-(va&PAGE_MASK));if(n>sizeof(*out)-a)n=(uint32_t)sizeof(*out)-a;
        uint64_t pa=ptoa_64(pp)+(va&PAGE_MASK);
        for(unsigned i=0;i<sizeof(base)/sizeof(base[0]);i++)
            if(np_bytes_overlap(pa,n,base[i],bytes[i]))return EINVAL;
        a+=n;
    }
    return 0;
}
int x9_pa_native_re25_read(uint64_t ticket,uint64_t lease,struct x9_pa_re25_snapshot *out)
{
    lck_mtx_lock(&i2c6_lock);int e=n_match(ticket,lease);
    if(!e)e=np_re25_output(out); /* BEFORE any PA read or epoch mutation */
    if(!e)e=np_parent(ticket,lease,false);
    if(!e && (np.status.error || ni.error || np.status.mute_uncertain ||
       np.status.revision_verified!=3 || np.status.muted!=3 || np.re25_attempted))e=EBUSY;
    if(!e)e=n_online(); /* new observation, not fault/offline cleanup */
    struct x9_pa_re25_snapshot snapshot={0};snapshot.version=1;
    bool observed=false;unsigned side=2;
    for(unsigned i=0;!e && i<2;i++) {
        side=i;observed=true;e=np_read_mute(i,true,np.status.sysctrl[i]);
    }
    if(!e) {
        /* No retry by replacing output or re-entering after a partial read. */
        np.re25_attempted=true;snapshot.generation=np.status.generation;
        snapshot.ticket=ticket;snapshot.i2c6_lease=lease;snapshot.reset_epoch=np.epoch;
        for(unsigned i=0;!e && i<2;i++) {
            side=i;
            snapshot.revision[i]=np.status.revision[i];snapshot.sysctrl[i]=np.status.sysctrl[i];
            e=np_transfer(i,0xf0,&snapshot.mtp_control[i],true);
            if(!e)e=np_transfer(i,0xf5,&snapshot.r25c_mohm[i],true);
            if(!e && ((snapshot.mtp_control[i]&3)!=3 || !snapshot.r25c_mohm[i] ||
               snapshot.r25c_mohm[i]==UINT16_MAX))e=ENODATA;
        }
        /* Reverse-side second snapshot also brackets the complete pair;
         * stable shadow numbers are not a MTP-busy/calibration-done proof. */
        for(unsigned n=2;!e && n;n--) {
            unsigned i=n-1;uint16_t value=0,control=0;side=i;
            e=np_transfer(i,0xf5,&value,true);
            if(!e)e=np_transfer(i,0xf0,&control,true);
            if(!e && (value!=snapshot.r25c_mohm[i] || control!=snapshot.mtp_control[i]))e=EIO;
        }
        for(unsigned i=0;!e && i<2;i++) {
            side=i;e=np_read_mute(i,true,snapshot.sysctrl[i]);
        }
        if(!e)side=2; /* Remaining errors concern the whole binding/output. */
        if(!e)e=n_online();if(!e)e=n_idle(false);
        if(!e)e=np_re25_output(out);
    }
    /* Wrong-ticket/output/preflight rejection does not poison another
     * binding; errors from actual transfers still use n_fault above. */
    if(e && observed)e=np_fault(side,e);
    if(!e)*out=snapshot;
    lck_mtx_unlock(&i2c6_lock);return e;
}
