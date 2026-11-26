/* 20H392 QMISMS storage requests. Byte wire, not host C++ struct layout.
 * readStoredMessages196c298/196c410/196c4d0: WMS31, storage01/u8,
 * RequestedTag10/u8, Message_Mode11/u8=1. Indexed22/23 use mode10.
 * No deletion, implicit IMS send, index remapping or whole-store mutation. */
#ifndef X9_SMS_STORAGE_H
#define X9_SMS_STORAGE_H
#include <stdint.h>

enum { X9MS_OK=0, X9MS_ARG=1, X9MS_UNSUPPORTED=2 };
struct x9ms_storage_request {
 uint32_t index;
 unsigned storage,list,modify,has_tag,tag;
};
static inline unsigned x9ms_u16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static inline uint32_t x9ms_u32(const uint8_t *p){
 return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
/* Commit output only after the complete, unique-tag request is accepted.
 * Unsupported modes must NOT be reinterpreted as storage tag filters.
 * Both storage31 optional fields are independent: tag10, mode11.
 * On RawRead22 only, tag11 is the optional SMS-on-IMS boolean (zero).
 * Absent mode retains the existing GSM-only request subset; any explicit
 * mode must be 3GPP1. This does not advertise IMS storage capabilities. */
static inline unsigned x9ms_storage_decode(const uint8_t *p,unsigned n,
                                           struct x9ms_storage_request *out){
 struct x9ms_storage_request r={0,0,0,0,0,0};unsigned id,seen=0;
 if(!p||!out||n<7||p[0]||x9ms_u16(p+5)!=n-7)return X9MS_ARG;
 id=x9ms_u16(p+3);if(id!=0x31&&id!=0x22&&id!=0x23)return X9MS_UNSUPPORTED;
 r.list=id==0x31;r.modify=id==0x23;
 for(unsigned at=7;at<n;){
  unsigned tag,k,bit;const uint8_t *v;
  if(n-at<3)return X9MS_ARG;
  tag=p[at];k=x9ms_u16(p+at+1);if(k>n-at-3)return X9MS_ARG;v=p+at+3;
  if(tag==1){
   bit=1;if(k!=(r.list?1u:r.modify?6u:5u)||v[0]>1)return X9MS_ARG;
   r.storage=v[0];
   if(!r.list){r.index=x9ms_u32(v+1);if(!r.index||r.index>65535)return X9MS_ARG;}
   if(r.modify&&v[5]!=0)return X9MS_UNSUPPORTED; /* MT -> read only. */
  }else if(r.list&&tag==0x10){
   bit=2;if(k!=1||v[0]>3)return X9MS_ARG;r.has_tag=1;r.tag=v[0];
  }else if(tag==(r.list?0x11u:0x10u)){
   bit=4;if(k!=1||v[0]!=1)return X9MS_UNSUPPORTED;
  }else if(id==0x22&&tag==0x11){
   bit=8;if(k!=1||v[0])return X9MS_UNSUPPORTED;
  }else return X9MS_UNSUPPORTED;
  if(seen&bit)return X9MS_ARG;seen|=bit;at+=3+k;
 }
 if(!(seen&1))return X9MS_ARG;
 *out=r;return X9MS_OK;
}
/* SMS-DELIVER originator-address length is in semi-octets. DCS follows its
 * type-of-address and packed digits; class 6 means no explicit class. */
static inline unsigned x9ms_pdu_class(const uint8_t *p,unsigned n){
 if(!p||n<3||p[0]>12)return 6;
 unsigned start=p[0]+1;
 if(start>n||n-start<3)return 6;
 unsigned address=p[start+1];
 unsigned dcs=start+4+(address+1)/2;
 if(address>20||dcs>=n)return 6;
 unsigned value=p[dcs];
 return (((value&0xc0)==0&&(value&0x10))||(value&0xf0)==0xf0)?value&3:4;
}
#endif
