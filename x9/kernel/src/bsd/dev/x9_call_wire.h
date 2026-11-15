/* Evidence-bounded call wire codec, shared by the iOS adapter and MD owner.
 * No call policy, dial defaults or state mapping here. Incoming routing uses
 * the original RIL's proved CMD -> slot -> response serializer chain.
 * See docs/call-ios-bridge.md for original consumer/producer addresses.
 * Freestanding C/C++; host tests compile this exact implementation. */
#ifndef X9_CALL_WIRE_H
#define X9_CALL_WIRE_H
#include <stdint.h>
#include <stddef.h>

#define X9_CW_MAX 4096u
static inline unsigned x9cw_u16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static inline uint32_t x9cw_u32(const uint8_t *p){return x9cw_u16(p)|((uint32_t)x9cw_u16(p+2)<<16);}
static inline void x9cw_put16(uint8_t *p,unsigned v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static inline void x9cw_put32(uint8_t *p,uint32_t v){x9cw_put16(p,v);x9cw_put16(p+2,v>>16);}
static inline void x9cw_zero(uint8_t *p,unsigned n){for(unsigned i=0;i<n;i++)p[i]=0;}
/* libmipc does not validate received magic, reserved bytes or byte9. Do not
 * invent an RX rejection rule from the AP serializer's zero-filled header. */
static inline int x9cw_frame(const uint8_t *p,unsigned n,unsigned id){
 return p&&n>=16&&n<=X9_CW_MAX&&x9cw_u16(p+14)==n-16&&x9cw_u16(p+10)==id;
}
/* Scalar/string TLVs only. libmipc's zero-length array form is 16 bytes,
 * not eight. It may be framed here, but is never interpreted as a scalar.
 * Duplicate normalized tags are ambiguous, including 0100 + 8100. */
static inline int x9cw_tlv(const uint8_t *p,unsigned n,unsigned wanted,
                          const uint8_t **value,unsigned *length){
 if(!p||n<16||n>X9_CW_MAX||!value||!length)return 0;
 *value=NULL;*length=0;
 for(unsigned at=16;at<n;){
  if(n-at<4)return 0;
  unsigned raw=x9cw_u16(p+at),k=x9cw_u16(p+at+2),size=k?((k+11)&~7u):16;
  if(size>n-at)return 0;
  for(unsigned prev=16;prev<at;){
   if((x9cw_u16(p+prev)&0x7fff)==(raw&0x7fff))return 0;
   unsigned old=x9cw_u16(p+prev+2);prev+=old?((old+11)&~7u):16;
  }
  if(raw==wanted){*value=p+at+4;*length=k;}
  at+=size;
 }
 return 1;
}
static inline int x9cw_word(const uint8_t *p,unsigned n,unsigned tag,uint32_t *out){
 const uint8_t *v;unsigned k;
 if(!out||!x9cw_tlv(p,n,tag,&v,&k)||!v||k!=4)return 0;
 *out=x9cw_u32(v);return 1;
}
struct x9cw_incoming {
 uint32_t call_id,toa,sequence,mode;
 uint16_t txid;uint8_t mask,response_mask,sim,evoltesi_flow;
 const uint8_t *number;unsigned number_length; /* includes NUL; never log */
};
static inline int x9cw_incoming(const uint8_t *p,unsigned n,struct x9cw_incoming *out){
 const uint8_t *v;unsigned k;struct x9cw_incoming c;
 if(!out||!x9cw_frame(p,n,0x8901))return 0;
 /* RfxMclMessage::obtainMipcCmd 19f978..980 calls RfxMipcUtils
  * 1ada10: mask & 0xf, then table129c50. RSP382464..47c supplies the
  * handler's slot to obtainMipcData; 1c0814..824 converts it back via
  * 1ad9e0/table129c44 and preserves ONLY the original txid. Confirmed
  * live SIM2: CMD22 -> slot1 -> RSP02. Do not route high-only masks
  * using Android's fallback slot0; reject absent/ambiguous low selectors.
  * This device's two slots use low1/2; no SIM3/4 or fanout admission. */
 unsigned route=p[8]&0x0f;
 if(route!=1&&route!=2)return 0;
 if(!x9cw_word(p,n,0x100,&c.call_id)||!c.call_id||
    !x9cw_word(p,n,0x102,&c.toa)||!x9cw_word(p,n,0x103,&c.sequence)||
    !x9cw_word(p,n,0x104,&c.mode)||
    !x9cw_tlv(p,n,0x8101,&v,&k)||!v||!k||k>128||v[k-1])return 0;
 for(unsigned i=0;i+1<k;i++)if(!v[i])return 0;
 c.number=v;c.number_length=k;
 if(!x9cw_tlv(p,n,0x105,&v,&k)||!v||k!=1)return 0;
 c.evoltesi_flow=v[0];c.mask=p[8];c.response_mask=(uint8_t)route;
 c.sim=(uint8_t)(route==2);c.txid=(uint16_t)x9cw_u16(p+12);
 /* Only this producer's six fields. Unknown variants require new evidence. */
 for(unsigned at=16;at<n;){
  unsigned tag=x9cw_u16(p+at);
  if(tag!=0x100&&tag!=0x8101&&tag!=0x102&&tag!=0x103&&tag!=0x104&&tag!=0x105)return 0;
  at+=(x9cw_u16(p+at+2)+11)&~7u;
 }
 *out=c;return 1;
}
static inline void x9cw_header(uint8_t *p,unsigned n,unsigned id,unsigned mask,unsigned txid){
 x9cw_zero(p,n);p[0]=0x84;p[1]=0x19;p[2]=0x54;p[3]=0x24;p[8]=(uint8_t)mask;
 x9cw_put16(p+10,id);x9cw_put16(p+12,txid);x9cw_put16(p+14,n-16);
}
static inline void x9cw_scalar(uint8_t *p,unsigned tag,uint32_t v,unsigned bytes){
 x9cw_put16(p,tag);x9cw_put16(p+2,bytes);
 for(unsigned i=0;i<bytes;i++)p[4+i]=(uint8_t)(v>>(8*i));
}
/* 8902/100 is APPROVE/u8, NOT the call id. The id is 102/u32. An explicit
 * caller policy supplies approve/cause; parsing 8901 alone never approves it.
 * Android's archived accepted shape omits cause; include it only explicitly.
 * out must not overlap cmd. There is no new request transaction here. */
static inline unsigned x9cw_approve(const uint8_t *cmd,unsigned n,uint8_t *out,unsigned room,
                                   unsigned approve,unsigned include_cause,uint32_t cause){
 struct x9cw_incoming c;unsigned size=include_cause?48:40;
 if(!out||room<size||approve>1||include_cause>1||!x9cw_incoming(cmd,n,&c))return 0;
 x9cw_header(out,size,0x8902,c.response_mask,c.txid);x9cw_scalar(out+16,0x100,approve,1);
 unsigned at=24;if(include_cause){x9cw_scalar(out+at,0x101,cause,4);at+=8;}
 x9cw_scalar(out+at,0x102,c.call_id,4);x9cw_scalar(out+at+8,0x103,c.sequence,4);
 return size;
}
/* iOS20H392 sendDialReq 1f1ac1f8c: VOICE20, tag1 CallingNumber is
 * raw bytes (writer 1f192a104), NOT a NUL string. Ordinary CallType tag10
 * is u8=0 (1f1ac2188..2198). Restrict this stage to decimal digits and
 * the RIL's <=40-byte limit (3814f4..4fc). Reject, do not silently discard,
 * CLIR, emergency/IP/video, pause/USSD, duplicate and unknown variants. */
struct x9cw_dial_number {const uint8_t *digits;unsigned length;};
static inline int x9cw_qmi_dial(const uint8_t *p,unsigned n,struct x9cw_dial_number *out){
 if(!p||!out||n<8||n>64||p[0]||x9cw_u16(p+3)!=0x20||x9cw_u16(p+5)!=n-7)return 0;
 struct x9cw_dial_number d={NULL,0};unsigned seen=0;
 for(unsigned at=7;at<n;){
  if(n-at<3)return 0;
  unsigned tag=p[at],k=x9cw_u16(p+at+1);at+=3;
  if(k>n-at)return 0;
  if(tag==1){
   if((seen&1)||!k||k>40)return 0;
   for(unsigned i=0;i<k;i++)if(p[at+i]<'0'||p[at+i]>'9')return 0;
   seen|=1;d.digits=p+at;d.length=k;
  }else if(tag==0x10){
   if((seen&2)||k!=1||p[at])return 0;
   seen|=2;
  }else return 0;
  at+=k;
 }
 if(!(seen&1))return 0;
 *out=d;return 1;
}
/* A captured route/profile is supplied EXPLICITLY by the caller, not chosen
 * from QMI CallType or a modem default. Only PLJ110 SIM2's measured profile
 * is implemented. Non-emergency getDomain returns0 on the IMS request branch;
 * getEccPreferredDomain still reads runtime state. No production provider yet
 * establishes the complete IMS/profile context in iOS. */
struct x9cw_dial_profile {
 unsigned sim;uint32_t address_type,call_type,domain,ecc_domain;unsigned clir;
};
static inline int x9cw_dial_profile_valid(const struct x9cw_dial_profile *p){
 return p&&p->sim==1&&p->address_type==2&&p->call_type==1&&p->domain==0&&
        p->ecc_domain==0&&p->clir==0;
}
/* Pure encoder, never submits/announces/approves. Reject a second dial while
 * any native call is known. Copy digits before output writes, permitting
 * input/output overlap without corrupting the private number. */
static inline unsigned x9cw_dial(const uint8_t *qmi,unsigned n,uint8_t *out,unsigned room,
                                const struct x9cw_dial_profile *profile,unsigned native_count){
 struct x9cw_dial_number d;uint8_t number[40];
 if(!out||native_count||!x9cw_dial_profile_valid(profile)||!x9cw_qmi_dial(qmi,n,&d))return 0;
 unsigned string_size=(d.length+12)&~7u,size=16+string_size+40;
 if(room<size)return 0;
 for(unsigned i=0;i<d.length;i++)number[i]=d.digits[i];
 x9cw_header(out,size,0x901,2,0);
 x9cw_put16(out+16,0x8100);x9cw_put16(out+18,d.length+1);
 for(unsigned i=0;i<d.length;i++)out[20+i]=number[i];
 x9cw_zero(number,sizeof(number));
 unsigned at=16+string_size;
 /* Exactly the observed serialized order, including CLIR108/u8. */
 x9cw_scalar(out+at,0x108,0,1);x9cw_scalar(out+at+8,0x101,2,4);
 x9cw_scalar(out+at+16,0x102,1,4);x9cw_scalar(out+at+24,0x103,0,4);
 x9cw_scalar(out+at+32,0x104,0,4);return size;
}
static inline int x9cw_native_dial(const uint8_t *p,unsigned n,unsigned sim_count){
 if(!x9cw_frame(p,n,0x901)||sim_count!=2||p[8]!=2||n<64||
    x9cw_u16(p+16)!=0x8100)return 0;
 unsigned k=x9cw_u16(p+18);
 if(k<2||k>41)return 0;
 unsigned size=(k+11)&~7u,at=16+size;
 if(n!=at+40||p[20+k-1])return 0;
 for(unsigned i=0;i<k-1;i++)if(p[20+i]<'0'||p[20+i]>'9')return 0;
 for(unsigned i=4+k;i<size;i++)if(p[16+i])return 0;
 const unsigned tags[]={0x108,0x101,0x102,0x103,0x104},values[]={0,2,1,0,0};
 for(unsigned i=0;i<5;i++,at+=8){
  unsigned bytes=i?4:1;
  if(x9cw_u16(p+at)!=tags[i]||x9cw_u16(p+at+2)!=bytes||
     (bytes==4?x9cw_u32(p+at+4):p[at+4])!=values[i])return 0;
  for(unsigned j=4+bytes;j<8;j++)if(p[at+j])return 0;
 }
 return 1;
}
/* This observed DIAL_CNF has result0/u32 ONLY. It contains no call id.
 * Do not generate a QMI20 success/CallIdentifier from this confirmation. */
static inline int x9cw_dial_cnf(const uint8_t *p,unsigned n,uint32_t *result){
 if(!result||!x9cw_frame(p,n,0x902)||n!=24||x9cw_u16(p+16)||x9cw_u16(p+18)!=4)return 0;
 *result=x9cw_u32(p+20);return 1;
}
/* AP request whitelist, zero padding, no wildcard hangup/cause/fanout.
 * DIAL is restricted to the explicit measured SIM2 profile above.
 * No status subscription or guessed call/voice configuration. */
static inline int x9cw_request(const uint8_t *p,unsigned n,unsigned id,unsigned sim_count){
 if(!x9cw_frame(p,n,id)||(p[8]!=1&&(p[8]!=2||sim_count!=2)))return 0;
 if(id==0x901)return x9cw_native_dial(p,n,sim_count);
 if(id==0x909)return n==16; /* RIL381354..398: read-only GET_STATUS{} */
 if(id==0x907)return n==16; /* archived Android ANSWER: no TLVs */
 if(id!=0x905||n!=32||x9cw_u16(p+16)!=0x100||x9cw_u16(p+18)!=4||
    x9cw_u32(p+20)||x9cw_u16(p+24)!=0x101||x9cw_u16(p+26)!=4||!x9cw_u32(p+28))return 0;
 return 1;
}
/* Actual iOS20H392 answer22/end21: tag1 CallIdentifier is a byte.
 * No optional TLVs silently discarded. Return 0 for every other request. */
static inline int x9cw_qmi_action(const uint8_t *p,unsigned n,unsigned *id,unsigned *call_id){
 if(!p||!id||!call_id||n!=11||p[0]||x9cw_u16(p+5)!=4||p[7]!=1||
    x9cw_u16(p+8)!=1||(x9cw_u16(p+3)!=0x21&&x9cw_u16(p+3)!=0x22))return 0;
 *id=x9cw_u16(p+3);*call_id=p[10];return 1;
}
/* Staged adapter only: the caller MUST supply a verified single-call mapping.
 * Native id is opaque u32, not a cast of Apple's u8. Empty Android ANSWER
 * cannot target one member of a multi-call set; do not send in that case.
 * This creates bytes only, never submits a mutation or advertises VOICE. */
static inline unsigned x9cw_action(const uint8_t *qmi,unsigned n,uint8_t *out,unsigned room,
                                  unsigned ios_id,uint32_t native_id,unsigned sim,
                                  unsigned native_call_count){
 unsigned id,call;unsigned size;
 if(!out||sim>1||native_call_count!=1||!native_id||!ios_id||ios_id>255||
    !x9cw_qmi_action(qmi,n,&id,&call)||call!=ios_id)return 0;
 size=id==0x22?16:32;if(room<size)return 0;
 x9cw_header(out,size,id==0x22?0x907:0x905,1u<<sim,0);
 if(id==0x21){x9cw_scalar(out+16,0x100,0,4);x9cw_scalar(out+24,0x101,native_id,4);}
 return size;
}
#endif
