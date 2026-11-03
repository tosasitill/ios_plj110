/* USB control-IN log frames. Cursor is explicitly supplied by host. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define X9LOG_REQ 0x46u
#define X9LOG_FRAME 64u
#define X9LOG_BATCH_REQ 0x47u
#define X9LOG_BATCH_FRAME 4096u
#define X9LOG_DATA 40u
#define X9LOG_MAGIC 0x474f4c58u
#define X9LOG_VERSION 1u
#define X9LOG_GAP 1u
#define X9LOG_AHEAD 2u
struct x9log_reader {
 void *ctx;
 uint64_t (*position)(void *);
 uint32_t (*copy)(void *,uint64_t *,char *,uint32_t);
};
static inline void x9log_u32(uint8_t *p,uint32_t x){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(8*i));}
static inline void x9log_u64(uint8_t *p,uint64_t x){x9log_u32(p,(uint32_t)x);x9log_u32(p+4,(uint32_t)(x>>32));}
/* Caller provides capacity bytes of output; callback copy may advance cursor on overrun.
 * Cursor is 32-bit on wire; total >= 2^32 is not representable, return error.
 * Empty frames carry 24-byte header and do not advance cursor. */
static inline int x9log_read_capacity(const struct x9log_reader *r,uint32_t request,uint8_t *out,uint32_t capacity,uint32_t epoch)
{
 if(!r||!r->position||!r->copy||!out||!epoch||
    (capacity!=X9LOG_FRAME&&capacity!=X9LOG_BATCH_FRAME))return -1;
 uint64_t total=r->position(r->ctx),pos=request;
 if(total>UINT32_MAX)return -1;
 uint32_t flags=0;
 if(pos>total){pos=total;flags|=X9LOG_AHEAD;}
 uint64_t start=pos;uint32_t n=r->copy(r->ctx,&pos,(char*)out+24,capacity-24);
 if(n>capacity-24||pos<n||pos>UINT32_MAX)return -1;
 /* Producer may append while copy executes; refresh the advertised limit. */
 if(pos>total){total=r->position(r->ctx);if(total<pos||total>UINT32_MAX)return -1;}
 uint64_t actual=pos-n;if(actual>start)flags|=X9LOG_GAP;
 x9log_u32(out,X9LOG_MAGIC);x9log_u32(out+4,epoch);x9log_u32(out+8,(uint32_t)actual);x9log_u32(out+12,(uint32_t)pos);x9log_u32(out+16,(uint32_t)total);x9log_u32(out+20,flags);
 return (int)(24+n);
}

/* Legacy request keeps its original 64-byte header/payload contract. */
static inline int x9log_read(const struct x9log_reader *r,uint32_t request,uint8_t out[X9LOG_FRAME],uint32_t epoch)
{return x9log_read_capacity(r,request,out,X9LOG_FRAME,epoch);}
