/* Matching MD913258e6 / RIL37cef0 / Apple20H392 1f1ad380c.
 * This reports actual SRVCC events; it does NOT migrate call identities,
 * select a RAT/profile, turn on speech or synthesize a completed handover. */
#ifndef X9_CALL_HANDOVER_H
#define X9_CALL_HANDOVER_H
#include "x9_call_wire.h"

struct x9cw_handover {unsigned sim,native_state,ios_state;};
/* MD event jump9298f81c: EVENT3 -> tag115/u32, sourced from u8.
 * RIL37cfd4..37d020 passes values0..3 unchanged to notifySrvccState.
 * Android radio SrvccState: STARTED0/COMPLETED1/FAILED2/CANCELED3.
 * Apple193a040: START1/FAIL2/COMPLETE3/CANCEL4. Not numeric passthrough.
 * URC19f898..19f8a4 uses the same low-SIM converter as CMD19f978.
 * Return0 for other event classes, -1 for malformed SRVCC, 1 for SRVCC. */
static inline int x9cw_handover(const uint8_t *p,unsigned n,struct x9cw_handover *out){
 uint32_t event,state;
 if(!out||!x9cw_frame(p,n,0x4902)||!x9cw_word(p,n,0x100,&event))return -1;
 if(event!=3)return 0;
 unsigned low=p[8]&15;
 if((low!=1&&low!=2)||n!=32||!x9cw_word(p,n,0x115,&state)||state>3)return -1;
 for(unsigned at=16;at<n;at+=8){
  unsigned tag=x9cw_u16(p+at);
  if((tag!=0x100&&tag!=0x115)||x9cw_u16(p+at+2)!=4)return -1;
 }
 static const uint8_t mapping[4]={1,3,2,4};
 out->sim=low-1;out->native_state=state;out->ios_state=mapping[state];return 1;
}
/* Apple handler19 constructs indication54, consumer1ac325c reads tag1,
 * parseV192ad18 consumes one byte. No QMI Result TLV in an indication. */
static inline unsigned x9cw_handover_indication(uint8_t *out,unsigned room,unsigned state){
 if(!out||room<11||state<1||state>4)return 0;
 x9cw_zero(out,11);out[0]=4;out[3]=0x54;out[5]=4;
 out[7]=1;out[8]=1;out[10]=(uint8_t)state;return 11;
}
#endif
