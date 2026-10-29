/* Bounded named-node property lookup for device handoff verification. */
#include "fdt.h"
#include <stddef.h>

static uint32_t be(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int eq(const char *a,const char *b) {while(*a&&*a==*b){a++;b++;}return *a==*b;}
int fdt_property(uintptr_t address,const char *wanted_path,const char *wanted_key,const uint8_t **value,size_t *length)
{
 if(!address||(address&3)||!wanted_path||!wanted_key||!value||!length)return 0;
 const uint8_t *b=(const uint8_t *)address;if(be(b)!=0xd00dfeed)return 0;
 uint32_t total=be(b+4),start=be(b+8),strings=be(b+12),ss=be(b+32),sz=be(b+36);
 if(total<40||total>0x400000||start>total||sz>total-start||strings>total||ss>total-strings)return 0;
 size_t at=start,end=(size_t)start+sz,used=0,parents[32];unsigned depth=0,matches=0;
 char path[256]={0};const uint8_t *found=0;size_t found_size=0;
 while(end-at>=4) {
  unsigned t=be(b+at);at+=4;
  if(t==1) {
   if(depth==32)return 0;
   size_t first=at;while(at<end&&b[at])at++;if(at==end)return 0;
   parents[depth]=used;
   if(depth) {
    size_t n=at-first;if(used+1+n>=sizeof(path))return 0;
    path[used++]='/';for(size_t i=0;i<n;i++)path[used++]=(char)b[first+i];path[used]=0;
   } else if(at!=first)return 0;
   depth++;at=(at+4)&~(size_t)3;
  } else if(t==2) {
   if(!depth)return 0;used=parents[--depth];path[used]=0;
  } else if(t==3) {
   if(!depth||end-at<8)return 0;
   uint32_t n=be(b+at),offset=be(b+at+4);at+=8;
   if(n>end-at||offset>=ss)return 0;
   const char *key=(const char *)b+strings+offset;
   size_t limit=ss-offset,i=0;while(i<limit&&key[i])i++;if(i==limit)return 0;
   if(eq(path,wanted_path)&&eq(key,wanted_key)) {found=b+at;found_size=n;matches++;}
   at=(at+n+3)&~(size_t)3;
  } else if(t==4)continue;
  else if(t==9&&!depth) {
   if(matches!=1)return 0;*value=found;*length=found_size;return 1;
  } else return 0;
  if(at>end)return 0;
 }
 return 0;
}
