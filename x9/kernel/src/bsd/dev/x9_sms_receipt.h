/* Structural admission for retained WMS storage terminal SDUs. This checks
 * framing/result only; it does not validate the operation payload or prove
 * modem, SIM, host-consumer, or durable-storage completion. */
#ifndef X9_SMS_RECEIPT_H
#define X9_SMS_RECEIPT_H
#include <stdint.h>
static inline unsigned x9sr_u16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static inline int x9sr_storage_response(const uint8_t *p,unsigned n){
 if(!p||n<14||n>1536||p[0]!=2||x9sr_u16(p+5)!=n-7)return 0;
 unsigned id=x9sr_u16(p+3);if(id!=0x31&&id!=0x22&&id!=0x23)return 0;
 unsigned at=7;int result_seen=0;uint8_t seen[256]={0};
 while(at<n){
  if(n-at<3)return 0;
  unsigned tag=p[at],k=x9sr_u16(p+at+1);if(k>n-at-3||seen[tag])return 0;
  seen[tag]=1;
  if(tag==2){
   if(k!=4||x9sr_u16(p+at+3)>1||
      (!!x9sr_u16(p+at+3))!=(!!x9sr_u16(p+at+5)))return 0;
   result_seen=1;
  }
  at+=3+k;
 }
 return result_seen;
}
#endif
