/* PLJ110 CALL registration, not network/IMS/audio readiness.
 * Android RIL37c458/470 -> RfxBaseHandler1e2828/1e2c00 ->
 * libmipc14228/14510. MD91335654/913358ea confirms each registration.
 * Shared by the real owner and compile-only synthetic vectors. */
#ifndef X9_CALL_INIT_H
#define X9_CALL_INIT_H
#include "x9_call_wire.h"

struct x9cw_voice_register { unsigned present,call_events,handover; };
static inline int x9cw_qmi_register(const uint8_t *p,unsigned n,
                                   struct x9cw_voice_register *out){
 if(!p||!out||n<11||n>15||p[0]||x9cw_u16(p+3)!=3||x9cw_u16(p+5)!=n-7)return 0;
 struct x9cw_voice_register r={0,0,0};
 for(unsigned at=7;at<n;){
  if(n-at<4||x9cw_u16(p+at+1)!=1||p[at+3]>1)return 0;
  unsigned bit=p[at]==0x13?1:p[at]==0x14?2:0;
  if(!bit||(r.present&bit))return 0;
  r.present|=bit;if(bit==1)r.call_events=p[at+3];else r.handover=p[at+3];at+=4;
 }
 *out=r;return 1;
}
/* Only the original handler's two already-consumed events and incoming
 * command. No blanket Android20-URC registration without consumers. */
static inline unsigned x9cw_call_register(uint8_t *out,unsigned room,
                                         unsigned id,unsigned sim,unsigned sims){
 if(!out||room<24||!sims||sims>2||sim>=sims||
    (id!=0x8901&&id!=0x4901&&id!=0x4902))return 0;
 x9cw_header(out,24,id==0x8901?0x30b:0x307,id==0x8901?0xff:1u<<sim,0);
 x9cw_scalar(out+16,0x100,id,2);return 24;
}
static inline int x9cw_call_register_request(const uint8_t *p,unsigned n,unsigned sims){
 if(!p||n!=24||x9cw_u16(p+16)!=0x100||x9cw_u16(p+18)!=2)return 0;
 unsigned id=x9cw_u16(p+20),sim=p[8]==2?1:0;
 uint8_t expected[24];
 if(!x9cw_call_register(expected,sizeof(expected),id,sim,sims))return 0;
 /* Transaction is allocated by the existing sole MD owner. */
 for(unsigned i=0;i<n;i++)if(i!=12&&i!=13&&p[i]!=expected[i])return 0;
 return 1;
}
/* MD success writes ONLY result0/u32; IND failure additionally writes
 * tag100/u8=0 (91335760..76). No descriptor/default success inference. */
static inline int x9cw_call_register_cnf(const uint8_t *p,unsigned n,unsigned request,
                                        uint64_t token,uint32_t *result){
 if(!result||!token||token>65534||(request!=0x307&&request!=0x30b)||
    !x9cw_frame(p,n,request+1)||x9cw_u16(p+12)!=token||
    (n!=24&&n!=32)||x9cw_u16(p+16)||x9cw_u16(p+18)!=4)return 0;
 uint32_t r=x9cw_u32(p+20);
 if(n==32&&(request!=0x307||!r||x9cw_u16(p+24)!=0x100||
             x9cw_u16(p+26)!=1||p[28]))return 0;
 *result=r;return 1;
}
enum {X9_CI_NEW=0,X9_CI_STATUS,X9_CI_EVENT,X9_CI_BASELINE,X9_CI_DONE};
struct x9cw_call_init {
 unsigned stage;uint64_t token,deadline;uint8_t held,registered,baseline_known;
};
/* Advance ONLY on the original owner's timely, complete success. A
 * timeout is terminal for this boot; late confirmation is still consumed.
 * baseline_known is separate from registration and from voice readiness. */
static inline int x9cw_call_init_cnf(struct x9cw_call_init *s,const uint8_t *p,
                                    unsigned n,uint64_t now){
 if(!s||!s->token||(s->stage!=X9_CI_STATUS&&s->stage!=X9_CI_EVENT))return 0;
 if(now>=s->deadline)s->held=1;
 uint32_t r;int good=x9cw_call_register_cnf(p,n,0x307,s->token,&r);
 s->token=0;if(s->held||!good||r){s->held=1;return -1;}
 s->stage++;if(s->stage==X9_CI_BASELINE)s->registered=1;return 1;
}
#endif
