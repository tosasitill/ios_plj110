/* Own-code encoding from OPPO audio_buf.h and mtk-dsp-platform-driver.c.
 * Shared records travel by an 8B physical-address PAYLOAD, not IPI_DMA.
 * Task acceptance is not AFE/DMA/PA readiness or a speaker cursor. */
#include "x9_dsp_pcm.h"
#if defined(KERNEL)
#include <sys/systm.h>
#include <sys/errno.h>
#else
#include <string.h>
#include <errno.h>
#endif
static uint32_t get32(const uint8_t *p)
{return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t get64(const uint8_t *p)
{return get32(p)|((uint64_t)get32(p+4)<<32);}
static void put32(uint8_t *p,uint32_t v)
{for(unsigned n=0;n<4;n++)p[n]=(uint8_t)(v>>(n*8));}
static void put64(uint8_t *p,uint64_t v)
{put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32));}
static int valid_buffer(const struct x9_dsp_pcm_buffer *b)
{
    if(!b || !b->base || !b->bytes || b->base>UINT64_MAX-b->bytes ||
       b->bytes>UINT32_MAX || b->count>b->bytes || b->hardware>1 ||
       b->memory>2 || b->ignore_irq>1 || !b->channels || b->channels>32 ||
       !b->rate || !b->period_frames || !b->periods ||
       b->period_frames>INT32_MAX || b->periods>INT32_MAX || b->counter>INT32_MAX ||
       !b->direction || b->direction>4 || (b->direction&(b->direction-1)))return EINVAL;
    uint64_t end=b->base+b->bytes;
    if(b->read<b->base || b->read>=end || b->write<b->base || b->write>=end)return EINVAL;
    return 0;
}
int x9_dsp_pcm_encode_buffer(const struct x9_dsp_pcm_buffer *b,void *out,uint32_t n)
{
    if(!out || n!=X9_DSP_PCM_RECORD_BYTES)return EINVAL;
    int e=valid_buffer(b);if(e)return e;
    uint8_t record[X9_DSP_PCM_RECORD_BYTES];uint8_t *p=record;memset(p,0,sizeof(record));
    put64(p,b->base);put64(p+8,b->base+b->bytes);put64(p+16,b->read);
    put64(p+24,b->write);put64(p+32,b->bytes);put64(p+40,b->count);
    put32(p+48,b->format);put32(p+52,b->rate);put32(p+56,b->channels);put32(p+60,b->direction);
    put32(p+64,b->start_threshold);put32(p+68,b->stop_threshold);
    put32(p+72,b->period_frames);put32(p+76,b->periods);
    p[80]=b->hardware;p[81]=b->memif;p[82]=b->irq;p[83]=b->memory;
    put32(p+84,b->counter);p[88]=b->ignore_irq;memcpy(out,record,sizeof(record));return 0;
}
int x9_dsp_pcm_decode_buffer(const void *in,uint32_t n,struct x9_dsp_pcm_buffer *out)
{
    if(!in || !out || n!=X9_DSP_PCM_RECORD_BYTES)return EINVAL;
    const uint8_t *p=in;struct x9_dsp_pcm_buffer b={0};
    b.base=get64(p);b.bytes=get64(p+32);b.read=get64(p+16);b.write=get64(p+24);b.count=get64(p+40);
    b.format=get32(p+48);b.rate=get32(p+52);b.channels=get32(p+56);b.direction=get32(p+60);
    b.start_threshold=get32(p+64);b.stop_threshold=get32(p+68);
    b.period_frames=get32(p+72);b.periods=get32(p+76);
    b.hardware=p[80];b.memif=p[81];b.irq=p[82];b.memory=p[83];b.counter=get32(p+84);b.ignore_irq=p[88];
    int e=valid_buffer(&b);if(e)return e;
    if(get64(p+8)!=b.base+b.bytes)return EINVAL;
    /* Padding is not a semantic field. Firmware need not zero its own C
     * padding; never treat those bytes as a host virtual address. */
    *out=b;return 0;
}
int x9_dsp_pcm_encode_dram(uint64_t bus,uint64_t ap,uint64_t bytes,void *out,uint32_t n)
{
    if(!out || n!=X9_DSP_DRAM_RECORD_BYTES || !bus || !ap || !bytes ||
       bus>UINT64_MAX-bytes || ap>UINT64_MAX-bytes)return EINVAL;
    uint8_t *p=out;put64(p,bus);put64(p+8,ap);put64(p+16,bytes);put64(p+24,ap);return 0;
}
#if defined(KERNEL)
enum { PCMInit=25, PCMShareATOD=5, PCMShareDTOA=6, PCMAFEParam=0x201,
       PCMAFEFree=0x202, PCMAFEPrepare=0x203, PCMConsume=27, PCMAudioIPI=9 };
