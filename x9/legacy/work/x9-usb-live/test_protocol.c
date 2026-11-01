#include "log_protocol.h"
#include <assert.h>
#include <string.h>
struct ring {char data[128];uint64_t total;};
static uint64_t pos(void *p){return ((struct ring*)p)->total;}
static uint32_t copy(void *p,uint64_t *cursor,char *dst,uint32_t max){struct ring*r=p;if(*cursor<r->total-(*cursor>r->total?0:(r->total>sizeof r->data?sizeof r->data:r->total)))*cursor=r->total-sizeof r->data;uint64_t n=r->total-*cursor;if(n>max)n=max;for(uint32_t i=0;i<n;i++)dst[i]=r->data[(*cursor+i)%sizeof r->data];*cursor+=n;return n;}
static uint32_t get(const uint8_t*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int main(void){struct ring r={0};for(unsigned i=0;i<200;i++)r.data[i%128]=(char)i;r.total=200;struct x9log_reader reader={&r,pos,copy};uint8_t b[64],dup[64];
 assert(x9log_read(&reader,0,b,7)==64);assert(get(b)==X9LOG_MAGIC&&get(b+4)==7&&get(b+8)==72&&get(b+12)==112&&get(b+16)==200&&get(b+20)==X9LOG_GAP);assert((unsigned char)b[24]==72);
 assert(x9log_read(&reader,0,dup,7)==64&&!memcmp(b,dup,64));assert(x9log_read(&reader,190,b,7)==34&&get(b+8)==190&&get(b+12)==200);assert(x9log_read(&reader,300,b,7)==24&&get(b+20)==X9LOG_AHEAD&&get(b+12)==200);assert(x9log_read(&reader,0,b,0)<0);r.total=(uint64_t)UINT32_MAX+1;assert(x9log_read(&reader,0,b,1)<0);return 0;}
