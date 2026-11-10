/* Byte-only codec matching OPPO tfa_tib_dsp_msgmulti/tfa_dsp_msg.
 * Commands/coefficients are opaque data, never passed to a bus or mailbox. */
#if defined(KERNEL)
#include <sys/errno.h>
#else
#include <errno.h>
#endif
#include "x9_tfa_multimsg.h"

static uint32_t word_bytes(uint32_t packing)
{return packing==X9TFABE24?3u:packing==X9TFALE32?4u:0u;}
static uint32_t limit(uint32_t packing)
{return packing==X9TFABE24?X9_TFA_MULTIMSG_BE24_MAX:X9_TFA_MULTIMSG_LE32_MAX;}
static uint32_t word(const uint8_t *p,uint32_t packing)
{
    if(packing==X9TFABE24)
        return ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2];
    return ((uint32_t)p[2]<<16)|((uint32_t)p[1]<<8)|p[0];
}
static int span(const void *p,uint32_t bytes)
{return p && bytes && (uintptr_t)p<=UINTPTR_MAX-bytes;}
static int overlap(const void *a,uint32_t an,const void *b,uint32_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=y?y-x<an:x-y<bn;
}
/* Metadata is local to the caller. Only publish after a complete pass. */
static int scan(const void *data,uint32_t bytes,uint32_t packing,
                struct x9_tfa_multimsg_info *info)
{
    uint32_t w=word_bytes(packing);
    if(!w || !span(data,bytes) || bytes%w || bytes<4u*w)return EINVAL;
    if(bytes>limit(packing))return E2BIG;
    const uint8_t *p=data;
    if(word(p,packing)!=X9_TFA_MULTIMSG_COMMAND || (w==4 && p[3]))return EBADMSG;
    struct x9_tfa_multimsg_info s={X9_TFA_MULTIMSG_VERSION,packing,bytes,0,0,0,0,0};
    uint32_t off=w;
    while(off<bytes) {
        /* Each word count is unsigned 16 bits in both vendor encodings. */
        if((w==3 && p[off]) || (w==4 && (p[off+2] || p[off+3])))return EBADMSG;
        uint32_t count=word(p+off,packing);off+=w;
        if(!count) {
            if(off!=bytes || !s.messages)return EBADMSG;
            s.be24_bytes=(bytes/w)*3u;s.le32_bytes=(bytes/w)*4u;
            *info=s;return 0;
        }
        if(count>(bytes-off)/w)return EBADMSG;
        if(w==4)for(uint32_t i=0;i<count;i++) {
            const uint8_t *v=p+off+i*w;
            if(v[3]!=(v[2]&0x80u?0xffu:0u))return EBADMSG;
        }
        s.messages++;s.payload_words+=count;
        if(count>s.max_message_words)s.max_message_words=count;
        off+=count*w;
    }
    return EBADMSG; /* A full-length final payload is not a terminator. */
}
int x9_tfa_multimsg_scan(const void *data,uint32_t bytes,uint32_t packing,
                        struct x9_tfa_multimsg_info *out)
{
    if(!span(out,sizeof(*out)) || !span(data,bytes) ||
       overlap(data,bytes,out,sizeof(*out)))return EINVAL;
    struct x9_tfa_multimsg_info s;int e=scan(data,bytes,packing,&s);
    if(!e)*out=s;
    return e;
}
int x9_tfa_multimsg_get(const void *data,uint32_t bytes,uint32_t packing,
                       uint32_t index,struct x9_tfa_multimsg_message *out)
{
    if(!span(out,sizeof(*out)) || !span(data,bytes) ||
       overlap(data,bytes,out,sizeof(*out)))return EINVAL;
    struct x9_tfa_multimsg_info s;int e=scan(data,bytes,packing,&s);if(e)return e;
    if(index>=s.messages)return ENOENT;
    const uint8_t *p=data;uint32_t w=word_bytes(packing),off=w;
    for(uint32_t i=0;i<=index;i++) {
        uint32_t count=word(p+off,packing);off+=w;
        if(i==index) {
            struct x9_tfa_multimsg_message m={off,count*w,count,word(p+off,packing)};
            *out=m;return 0;
        }
        off+=count*w;
    }
    return ENOENT;
}
int x9_tfa_multimsg_convert(const void *data,uint32_t bytes,uint32_t packing,
                           uint32_t target,void *dest,uint32_t capacity,
                           uint32_t *written_bytes)
{
    if(!word_bytes(target) || !span(dest,capacity) ||
       !span(written_bytes,sizeof(*written_bytes)) || !span(data,bytes) ||
       overlap(data,bytes,dest,capacity) ||
       overlap(data,bytes,written_bytes,sizeof(*written_bytes)) ||
       overlap(dest,capacity,written_bytes,sizeof(*written_bytes)))return EINVAL;
    struct x9_tfa_multimsg_info s;int e=scan(data,bytes,packing,&s);if(e)return e;
    uint32_t n=target==X9TFABE24?s.be24_bytes:s.le32_bytes;
    if(n>limit(target))return E2BIG;
    if(n>capacity)return ENOSPC;
    const uint8_t *p=data;uint8_t *q=dest;
    uint32_t srcw=word_bytes(packing),dstw=word_bytes(target);
    /* Bounds and canonical extension were checked before the first write.
     * Counts/header are positive; their sign extension is necessarily zero.
     * All arithmetic is unsigned, including bit 23; no signed-shift UB. */
    for(uint32_t i=0;i<bytes/srcw;i++) {
        uint32_t v=word(p+i*srcw,packing);uint8_t *d=q+i*dstw;
        if(dstw==3){d[0]=(uint8_t)(v>>16);d[1]=(uint8_t)(v>>8);d[2]=(uint8_t)v;}
        else {d[0]=(uint8_t)v;d[1]=(uint8_t)(v>>8);d[2]=(uint8_t)(v>>16);
              d[3]=(v&0x800000u)?0xffu:0u;}
    }
    *written_bytes=n;return 0;
}