static int fault(struct x9_dsp_pcm_session *s,int e)
{
    if(!s->error)s->error=(uint32_t)e;
    s->flags|=X9PCMFault;return e;
}
static int qualify(struct x9_dsp_pcm_session *s)
{
    if(!s || !s->bound)return ENXIO;
    if(s->error)return (int)s->error;
    if(s->lease_checks!=UINT64_MAX)s->lease_checks++;
    int e=s->hw.validate(s->hw.context);if(e==EAGAIN)e=EIO;
    if(!e && (s->flags&X9PCMAFEParams)) {
        e=s->hw.validate_afe(s->hw.context,&s->afe_buffer);
        if(e==EAGAIN)e=EIO;
    }
    X9AudioDSPStatus ds={0};
    if(!e)e=x9_adsp_status(s->hw.core,&ds);
    if(!e && (ds.state!=X9DSPOnline || ds.error || ds.generation!=s->hw.generation))e=ESTALE;
    return e?fault(s,e):0;
}
static int request(struct x9_dsp_pcm_session *s,uint16_t id,uint8_t ack,
                   const void *data,uint32_t bytes,uint32_t param1)
{
    int e=qualify(s);if(e)return e;
    if(bytes>X9_DSP_PAYLOAD_MAX || (bytes && !data))return EINVAL;
    struct x9_dsp_message m={0},reply={0};
    m.scene=(uint8_t)s->hw.scene;m.source=1;m.target=2;m.id=id;m.ack=ack;
    m.type=bytes?X9DSPPayload:X9DSPOnly;m.param1=bytes?bytes:param1;
    if(bytes)memcpy(m.payload,data,bytes);
    s->command=id;if(s->requests!=UINT64_MAX)s->requests++;
    uint32_t timeout=s->flags&X9PCMStartIssued?s->hw.copy_timeout_ms:s->hw.control_timeout_ms;
    e=x9_adsp_request(s->hw.core,&m,ack==X9DSPNeedAck?&reply:NULL,timeout);
    /* EAGAIN can mean no doorbell yet, but a preceding shared-memory write
     * may already be visible. This layer never silently retries it. */
    if(e)return fault(s,e==EAGAIN?EIO:e);
    if(ack==X9DSPNeedAck){s->reply_param1=reply.param1;s->reply_param2=reply.param2;}
    /* Raw reply values are NOT an invented errno or Ready certificate. */
    return qualify(s);
}
static int record_request(struct x9_dsp_pcm_session *s,uint16_t id,
                          const struct x9_dsp_pcm_buffer *b)
{
    uint8_t data[X9_DSP_PCM_RECORD_BYTES],address[8];
    int e=x9_dsp_pcm_encode_buffer(b,data,sizeof(data));if(e)return e;
    e=qualify(s);if(e)return e;
    if(b->hardware) {
        e=s->hw.validate_afe(s->hw.context,b);
        if(e)return fault(s,e==EAGAIN?EIO:e);
        e=qualify(s);if(e)return e;
    }
    memcpy(s->hw.region[X9PCMRegionATOD].cpu,data,sizeof(data));
    e=s->hw.sync_device(s->hw.context,X9PCMRegionATOD,0,sizeof(data));
    if(e)return fault(s,e==EAGAIN?EIO:e);
    e=qualify(s);if(e)return e;
    if(b->hardware) {
        e=s->hw.validate_afe(s->hw.context,b);
        if(e)return fault(s,e==EAGAIN?EIO:e);
        e=qualify(s);if(e)return e;
    }
    put64(address,s->hw.region[X9PCMRegionATOD].bus);
    /* NEED_ACK protects reuse of the single ATOD record even while running.
     * TX-consume/BYPASS alone does not prove DSP has copied the record. */
    e=request(s,id,X9DSPNeedAck,address,sizeof(address),0);
    if(!e && b->hardware) {
        e=s->hw.validate_afe(s->hw.context,b);
        if(e)return fault(s,e==EAGAIN?EIO:e);
        e=qualify(s);
    }
    return e;
}
static bool overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{return a<b+m && b<a+n;}
static int clear_region(struct x9_dsp_pcm_session *s,unsigned n)
{
    int e=qualify(s);if(e)return e;
    const struct x9_dsp_pcm_region *r=&s->hw.region[n];
    memset(r->cpu,0,r->bytes);
    e=s->hw.sync_device(s->hw.context,n,0,r->bytes);
    if(e)return fault(s,e==EAGAIN?EIO:e);
    return qualify(s);
}
int x9_dsp_pcm_bind(struct x9_dsp_pcm_session *s,const struct x9_dsp_pcm_binding *p)
{
    if(!s || !p || s->bound || p->version!=X9_DSP_PCM_BINDING_VERSION || p->core>1 ||
       !p->generation || p->scene>=64 || !p->validate || !p->validate_afe || !p->sync_device || !p->sync_cpu ||
       !p->control_timeout_ms || p->control_timeout_ms>250 ||
       !p->copy_timeout_ms || p->copy_timeout_ms>10)return EINVAL;
    for(unsigned n=0;n<X9PCMRegionCount;n++) {
        const struct x9_dsp_pcm_region *r=&p->region[n];
        if(!r->cpu || !r->bus || ((uintptr_t)r->cpu&127) || (r->bus&127) ||
           (r->bytes&127) || r->bytes<128 || r->bus>UINT64_MAX-r->bytes ||
           (uint64_t)(uintptr_t)r->cpu>UINT64_MAX-r->bytes)return EINVAL;
        for(unsigned k=0;k<n;k++)if(overlap(r->bus,r->bytes,p->region[k].bus,p->region[k].bytes) ||
            overlap((uint64_t)(uintptr_t)r->cpu,r->bytes,
                    (uint64_t)(uintptr_t)p->region[k].cpu,p->region[k].bytes))return EINVAL;
    }
    if(p->region[X9PCMRegionRing].bytes<65536 || p->region[X9PCMRegionRing].bytes>131072)return EINVAL;
    memset(s,0,sizeof(*s));s->hw=*p;s->bound=true;return 0;
}
int x9_dsp_pcm_open(struct x9_dsp_pcm_session *s)
{
    if(!s || !s->bound)return ENXIO;
    if(s->flags&X9PCMOpen)return EBUSY;
    if(s->used)return ESTALE;
    int e=qualify(s);if(e)return e;
    if(!(s->flags&X9PCMInitialized)) {
        /* gen_pool_dma_zalloc is the original baseline. Never register
         * inherited DTOA/ATOD contents as this generation's fresh records. */
        e=clear_region(s,X9PCMRegionATOD);
        if(!e)e=clear_region(s,X9PCMRegionDTOA);
        if(!e)e=request(s,PCMInit,X9DSPBypass,NULL,0,0);
        for(unsigned n=0;!e && n<2;n++) {
            uint8_t data[X9_DSP_DRAM_RECORD_BYTES];
            const struct x9_dsp_pcm_region *r=&s->hw.region[n];
            e=x9_dsp_pcm_encode_dram(r->bus,(uint64_t)(uintptr_t)r->cpu,r->bytes,data,sizeof(data));
            if(!e)e=request(s,n?PCMShareDTOA:PCMShareATOD,X9DSPBypass,data,sizeof(data),0);
        }
        if(e)return fault(s,e);
        s->flags|=X9PCMInitialized;
    }
    s->used=true; /* even an ambiguous OPEN may have created the task */
    e=request(s,X9DSPOpen,X9DSPNeedAck,NULL,0,0);if(!e)s->flags|=X9PCMOpen;return e;
}
int x9_dsp_pcm_hw_params(struct x9_dsp_pcm_session *s,uint32_t period)
{
    if(!s || !(s->flags&X9PCMOpen))return ENXIO;
    if(s->flags&(X9PCMParams|X9PCMPrepared|X9PCMStartIssued))return EBUSY;
    const struct x9_dsp_pcm_region *r=&s->hw.region[X9PCMRegionRing];
    if(!period || period>r->bytes/8 || (r->bytes/4)%period)return EINVAL;
    struct x9_dsp_pcm_buffer b={0};
    b.base=b.read=b.write=r->bus;b.bytes=r->bytes;
    b.format=2; /* SNDRV_PCM_FORMAT_S16_LE, NOT ETDM valid width 15 */
    b.rate=48000;b.channels=2;b.direction=1;b.memory=1;
    b.period_frames=period;b.periods=r->bytes/4/period;
    int e=record_request(s,X9DSPHWParam,&b);
    if(!e){s->buffer=b;s->submitted=s->consumed=0;s->underruns=0;s->pending_frames=0;s->flags|=X9PCMParams;}
    return e;
}
int x9_dsp_pcm_afe_params(struct x9_dsp_pcm_session *s,const struct x9_dsp_pcm_buffer *b,bool prepare)
{
    if(!s || !(s->flags&X9PCMParams))return ENXIO;
    if(s->flags&X9PCMStartIssued)return EBUSY;
    if(!b || !b->hardware || b->format!=2 || b->rate!=48000 || b->channels!=2 || b->direction!=1)
        return EINVAL;
    /* Callback and mailbox waits must not cause us to encode one descriptor
     * and retain a later caller-owned image as the supposedly same binding. */
    const struct x9_dsp_pcm_buffer candidate=*b;b=&candidate;
    if(prepare && !(s->flags&X9PCMAFEParams))return ENXIO;
    if(!prepare && (s->flags&X9PCMAFEParams))return EBUSY;
    if(prepare) {
        const struct x9_dsp_pcm_buffer *a=&s->afe_buffer;
        if(b->base!=a->base || b->bytes!=a->bytes || b->memif!=a->memif || b->irq!=a->irq ||
           b->memory!=a->memory || b->period_frames!=a->period_frames || b->periods!=a->periods)
            return EINVAL;
    }
    int e=record_request(s,prepare?PCMAFEPrepare:PCMAFEParam,b);
    if(!e){s->afe_buffer=*b;s->flags|=prepare?X9PCMAFEPrepared:X9PCMAFEParams;}return e;
}
int x9_dsp_pcm_prepare(struct x9_dsp_pcm_session *s,uint32_t start)
{
    if(!s || !(s->flags&X9PCMParams))return ENXIO;
    if(s->flags&(X9PCMPrepared|X9PCMStartIssued))return EBUSY;
    if(start<960 || start>=s->buffer.bytes/4 || start%s->buffer.period_frames)return EINVAL;
    struct x9_dsp_pcm_buffer b=s->buffer;
    b.read=b.write=b.base;b.count=0;b.counter=0;b.start_threshold=start;b.stop_threshold=UINT32_MAX;
    s->pending_frames=(uint32_t)(b.bytes/4);
    int e=clear_region(s,X9PCMRegionRing);
    if(!e)e=record_request(s,X9DSPPrepare,&b);
    if(!e){s->buffer=b;s->submitted=s->consumed=0;s->pending_frames=0;s->flags|=X9PCMPrepared;}
    return e;
}
int x9_dsp_pcm_submit(struct x9_dsp_pcm_session *s,const int16_t *pcm,uint32_t frames)
{
    if(!s || !(s->flags&X9PCMPrepared))return ENXIO;
    if(!pcm || !frames || frames>s->buffer.bytes/4)return EINVAL;
    uint64_t address=(uint64_t)(uintptr_t)pcm;
    if((address&1) || address>UINT64_MAX-(uint64_t)frames*4)return EINVAL;
    for(unsigned n=0;n<X9PCMRegionCount;n++)if(overlap(address,(uint64_t)frames*4,
        (uint64_t)(uintptr_t)s->hw.region[n].cpu,s->hw.region[n].bytes))return EINVAL;
    int e=qualify(s);if(e)return e;
    if(s->submitted<s->consumed || s->submitted>UINT64_MAX-frames)return fault(s,EOVERFLOW);
    uint64_t pending=s->submitted-s->consumed;
    if(pending+frames>s->buffer.bytes/4-32)return EAGAIN; /* no writes yet */
    if(s->buffer.counter==INT32_MAX)return fault(s,EOVERFLOW);
    uint32_t offset=(uint32_t)(s->submitted%(s->buffer.bytes/4))*4;
    uint32_t bytes=frames*4,first=bytes;
    if(first>s->buffer.bytes-offset)first=(uint32_t)s->buffer.bytes-offset;
    s->pending_frames=frames;
    void *ring=s->hw.region[X9PCMRegionRing].cpu;
    memcpy((uint8_t *)ring+offset,pcm,first);
    e=s->hw.sync_device(s->hw.context,X9PCMRegionRing,offset,first);
    if(e)return fault(s,e==EAGAIN?EIO:e);
    e=qualify(s);if(e)return e;
    if(first<bytes) {
        memcpy(ring,(const uint8_t *)pcm+first,bytes-first);
        e=s->hw.sync_device(s->hw.context,X9PCMRegionRing,0,bytes-first);
        if(e)return fault(s,e==EAGAIN?EIO:e);
        e=qualify(s);if(e)return e;
    }
    struct x9_dsp_pcm_buffer b=s->buffer;
    b.write=b.base+((s->submitted+frames)%(b.bytes/4))*4;
    b.read=b.base+(s->consumed%(b.bytes/4))*4;b.count=(pending+frames)*4;b.counter++;
    e=record_request(s,X9DSPDLCopy,&b);
    if(!e){s->buffer=b;s->submitted+=frames;s->pending_frames=0;}
    return e;
}
int x9_dsp_pcm_start(struct x9_dsp_pcm_session *s)
{
    if(!s || !(s->flags&X9PCMPrepared) || !(s->flags&X9PCMAFEPrepared))return ENXIO;
    if(s->flags&X9PCMStartIssued)return EBUSY;
    if(s->submitted<s->buffer.start_threshold || s->pending_frames)return ENXIO;
    /* Publish uncertainty before the direct START; success only means TX
     * consumption, not audible output or a hardware frame-zero anchor. */
    s->flags|=X9PCMStartIssued;
    return request(s,X9DSPStart,X9DSPDirect,NULL,0,1);
}
int x9_dsp_pcm_stop(struct x9_dsp_pcm_session *s)
{
    if(!s || !s->bound)return ENXIO;
    if(!(s->flags&X9PCMStartIssued))return s->error?(int)s->error:0;
    int e=request(s,X9DSPStop,X9DSPDirect,NULL,0,1);
    if(!e)s->flags&=~(X9PCMStartIssued|X9PCMPrepared);
    /* Direct STOP is NOT authoritative DMA quiescence or permission to
     * free the ring. The combined backend must independently mute/drain. */
    return e;
}
int x9_dsp_pcm_close(struct x9_dsp_pcm_session *s)
{
    if(!s || !s->bound)return ENXIO;
    if(s->error)return (int)s->error;
    if(s->flags&X9PCMStartIssued)return EBUSY;
    if(!(s->flags&X9PCMOpen))return 0;
    int e=0;
    if(s->flags&X9PCMAFEParams) {
        e=request(s,PCMAFEFree,X9DSPNeedAck,NULL,0,1);
        if(!e)s->flags&=~(X9PCMAFEParams|X9PCMAFEPrepared);
    }
    if(!e && (s->flags&X9PCMParams)) {
        e=request(s,X9DSPHWFree,X9DSPNeedAck,NULL,0,1);
        if(!e)s->flags&=~(X9PCMParams|X9PCMPrepared);
    }
    if(!e)e=request(s,X9DSPClose,X9DSPNeedAck,NULL,0,0);
    if(!e)s->flags&=~X9PCMOpen;
    return e;
}
static int consume_record(struct x9_dsp_pcm_session *s,const void *data,uint32_t bytes)
{
    struct x9_dsp_pcm_buffer b;int e=x9_dsp_pcm_decode_buffer(data,bytes,&b);if(e)return fault(s,e);
    const struct x9_dsp_pcm_buffer *a=&s->buffer;
    if(b.base!=a->base || b.bytes!=a->bytes || b.format!=a->format || b.rate!=a->rate ||
       b.channels!=a->channels || b.direction!=a->direction || b.hardware!=a->hardware ||
       b.memory!=a->memory || b.memif!=a->memif || b.irq!=a->irq || b.ignore_irq!=a->ignore_irq ||
       b.period_frames!=a->period_frames || b.periods!=a->periods ||
       ((b.read-b.base)|(b.write-b.base)|b.count)&3)return fault(s,EPROTO);
    if(s->submitted<s->consumed)return fault(s,EIO);
    uint32_t old=(uint32_t)(s->consumed%(a->bytes/4))*4;
    uint32_t now=(uint32_t)(b.read-b.base);
    uint32_t delta=(uint32_t)((now+a->bytes-old)%a->bytes);
    /* Outstanding stays strictly below one ring: a full-wrap ambiguity is
     * impossible without a prior overrun/invalid producer. */
    if(delta/4>s->submitted-s->consumed)return fault(s,EIO);
    s->consumed+=delta/4;s->buffer.read=b.read;
    s->buffer.count=(s->submitted-s->consumed)*4;return 0;
}
int x9_dsp_pcm_observe(struct x9_dsp_pcm_session *s)
{
    if(!s || !(s->flags&X9PCMStartIssued))return ENXIO;
    uint8_t a[X9_DSP_PCM_RECORD_BYTES],b[X9_DSP_PCM_RECORD_BYTES];
    for(unsigned n=0;n<3;n++) {
        int e=qualify(s);if(e)return e;
        e=s->hw.sync_cpu(s->hw.context,X9PCMRegionDTOA,0,sizeof(a));
        if(e)return fault(s,e==EAGAIN?EIO:e);
        e=qualify(s);if(e)return e;
        memcpy(a,s->hw.region[X9PCMRegionDTOA].cpu,sizeof(a));
        e=s->hw.sync_cpu(s->hw.context,X9PCMRegionDTOA,0,sizeof(b));
        if(e)return fault(s,e==EAGAIN?EIO:e);
        e=qualify(s);if(e)return e;
        memcpy(b,s->hw.region[X9PCMRegionDTOA].cpu,sizeof(b));
        if(!memcmp(a,b,sizeof(a)))return consume_record(s,a,sizeof(a));
    }
    return EAGAIN; /* no cursor mutation from a torn shared record */
}
int x9_dsp_pcm_event(struct x9_dsp_pcm_session *s,const struct x9_adsp_event *event)
{
    if(!s || !event || !s->bound)return EINVAL;
    if(event->id!=PCMAudioIPI || event->generation!=s->hw.generation)return ESTALE;
    struct x9_dsp_message m;
    int e=x9_dsp_decode(event->data,event->length,&m);if(e)return e;
    if(m.scene!=s->hw.scene || m.source!=2 || m.target!=1 || m.id!=PCMConsume)return ENOMSG;
    if(!(s->flags&X9PCMStartIssued))return ENXIO;
    e=qualify(s);if(e)return e;
    if(m.param2==1)return fault(s,ESTALE);
    if(m.param2==2){if(s->underruns!=UINT64_MAX)s->underruns++;return fault(s,EPIPE);}
    if(m.param2)return fault(s,EPROTO);
    if(m.type==X9DSPPayload)return consume_record(s,m.payload,m.param1);
    if(m.type!=X9DSPOnly)return fault(s,EPROTO);
    return x9_dsp_pcm_observe(s);
}
#endif
