/* Bounded RAM coverage against this boot's FDT. No writes to target RAM.
 * Accept adjacent/overlapping memory tuples, reject every reservation overlap. */
#include "fdt.h"
#include <stddef.h>
#include "x9_ram_ranges.h"
#define MAX_RAM_TUPLES 128
static struct { const char *reason;uint64_t lo,size,conflict,conflict_size;
                uint64_t memory[MAX_RAM_TUPLES][2];unsigned count; } audit;
static int fail(const char *reason,uint64_t a,uint64_t n)
{ audit.reason=reason;audit.conflict=a;audit.conflict_size=n;return 0; }
const char *fdt_ram_failure(uint64_t *lo,uint64_t *size,uint64_t *conflict,uint64_t *n)
{ *lo=audit.lo;*size=audit.size;*conflict=audit.conflict;*n=audit.conflict_size;return audit.reason; }
int fdt_ram_memory_tuple(unsigned i,uint64_t *a,uint64_t *n)
{ if(i>=audit.count)return 0;*a=audit.memory[i][0];*n=audit.memory[i][1];return 1; }
static uint32_t be(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be(p)<<32 | be(p+4); }
static int equal(const uint8_t *p,size_t n,const char *s)
{
 for(size_t i=0;i<n;i++){if(p[i]!=(uint8_t)s[i])return 0;if(!s[i])return 1;}
 return 0;
}
static int hit(uint64_t base,uint64_t size,uint64_t lo,uint64_t hi)
{ return size && (base>UINT64_MAX-size || (base<hi && lo<base+size)); }
static int range_available(uintptr_t address,uint64_t lo,uint64_t size)
{
 audit.reason="none";audit.lo=lo;audit.size=size;audit.conflict=0;audit.conflict_size=0;audit.count=0;
 if(!address || (address&3) || !size || lo>UINT64_MAX-size)return fail("invalid query",lo,size);
 const uint8_t *f=(const uint8_t *)address;
 if(be(f)!=0xd00dfeed)return fail("bad FDT magic",0,0);
 uint32_t total=be(f+4),start=be(f+8),strings=be(f+12),reserve=be(f+16);
 uint32_t strsize=be(f+32),structsize=be(f+36);
 if(total<40 || total>0x400000 || start>total || structsize>total-start ||
    strings>total || strsize>total-strings || reserve>total || (reserve&7))
  return fail("invalid FDT header",total,reserve);
 uint64_t hi=lo+size;int terminated=0;
 for(unsigned i=0;i<256 && total-reserve>=16;i++,reserve+=16){
  uint64_t a=be64(f+reserve),n=be64(f+reserve+8);
  if(!a&&!n){terminated=1;break;}
  if(hit(a,n,lo,hi))return fail("reservation-map overlap",a,n);
 }
 if(!terminated)return fail("unterminated reservation map",0,0);
 struct level{const uint8_t *reg;uint32_t len;unsigned memory,reserved;}stack[32];
 unsigned depth=0;int finished=0;size_t at=start,end=(size_t)start+structsize;
 while(at<end && end-at>=4){
  uint32_t token=be(f+at);at+=4;
  if(token==1){
   if(depth==32)return fail("FDT nesting limit",at,depth);
   size_t name=at;while(at<end&&f[at])at++;
   if(at==end)return fail("unterminated node name",name,0);
   unsigned reserved=(depth&&stack[depth-1].reserved)||
    (depth==1&&equal(f+name,at-name+1,"reserved-memory"));
   stack[depth++]=(struct level){0,0,0,reserved};at=(at+4)&~(size_t)3;
  }else if(token==2){
   if(!depth)return fail("unmatched end node",at,0);
   struct level *s=&stack[--depth];
   if(s->reg&&(s->memory||s->reserved)){
    if(s->len%16)return fail("reg is not 64-bit address/size pairs",s->len,0);
    for(uint32_t i=0;i<s->len;i+=16){
     uint64_t a=be64(s->reg+i),n=be64(s->reg+i+8);
     if(a>UINT64_MAX-n)return fail("reg overflow",a,n);
     if(s->reserved&&hit(a,n,lo,hi))return fail("reserved-memory overlap",a,n);
     if(s->memory&&n){
      if(audit.count==MAX_RAM_TUPLES)return fail("memory tuple limit",a,n);
      audit.memory[audit.count][0]=a;audit.memory[audit.count++][1]=n;
     }
    }
   }
  }else if(token==3){
   if(!depth||end-at<8)return fail("truncated property header",at,0);
   uint32_t n=be(f+at),offset=be(f+at+4);at+=8;
   if(n>end-at||offset>=strsize)return fail("property outside FDT",at,n);
   const uint8_t *key=f+strings+offset;size_t keysize=strsize-offset;
   if(equal(key,keysize,"reg")){stack[depth-1].reg=f+at;stack[depth-1].len=n;}
   if(equal(key,keysize,"device_type")&&equal(f+at,n,"memory"))stack[depth-1].memory=1;
   if((depth==1||(depth==2&&stack[depth-1].reserved))&&
      (equal(key,keysize,"#address-cells")||equal(key,keysize,"#size-cells")))
    if(n!=4||be(f+at)!=2)return fail("unsupported address/size cells",at,n==4?be(f+at):n);
   at=(at+n+3)&~(size_t)3;
  }else if(token==4)continue;
  else if(token==9&&!depth){finished=1;break;}
  else return fail("invalid structure token",at-4,token);
  if(at>end)return fail("structure outside FDT",at,end);
 }
 if(!finished)return fail("missing FDT end token",at,0);
 /* Extend coverage regardless of tuple order. Adjacent is valid; even a
  * one-byte gap fails. At most count extensions are needed. */
 uint64_t cursor=lo;
 for(unsigned pass=0;pass<audit.count && cursor<hi;pass++){
  uint64_t next=cursor;
  for(unsigned i=0;i<audit.count;i++){
   uint64_t a=audit.memory[i][0],z=a+audit.memory[i][1];
   if(a<=cursor&&z>next)next=z;
  }
  if(next==cursor)return fail("memory coverage gap",cursor,hi-cursor);
  cursor=next;
 }
 return cursor>=hi ? 1 : fail("memory coverage incomplete",cursor,hi-cursor);
}
int fdt_ram_available(uintptr_t address,uint64_t lo,uint64_t size)
{
 if(lo==X9_RAM_LO && size==X9_RAM_SPAN){
  for(unsigned i=0;i<X9_RAM_NR;i++)
   if(!range_available(address,x9_ram_ranges[i][0],x9_ram_ranges[i][1]-x9_ram_ranges[i][0]))return 0;
  return 1;
 }
 return range_available(address,lo,size);
}

