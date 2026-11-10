/* Independent implementation of OPPO ADSP-v3 mtk-mbox/adsp_core wire ABI.
 * Polling deliberately has no IRQ consumer racing slot copies/clear.
 * Physical board bring-up and audio shared-DMA descriptors remain separate. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include "x9_adsp_transport.h"
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *,int);

enum { SET=0x100/4, CLR=0x10c/4, AUDIO_IPI=9, READY_IPI=13,
       WDT_IPI=0, IPI_COUNT=35, EVENT_COUNT=16 };
struct transport {
    struct x9_adsp_platform hw;
    X9AudioDSPStatus status;
    bool registered, powered;
    uint32_t head, count;
    struct x9_adsp_event events[EVENT_COUNT];
};
LCK_GRP_DECLARE(adsp_group,"x9-adsp");
LCK_MTX_DECLARE(adsp_lock0,&adsp_group);
LCK_MTX_DECLARE(adsp_lock1,&adsp_group);
static struct transport cores[2];
static lck_mtx_t *locks[2]={&adsp_lock0,&adsp_lock1};

static uint32_t rd(volatile uint32_t *p)
{
    uint32_t v=*p; __asm__ volatile("dmb oshld":::"memory"); return v;
}
static void wr(volatile uint32_t *p,uint32_t v)
{
    *p=v; __asm__ volatile("dsb sy":::"memory");
}
static uint64_t deadline(uint32_t milliseconds)
{
    uint64_t ticks; nanoseconds_to_absolutetime((uint64_t)milliseconds*1000000,&ticks);
    return mach_absolute_time()+ticks;
}
static void pause_poll(void) { delay_for_interval(1,1000*NSEC_PER_USEC); }
static int poison(struct transport *t,int error)
{
    t->status.state=X9DSPQuarantined; t->status.error=(uint32_t)error;
    return error;
}
static int queue_event(struct transport *t,const struct x9_adsp_event *e)
{
    if(t->count==EVENT_COUNT) {
        t->status.event_overflows++; return poison(t,ENOBUFS);
    }
    t->events[(t->head+t->count)%EVENT_COUNT]=*e;
    t->count++; t->status.event_pending=t->count; return 0;
}
/* Caller holds power/wake ownership and core mutex. Copy the outer header
 * once; bound its untrusted byte length BEFORE touching payload. DSP holds
 * this pin until CLR, so the copied slot is stable. Tail padding is ignored.
 * Linux sends fixed 60-slot direct RX; len remains the inner byte length. */
