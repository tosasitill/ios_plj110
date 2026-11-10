/* OPPO family-2 ReZ_SCALE and tfa98xx_adsp_send_calib_values byte layout.
 * Deliberately no factory-value fallback, calibration or parameter I/O. */
#if defined(KERNEL)
#include <sys/errno.h>
#else
#include <errno.h>
#endif
#include "x9_tfa_re25.h"
static uint32_t re_word_bytes(uint32_t packing)
{return packing==X9TFABE24?3u:packing==X9TFALE32?4u:0u;}
static int re_span(const void *p,uint32_t n)
{return p && n && (uintptr_t)p<=UINTPTR_MAX-n;}
static int re_overlap(const void *a,uint32_t an,const void *b,uint32_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=y?y-x<an:x-y<bn;
}
static uint32_t re_q16(uint32_t mohms)
{return (uint32_t)(((uint64_t)mohms*X9_TFA_RE25_SCALE)/1000u);}
static int re_limits(const struct x9_tfa_re25_limits *limits)
{
    if(!re_span(limits,sizeof(*limits)))return EINVAL;
    for(unsigned i=0;i<2;i++)
        if(!limits->minimum_mohm[i] ||
           limits->minimum_mohm[i]>limits->maximum_mohm[i] ||
           limits->maximum_mohm[i]>65534u)return ERANGE;
    return 0;
}
static void re_put(uint8_t *p,uint32_t packing,uint32_t word)
{
    if(packing==X9TFABE24) {
        p[0]=(uint8_t)(word>>16);p[1]=(uint8_t)(word>>8);p[2]=(uint8_t)word;
    } else {
        p[0]=(uint8_t)word;p[1]=(uint8_t)(word>>8);p[2]=(uint8_t)(word>>16);
        p[3]=word&0x800000u?0xffu:0u;
    }
}
int x9_tfa_re25_encode(const uint32_t mohms[2],
    const struct x9_tfa_re25_limits *limits,uint32_t packing,uint32_t form,
    uint8_t selector,void *dest,uint32_t capacity,uint32_t *written_bytes)
{
    uint32_t w=re_word_bytes(packing);
    if(!w || (form!=X9TFARe25Command && form!=X9TFARe25Batch) ||
       !re_span(mohms,2u*sizeof(*mohms)) || !re_span(limits,sizeof(*limits)) ||
       !re_span(dest,capacity) || !re_span(written_bytes,sizeof(*written_bytes)) ||
       re_overlap(dest,capacity,mohms,2u*sizeof(*mohms)) ||
       re_overlap(dest,capacity,limits,sizeof(*limits)) ||
       re_overlap(written_bytes,sizeof(*written_bytes),mohms,2u*sizeof(*mohms)) ||
       re_overlap(written_bytes,sizeof(*written_bytes),limits,sizeof(*limits)) ||
       re_overlap(dest,capacity,written_bytes,sizeof(*written_bytes)))return EINVAL;
    int e=re_limits(limits);if(e)return e;
    uint32_t q16[2];
    for(unsigned i=0;i<2;i++) {
        if(mohms[i]<limits->minimum_mohm[i] || mohms[i]>limits->maximum_mohm[i])return ERANGE;
        q16[i]=re_q16(mohms[i]);
    }
    uint32_t words=form==X9TFARe25Batch?6u:3u,n=words*w,off=0;
    if(capacity<n)return ENOSPC;
    /* Every argument, both channels and the complete size checked first. */
    uint8_t *p=dest;
    if(form==X9TFARe25Batch) {
        re_put(p,packing,X9_TFA_MULTIMSG_COMMAND);re_put(p+w,packing,3u);off=2u*w;
    }
    re_put(p+off,packing,((uint32_t)selector<<16)|0x8105u);
    re_put(p+off+w,packing,q16[0]);re_put(p+off+2u*w,packing,q16[1]);
    if(form==X9TFARe25Batch)re_put(p+off+3u*w,packing,0);
    *written_bytes=n;return 0;
}
int x9_tfa_re25_decode(const void *data,uint32_t bytes,uint32_t packing,
    const struct x9_tfa_re25_limits *limits,struct x9_tfa_re25_pair *out)
{
    uint32_t w=re_word_bytes(packing);
    if(!w || bytes!=2u*w || !re_span(data,bytes) ||
       !re_span(limits,sizeof(*limits)) || !re_span(out,sizeof(*out)) ||
       re_overlap(data,bytes,out,sizeof(*out)) ||
       re_overlap(limits,sizeof(*limits),out,sizeof(*out)))return EINVAL;
    int e=re_limits(limits);if(!e)e=x9_tfa_words_scan(data,bytes,packing);if(e)return e;
    const uint8_t *p=data;
    struct x9_tfa_re25_pair pair={X9_TFA_RE25_VERSION,packing,{0,0},{0,0}};
    for(unsigned i=0;i<2;i++) {
        const uint8_t *v=p+i*w;
        uint32_t q=w==3?((uint32_t)v[0]<<16)|((uint32_t)v[1]<<8)|v[2]:
                        ((uint32_t)v[2]<<16)|((uint32_t)v[1]<<8)|v[0];
        if(!q || (q&0x800000u) || q<re_q16(limits->minimum_mohm[i]) ||
           q>re_q16(limits->maximum_mohm[i]))return ERANGE;
        pair.q16[i]=q;pair.mohm[i]=(uint32_t)(((uint64_t)q*1000u)/X9_TFA_RE25_SCALE);
    }
    *out=pair;return 0;
}
int x9_tfa_re25_read_request(uint32_t packing,uint32_t form,uint8_t selector,
    void *dest,uint32_t capacity,uint32_t *written_bytes)
{
    uint32_t w=re_word_bytes(packing);
    if(!w || (form!=X9TFARe25Command && form!=X9TFARe25Batch) ||
       !re_span(dest,capacity) || !re_span(written_bytes,sizeof(*written_bytes)) ||
       re_overlap(dest,capacity,written_bytes,sizeof(*written_bytes)))return EINVAL;
    uint32_t n=(form==X9TFARe25Batch?4u:1u)*w,off=0;
    if(capacity<n)return ENOSPC;
    uint8_t *p=dest;
    if(form==X9TFARe25Batch) {
        re_put(p,packing,X9_TFA_MULTIMSG_COMMAND);re_put(p+w,packing,1u);off=2u*w;
    }
    re_put(p+off,packing,((uint32_t)selector<<16)|0x8185u);
    if(form==X9TFARe25Batch)re_put(p+off+w,packing,0);
    *written_bytes=n;return 0;
}