/* Effective map is always a subset of the captured allowlist. Split only on
 * positively identified live reservations; malformed/uncovered RAM fails. */
static uint64_t live[128][2],live_bytes;
static unsigned live_count;
uint64_t fdt_ram_live_bytes(void){return live_bytes;}
int fdt_ram_live_tuple(unsigned i,uint64_t *a,uint64_t *z)
{if(i>=live_count)return 0;*a=live[i][0];*z=live[i][1];return 1;}
int fdt_ram_build_live(uintptr_t fdt)
{
 live_count=X9_RAM_NR;live_bytes=0;
 for(unsigned i=0;i<live_count;i++){live[i][0]=x9_ram_ranges[i][0];live[i][1]=x9_ram_ranges[i][1];}
 unsigned i=0,attempts=0;
 while(i<live_count){
  if(++attempts>512)return fail("live map iteration limit",0,0);
  uint64_t lo=live[i][0],hi=live[i][1];
  if(range_available(fdt,lo,hi-lo)){i++;continue;}
  if(!equal((const uint8_t *)audit.reason,64,"reserved-memory overlap") &&
     !equal((const uint8_t *)audit.reason,64,"reservation-map overlap"))return 0;
  uint64_t a=audit.conflict,n=audit.conflict_size;
  if(a>UINT64_MAX-n || a+n>UINT64_MAX-0x3fff)return fail("reservation rounding overflow",a,n);
  uint64_t end=(a+n+0x3fff)&~0x3fffULL; a&=~0x3fffULL;
  if(a<=lo && end>=hi){
   for(unsigned j=i+1;j<live_count;j++){live[j-1][0]=live[j][0];live[j-1][1]=live[j][1];}
   live_count--;
  }else if(a<=lo){live[i][0]=end;}
  else if(end>=hi){live[i][1]=a;}
  else{
   if(live_count==128)return fail("live map split limit",a,n);
   for(unsigned j=live_count;j>i+1;j--){live[j][0]=live[j-1][0];live[j][1]=live[j-1][1];}
   live[i][1]=a;live[i+1][0]=end;live[i+1][1]=hi;live_count++;
  }
 }
 for(i=0;i<live_count;i++)live_bytes+=live[i][1]-live[i][0];
 if(!live_count || live[0][0]!=X9_RAM_LO || live[0][1]!=x9_ram_ranges[0][1])
  return fail("live map changed bootstrap extent",0,0);
 if(live_bytes<(8ULL<<30))return fail("live map below 8 GiB",live_bytes,8ULL<<30);
 return 1;
}
/* Bounded traversal of the copied Apple DT. Patch only one chosen property,
 * after validating the entire tree. All fields are little endian. */
struct map_walk{uint8_t *p;size_t size,at,found;unsigned nodes,matches;};
static uint32_t le32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int map_node(struct map_walk *w,unsigned depth)
{
 if(depth>32 || ++w->nodes>1024 || w->size-w->at<8)return -1;
 uint32_t props=le32(w->p+w->at),children=le32(w->p+w->at+4);w->at+=8;
 if(props>1024||children>1024)return -1;
 unsigned chosen=0;
 for(unsigned i=0;i<props;i++){
  if(w->size-w->at<36)return -1;
  uint8_t *key=w->p+w->at;uint32_t n=le32(key+32)&0x7fffffff;w->at+=36;
  size_t padded=((size_t)n+3)&~(size_t)3;
  if(padded>w->size-w->at)return -1;
  if(equal(key,32,"name"))chosen=depth==1&&equal(w->p+w->at,n,"chosen");
  if(equal(key,32,"x9-ram-map")&&chosen){if(n!=2064)return -1;w->found=w->at;w->matches++;}
  w->at+=padded;
 }
 for(unsigned i=0;i<children;i++)if(map_node(w,depth+1))return -1;
 return 0;
}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(i*8));}
int fdt_ram_patch_adt(void *adt,size_t size)
{
 struct map_walk w={(uint8_t *)adt,size,0,0,0,0};
 if(!live_count || live_bytes<(8ULL<<30) || map_node(&w,0)||w.at!=size||w.matches!=1)return -1;
 uint8_t *p=w.p+w.found;
 for(unsigned i=0;i<2064;i++)p[i]=0;
 /* header: u32 version=1, u32 count, u64 usable bytes. */
 p[0]=1;p[4]=(uint8_t)live_count;put64(p+8,live_bytes);
 for(unsigned i=0;i<live_count;i++){put64(p+16+i*16,live[i][0]);put64(p+24+i*16,live[i][1]);}
 return 0;
}