static int receive(struct transport *t,struct x9_adsp_event *e)
{
    uint32_t pending=rd(t->hw.rx+CLR);
    if(!pending)return EAGAIN;
    if(pending!=1) {t->status.malformed++; return poison(t,EPROTO);} /* never clear another pin */
    uint32_t id=rd(t->hw.rx),len=rd(t->hw.rx+1);
    /* options/reserved are not validated on RX: OPPO's RX handler ignores
     * them and no captured firmware reply proves an echo requirement. */
    if(id>=IPI_COUNT || len>X9_ADSP_EVENT_BYTES) {
        wr(t->hw.rx+CLR,1); t->status.malformed++; return poison(t,EMSGSIZE);
    }
    bzero(e,sizeof(*e)); e->id=id; e->length=len; e->generation=t->status.generation;
    for(uint32_t i=0;i<len;i+=4) {
        uint32_t v=rd(t->hw.rx+4+i/4);
        for(uint32_t j=0;j<4 && i+j<len;j++)e->data[i+j]=(uint8_t)(v>>(j*8));
    }
    wr(t->hw.rx+CLR,1); /* release only after complete private copy */
    t->status.received++; t->status.last_ipi=id; return 0;
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static int dispatch(struct transport *t,const struct x9_adsp_event *e,
                    const struct x9_dsp_message *request,
                    struct x9_dsp_message *reply,bool *matched)
{
    if(e->id==WDT_IPI)return poison(t,EIO);
    if(e->id==READY_IPI) {
        if(e->length!=4 || get32(e->data)!=t->hw.core) {
            t->status.malformed++; return poison(t,EPROTO);
        }
        /* A READY while running is an unrequested DSP restart, not recovery
         * of a timed-out command or permission to reuse its task/message ID. */
        if(t->status.state!=X9DSPBooting)return poison(t,ESTALE);
        t->status.state=X9DSPOnline; t->status.ready_count++; return 0;
    }
    if(e->id==AUDIO_IPI) {
        struct x9_dsp_message m;
        int error=x9_dsp_decode(e->data,e->length,&m);
        if(error || m.source!=2 || m.target!=1) {
            t->status.malformed++; return poison(t,error?error:EPROTO);
        }
        if(m.ack==X9DSPAckBack) {
            if(!request || !x9_dsp_ack_matches(request,&m) || *matched) {
                t->status.stray_acks++; return poison(t,ESTALE);
            }
            *reply=m; *matched=true; t->status.matched_acks++; return 0;
        }
    }
    return queue_event(t,e); /* logger/timesync/audio events, no callback recursion */
}
static int finish_wake(struct transport *t,int error)
{
    int release=t->hw.wake_unlock(t->hw.context);
    if(release) {t->status.wake_errors++; return poison(t,release);}
    if(error && error!=EAGAIN)t->status.error=(uint32_t)error;
    return error;
}
int x9_adsp_register_transport(const struct x9_adsp_platform *p)
{
    if(!sweet_early_is_x9() || sweet_x9_arg("x9_audio",0)!=1)return ENXIO;
    if(!p || p->version!=1 || p->core>=2 || !p->tx || !p->rx || p->tx==p->rx ||
       ((uintptr_t)p->tx&3) || ((uintptr_t)p->rx&3) || !p->power_on || !p->power_off ||
       !p->stop_and_reset || !p->core_start || !p->wake_lock || !p->wake_unlock)return EINVAL;
    lck_mtx_lock(locks[p->core]); struct transport *t=&cores[p->core];
    int e=t->registered?EBUSY:0;
    if(!e) {
        t->hw=*p; t->registered=true;
        t->status.version=1; t->status.core=p->core; t->status.state=X9DSPOffline;
    }
    lck_mtx_unlock(locks[p->core]); return e;
}
int x9_adsp_status(uint32_t core,X9AudioDSPStatus *s)
{
    if(core>=2 || !s)return EINVAL;
    lck_mtx_lock(locks[core]); *s=cores[core].status;
    s->version=1; s->core=core;
    lck_mtx_unlock(locks[core]); return 0;
}
int x9_adsp_boot(uint32_t core)
{
    if(core>=2)return EINVAL;
    lck_mtx_lock(locks[core]); struct transport *t=&cores[core]; int e=0;
    if(!t->registered)e=ENXIO;
    else if(t->status.state==X9DSPOnline || t->status.state==X9DSPBooting)e=EBUSY;
    else {
        /* power_on must unwind its own failure without MMIO from this layer.
         * After success retain the lease until reset/power_off both succeed. */
        if(!t->powered) {
            e=t->hw.power_on(t->hw.context);
            if(!e)t->powered=true;
        }
        if(!e)e=t->hw.stop_and_reset(t->hw.context);
        if(!e) {
            t->status.generation++;
            t->head=t->count=t->status.event_pending=0; bzero(t->events,sizeof(t->events));
            /* Core is HALTED. Discard old inbound pin, never clear AP's TX
             * bit ourselves to hide a firmware that has not consumed it. */
            uint32_t pending=rd(t->hw.rx+CLR);
            if(pending&~1u)e=EPROTO;
            else if(pending)wr(t->hw.rx+CLR,1);
            if(!e && rd(t->hw.tx+SET))e=EBUSY;
        }
        if(!e) {
            t->status.state=X9DSPBooting; t->status.error=0;
            e=t->hw.core_start(t->hw.context);
        }
        uint64_t until=deadline(2000);
        while(!e && t->status.state==X9DSPBooting) {
            if(mach_absolute_time()>=until) {t->status.timeouts++; e=ETIMEDOUT; break;}
            struct x9_adsp_event event; bool matched=false;
            int r=receive(t,&event);
            if(!r)e=dispatch(t,&event,NULL,NULL,&matched);
            else if(r!=EAGAIN)e=r;
            if(!e && t->status.state==X9DSPBooting) {
                if(mach_absolute_time()>=until) {t->status.timeouts++; e=ETIMEDOUT;}
                else pause_poll();
            }
        }
        if(e) {
            /* stop is required even if core_start only partially succeeded.
             * Do NOT drop the power lease when reset failed. */
            if(t->powered) {
                int off=t->hw.stop_and_reset(t->hw.context);
                if(!off) {off=t->hw.power_off(t->hw.context); if(!off)t->powered=false;}
                if(off)e=off;
            }
            poison(t,e);
        }
    }
    lck_mtx_unlock(locks[core]); return e;
}
int x9_adsp_shutdown(uint32_t core)
{
    if(core>=2)return EINVAL;
    lck_mtx_lock(locks[core]); struct transport *t=&cores[core]; int e=0;
    if(!t->registered)e=ENXIO;
    else if(t->powered) {
        e=t->hw.stop_and_reset(t->hw.context);
        if(!e)e=t->hw.power_off(t->hw.context);
        if(!e)t->powered=false;
    }
    if(t->registered) {
        t->head=t->count=t->status.event_pending=0; bzero(t->events,sizeof(t->events));
        /* shutdown never makes a quarantined session usable; next boot must
         * still perform reset and observe a fresh READY. */
        if(e)poison(t,e); else {t->status.state=X9DSPOffline; t->status.error=0;}
    }
    lck_mtx_unlock(locks[core]); return e;
}
int x9_adsp_poll(uint32_t core)
{
    if(core>=2)return EINVAL;
    lck_mtx_lock(locks[core]); struct transport *t=&cores[core]; int e=0;
    if(t->status.state!=X9DSPOnline)e=ENXIO;
    else if((e=t->hw.wake_lock(t->hw.context))) {t->status.wake_errors++; poison(t,e);}
    else {
        struct x9_adsp_event event; bool matched=false;
        e=receive(t,&event);
        if(!e)e=dispatch(t,&event,NULL,NULL,&matched);
        e=finish_wake(t,e);
    }
    lck_mtx_unlock(locks[core]); return e;
}
int x9_adsp_next_event(uint32_t core,struct x9_adsp_event *e)
{
    if(core>=2 || !e)return EINVAL;
    lck_mtx_lock(locks[core]); struct transport *t=&cores[core]; int r=EAGAIN;
    if(t->count) {
        *e=t->events[t->head]; bzero(&t->events[t->head],sizeof(*e));
        t->head=(t->head+1)%EVENT_COUNT; t->count--; t->status.event_pending=t->count; r=0;
    }
    lck_mtx_unlock(locks[core]); return r;
}
int x9_adsp_request(uint32_t core,const struct x9_dsp_message *request,
                    struct x9_dsp_message *reply,uint32_t timeout_ms)
{
    if(core>=2 || !request || request->source!=1 || request->target!=2 ||
       request->ack==X9DSPAckBack || (request->ack==X9DSPNeedAck && !reply) ||
       !timeout_ms || timeout_ms>250)return EINVAL;
    /* Own a private snapshot before locking or touching hardware. encode
     * rejects DMA descriptors and bounds payload. Zero rounded MMIO tail. */
    struct x9_dsp_message message=*request;
    uint8_t wire[X9_DSP_MESSAGE_MAX]={0}; uint32_t bytes=sizeof(wire);
    int e=x9_dsp_encode(&message,wire,&bytes); if(e)return e;
    lck_mtx_lock(locks[core]); struct transport *t=&cores[core];
    struct x9_dsp_message answer; bool matched=false;
    if(t->status.state!=X9DSPOnline)e=ENXIO;
    else if((e=t->hw.wake_lock(t->hw.context))) {t->status.wake_errors++; poison(t,e);}
    else {
        /* Drain at most one preexisting event. If more remain, return EAGAIN
         * without a doorbell. A stale ACK cannot match the new request. */
        struct x9_adsp_event event;
        int r=receive(t,&event);
        if(!r)e=dispatch(t,&event,NULL,NULL,&matched);
        else if(r!=EAGAIN)e=r;
        if(!e && rd(t->hw.rx+CLR))e=EAGAIN;
        if(!e && rd(t->hw.tx+SET))e=poison(t,EBUSY);
        if(!e) {
            /* mtk_ipi_msg_hd: four LE u32, followed by the inner audio_ipi.
             * Never copy the host struct's data pointer into mailbox SRAM. */
            wr(t->hw.tx,AUDIO_IPI); wr(t->hw.tx+1,bytes);
            wr(t->hw.tx+2,0xffff0000u); wr(t->hw.tx+3,0xdeadbeefu);
            for(uint32_t i=0;i<bytes;i+=4)wr(t->hw.tx+4+i/4,get32(wire+i));
            __asm__ volatile("dsb sy":::"memory"); wr(t->hw.tx+SET,1);
            t->status.sent++; t->status.last_message=message.id; t->status.error=0;
            uint64_t consumed_by=deadline(1),acked_by=deadline(timeout_ms);
            bool consumed=false;
            while(!e) {
                uint64_t now=mach_absolute_time();
                if((!consumed && now>=consumed_by) ||
                   (message.ack==X9DSPNeedAck && !matched && now>=acked_by)) {
                    t->status.timeouts++; e=poison(t,ETIMEDOUT); break;
                }
                /* Once consumed, SET can only become busy from an unexpected
                 * other owner. No pipelining is allowed on this transport. */
                uint32_t pending=rd(t->hw.tx+SET);
                if(pending&~1u) {e=poison(t,EPROTO); break;}
                bool busy=(pending&1)!=0;
                if(consumed && busy) {e=poison(t,EBUSY); break;}
                if(!busy)consumed=true;
                r=receive(t,&event);
                if(!r)e=dispatch(t,&event,&message,&answer,&matched);
                else if(r!=EAGAIN)e=r;
                if(e)break;
                if(consumed && (message.ack!=X9DSPNeedAck || matched))break;
                now=mach_absolute_time();
                if((!consumed && now>=consumed_by) ||
                   (message.ack==X9DSPNeedAck && !matched && now>=acked_by)) {
                    t->status.timeouts++; e=poison(t,ETIMEDOUT); break;
                }
                /* TX consumption is usually microseconds, not a 1ms sleep. */
                if(!consumed)delay(10); else pause_poll();
            }
        }
        e=finish_wake(t,e);
        if(!e && message.ack==X9DSPNeedAck)*reply=answer;
    }
    lck_mtx_unlock(locks[core]); return e;
}
