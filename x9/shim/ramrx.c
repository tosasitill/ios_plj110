/* Transport-independent bounded RAM receiver. No MMIO or partition access. */
#include "ramrx.h"

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p+4)<<32; }
static void put32(uint8_t *p, uint32_t n) { for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(n>>(8*i)); }
static void put64(uint8_t *p, uint64_t n) { put32(p,(uint32_t)n); put32(p+4,(uint32_t)(n>>32)); }

uint32_t ramrx_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc=~crc;
    while (n--) {
        crc^=*p++;
        for (unsigned i=0;i<8;i++) crc=(crc>>1)^((0U-(crc&1))&0xedb88320U);
    }
    return ~crc;
}

int ramrx_lz4(const uint8_t *src, size_t size, uint8_t *dst, size_t wanted)
{
    size_t ip=0,op=0;
    while (ip<size) {
        unsigned token=src[ip++];
        size_t literal=token>>4,match=(token&15)+4;
        if (literal==15) {
            unsigned extra;
            do { if (ip==size) return -1; extra=src[ip++];
                 if (literal>wanted || extra>wanted-literal) return -1;
                 literal+=extra; } while (extra==255);
        }
        if (literal>size-ip || literal>wanted-op) return -1;
        for (size_t i=0;i<literal;i++) dst[op++]=src[ip++];
        if (ip==size) return op==wanted ? 0 : -1;
        if (size-ip<2) return -1;
        unsigned offset=src[ip] | (unsigned)src[ip+1]<<8; ip+=2;
        if (!offset || offset>op) return -1;
        if ((token&15)==15) {
            unsigned extra;
            do { if (ip==size) return -1; extra=src[ip++];
                 if (match>wanted || extra>wanted-match) return -1;
                 match+=extra; } while (extra==255);
        }
        if (match>wanted-op) return -1;
        for (size_t i=0;i<match;i++) { dst[op]=dst[op-offset]; op++; }
    }
    return op==wanted ? 0 : -1;
}

void ramrx_init(struct ramrx *s, void *destination, uint64_t capacity)
{
    s->destination=destination; s->capacity=capacity;
    s->total=s->received=0; s->expected_crc=s->crc=s->state=s->error=0;
}
static int fail(struct ramrx *s, uint32_t why) { s->error=why; return -1; }

int ramrx_command(struct ramrx *s, unsigned command, const uint8_t *p, size_t n)
{
    if (command==RAMRX_ABORT && !n) {
        ramrx_init(s,s->destination,s->capacity); return 0;
    }
    if (command==RAMRX_BEGIN) {
        static const uint8_t magic[8]={'X','9','R','A','M','R','X','1'};
        if (n!=24) return fail(s,1);
        for (unsigned i=0;i<8;i++) if (p[i]!=magic[i]) return fail(s,1);
        uint64_t total=le64(p+8);
        if (!total || total>s->capacity || le32(p+20)!=1) return fail(s,2);
        s->total=total; s->received=0; s->crc=0;
        s->expected_crc=le32(p+16); s->state=1; s->error=0; return 0;
    }
    if (command==RAMRX_DATA) {
        if (s->state!=1 || n<24 || n>RAMRX_FRAME_MAX) return fail(s,3);
        uint64_t offset=le64(p);
        uint32_t raw=le32(p+8),codec=le32(p+12),crc=le32(p+16),stored=le32(p+20);
        if (offset!=s->received || !raw || raw>RAMRX_BLOCK || stored!=n-24 ||
            raw>s->total-s->received) return fail(s,4);
        if (codec==0 && stored==raw) {
            for (unsigned i=0;i<raw;i++) s->scratch[i]=p[24+i];
        } else if (codec==1) {
            if (ramrx_lz4(p+24,stored,s->scratch,raw)) return fail(s,5);
        } else if (codec==2 && !stored) {
            for (unsigned i=0;i<raw;i++) s->scratch[i]=0;
        } else return fail(s,6);
        if (ramrx_crc32(0,s->scratch,raw)!=crc) return fail(s,7);
        /* A failed frame has not changed destination or the accepted offset. */
        volatile uint8_t *dst=s->destination+s->received;
        for (unsigned i=0;i<raw;i++) dst[i]=s->scratch[i];
        s->crc=ramrx_crc32(s->crc,s->scratch,raw);
        s->received+=raw; s->error=0; return 0;
    }
    if (command==RAMRX_COMMIT && !n) {
        if (s->state!=1 || s->received!=s->total || s->crc!=s->expected_crc)
            return fail(s,8);
        s->state=2; s->error=0; return 0;
    }
    if (command==RAMRX_BOOT && !n) {
        if (s->state!=2) return fail(s,9);
        s->state=3; s->error=0; return 0;
    }
    return fail(s,10);
}

void ramrx_status(const struct ramrx *s, uint8_t out[48])
{
    const uint8_t magic[8]={'X','9','R','A','M','R','X','1'};
    for (unsigned i=0;i<8;i++) out[i]=magic[i];
    put32(out+8,s->state); put32(out+12,s->error);
    put64(out+16,s->received); put64(out+24,s->total); put64(out+32,s->capacity);
    put32(out+40,s->crc); put32(out+44,RAMRX_BLOCK);
}
