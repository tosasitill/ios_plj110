/* Confirm a proposed root/kernel window against the FDT of this boot.
 * Both root and reserved-memory buses must use the captured 64-bit cells.
 */
#include "fdt.h"
#include <stddef.h>

static uint32_t be(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be(p)<<32 | be(p+4); }
static int equal(const uint8_t *p,size_t n,const char *s)
{
    for (size_t i=0;i<n;i++) {if(p[i]!=(uint8_t)s[i])return 0;if(!s[i])return 1;}
    return 0;
}
static int hit(uint64_t base,uint64_t size,uint64_t lo,uint64_t hi)
{ return size && (base>UINT64_MAX-size || (base<hi && lo<base+size)); }

int fdt_ram_available(uintptr_t address,uint64_t lo,uint64_t size)
{
    if (!address || (address&3) || !size || lo>UINT64_MAX-size) return 0;
    const uint8_t *f=(const uint8_t *)address;
    if (be(f)!=0xd00dfeed) return 0;
    uint32_t total=be(f+4),start=be(f+8),strings=be(f+12),reserve=be(f+16);
    uint32_t strsize=be(f+32),structsize=be(f+36);
    if (total<40 || total>0x400000 || start>total || structsize>total-start ||
        strings>total || strsize>total-strings || reserve>total || (reserve&7)) return 0;
    int terminated=0;
    uint64_t hi=lo+size;
    for (unsigned i=0;i<256 && total-reserve>=16;i++,reserve+=16) {
        uint64_t a=be64(f+reserve),n=be64(f+reserve+8);
        if (!a && !n) {terminated=1;break;}
        if (hit(a,n,lo,hi)) return 0;
    }
    if (!terminated) return 0;
    struct level { const uint8_t *reg;uint32_t len;unsigned memory,reserved; } stack[32];
    unsigned depth=0;int memory_ok=0,finished=0;
    size_t at=start,end=(size_t)start+structsize;
    while (at<end && end-at>=4) {
        uint32_t token=be(f+at);at+=4;
        if (token==1) {
            if (depth==32) return 0;
            size_t name=at;
            while (at<end && f[at])at++;
            if (at==end) return 0;
            unsigned reserved=(depth && stack[depth-1].reserved) ||
                              (depth==1 && equal(f+name,at-name+1,"reserved-memory"));
            stack[depth++]=(struct level){0,0,0,reserved};
            at=(at+4)&~(size_t)3;
        } else if (token==2) {
            if (!depth) return 0;
            struct level *s=&stack[--depth];
            if (s->reg && (s->memory || s->reserved)) {
                if (s->len%16) return 0;
                for (uint32_t i=0;i<s->len;i+=16) {
                    uint64_t a=be64(s->reg+i),n=be64(s->reg+i+8);
                    if (a>UINT64_MAX-n) return 0;
                    if (s->reserved && hit(a,n,lo,hi)) return 0;
                    if (s->memory && a<=lo && hi<=a+n) memory_ok=1;
                }
            }
        } else if (token==3) {
            if (!depth || end-at<8) return 0;
            uint32_t n=be(f+at),offset=be(f+at+4);at+=8;
            if (n>end-at || offset>=strsize) return 0;
            const uint8_t *key=f+strings+offset;
            size_t keysize=strsize-offset;
            struct level *s=&stack[depth-1];
            if (equal(key,keysize,"reg")) {s->reg=f+at;s->len=n;}
            if (equal(key,keysize,"device_type") && equal(f+at,n,"memory")) s->memory=1;
            if ((depth==1 || (depth==2 && s->reserved)) &&
                (equal(key,keysize,"#address-cells") || equal(key,keysize,"#size-cells"))) {
                if (n!=4 || be(f+at)!=2) return 0;
            }
            at=(at+n+3)&~(size_t)3;
        } else if (token==4) {
            continue;
        } else if (token==9 && !depth) {finished=1;break;}
        else return 0;
        if (at>end) return 0;
    }
    return finished && memory_ok;
}
