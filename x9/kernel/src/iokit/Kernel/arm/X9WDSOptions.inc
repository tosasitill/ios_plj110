#ifndef X9_WDS_OPTIONS_INC
#define X9_WDS_OPTIONS_INC
/* QMI profile intent, not a modem configuration object. Decode LE fields
 * individually: QMI29 is packed 34B, unlike MTK's flagged AT input or its
 * encoded QoS. Presence (including an all-zero value) is never omission.
 * Public QMI field names/order match the saved 20H392 serializer. */
struct x9o_umts_qos {
 uint8_t traffic_class,delivery_order,sdu_error_ratio,residual_ber,erroneous_sdu;
 uint32_t max_ul,max_dl,guaranteed_ul,guaranteed_dl,max_sdu,transfer_delay,priority;
 uint8_t signaling;
};
enum : unsigned {
 X9O_QOS=1u<<0,X9O_OPERATOR_PCO=1u<<1,X9O_MCC=1u<<2,
 X9O_MNC=1u<<3,X9O_IDS=1u<<4,X9O_ALWAYS_ON=1u<<5
};
struct x9o_profile_options {
 unsigned present;
 x9o_umts_qos qos;
 uint16_t operator_pco,mcc,mnc,ids[10];
 uint8_t mnc_digit_flag,always_on;
};
static unsigned x9o_decode_qos(const uint8_t *p,unsigned n,x9o_umts_qos &out){
 bzero(&out,sizeof(out));
 if(!p||n!=34)return X9Q_ARG;
 x9o_umts_qos q={};
 q.traffic_class=p[0];q.max_ul=x9q_u32(p+1);q.max_dl=x9q_u32(p+5);
 q.guaranteed_ul=x9q_u32(p+9);q.guaranteed_dl=x9q_u32(p+13);
 q.delivery_order=p[17];q.max_sdu=x9q_u32(p+18);
 q.sdu_error_ratio=p[22];q.residual_ber=p[23];q.erroneous_sdu=p[24];
 q.transfer_delay=x9q_u32(p+25);q.priority=x9q_u32(p+29);q.signaling=p[33];
 if(q.traffic_class>4||q.delivery_order>2||q.sdu_error_ratio>7||
    q.residual_ber>9||q.erroneous_sdu>3||q.signaling>1)return X9Q_ARG;
 // Physical bitrate/delay units and representable numeric ranges must be
 // qualified before conversion; structural decoding never grants a route.
 out=q;return 0;
}
static unsigned x9o_decode_profile(const uint8_t *req,unsigned n,x9o_profile_options &out){
 bzero(&out,sizeof(out));
 if(!req||n<7||n>X9Q_REQ)return X9Q_ARG;
 x9o_profile_options o={};
 for(unsigned at=7;at<n;){
  if(n-at<3)return X9Q_ARG;
  unsigned k=x9q_u16(req+at+1);if(k>n-at-3)return X9Q_ARG;
  const uint8_t *v=req+at+3;unsigned bit=0,size=0,tag=req[at];
  switch(tag){
   case 0x29:bit=X9O_QOS;size=34;break;
   case 0x37:bit=X9O_OPERATOR_PCO;size=2;break;
   case 0x38:bit=X9O_MCC;size=2;break;
   case 0x39:bit=X9O_MNC;size=3;break;
   case 0x47:bit=X9O_IDS;size=20;break;
   case 0x4e:bit=X9O_ALWAYS_ON;size=1;break;
  }
  if(bit){
   if(k!=size||(o.present&bit))return X9Q_ARG;
   o.present|=bit;
   switch(tag){
    case 0x29:{unsigned e=x9o_decode_qos(v,k,o.qos);if(e)return e;break;}
    case 0x37:o.operator_pco=(uint16_t)x9q_u16(v);break;
    case 0x38:o.mcc=(uint16_t)x9q_u16(v);break;
    case 0x39:o.mnc=(uint16_t)x9q_u16(v);o.mnc_digit_flag=v[2];break;
    case 0x47:for(unsigned j=0;j<10;j++)o.ids[j]=(uint16_t)x9q_u16(v+2*j);break;
    case 0x4e:o.always_on=v[0];break;
   }
  }
  at+=3+k;
 }
 out=o;return 0;
}
enum : unsigned { X9O_NONE,X9O_CGEQREQ_FRESH_CANDIDATE,X9O_UNMAPPED };
struct x9o_modem_plan {
 unsigned kind,blocked_tag;
 x9o_umts_qos qos;
};
static bool x9o_fresh_qos_shape(const x9o_umts_qos &q){
 // Native fresh c7: Interactive3, handling priority1, Signaling1.
 // Numeric zero in every rate/size/delay position avoids guessing units.
 return q.traffic_class==3&&q.priority==1&&q.signaling==1&&
  !q.max_ul&&!q.max_dl&&!q.guaranteed_ul&&!q.guaranteed_dl&&
  !q.delivery_order&&!q.max_sdu&&!q.sdu_error_ratio&&!q.residual_ber&&
  !q.erroneous_sdu&&!q.transfer_delay;
}
static x9o_modem_plan x9o_make_plan(const x9o_profile_options &o){
 x9o_modem_plan plan={};
 const unsigned bits[]={X9O_OPERATOR_PCO,X9O_MCC,X9O_MNC,X9O_IDS,X9O_ALWAYS_ON};
 const unsigned tags[]={0x37,0x38,0x39,0x47,0x4e};
 for(unsigned j=0;j<5;j++)if(o.present&bits[j]){
  // QMI37/u16 is an operator-reserved container ID, not SET_PCO101's
  // optional APNindex/u32. 47's ten IDs are not 88/39B PCO entries.
  plan.kind=X9O_UNMAPPED;plan.blocked_tag=tags[j];return plan;
 }
 if(o.present&X9O_QOS){
  plan.qos=o.qos;plan.blocked_tag=0x29;
  plan.kind=x9o_fresh_qos_shape(o.qos)?X9O_CGEQREQ_FRESH_CANDIDATE:X9O_UNMAPPED;
 }
 return plan;
}
/* Candidate payload encoder only: no dispatch, CID allocation or ownership
 * assertion. The caller must supply a proven AT/D2 CID, not QMI profile ID,
 * raw MD/PS CID or channel. Actual P77 command table chooses the legacy AT
 * producer; its CID mapping and before-ACT setup/callback must be bound by
 * the bearer owner before this payload can be submitted.
 * Explicit every argument: omission merges existing cached QoS. Native
 * subscribed enums0 map to AT delivery2, error strings0E0, erroneous3.
 * Actual L4C maps Interactive AT2->3; priority1 and Signaling1 stay numeric.
 * Do not generalize this zero-rate/delay contract to nonzero physical units.
 * Returned length covers CR and NUL, as SYS_AT3/TLV8100 requires. */
static inline unsigned x9o_encode_cgeqreq_candidate(const x9o_modem_plan &plan,
 unsigned at_d2_cid,char *out,unsigned room,unsigned &len){
 len=0;if(out&&room)bzero(out,room);
 if(plan.kind!=X9O_CGEQREQ_FRESH_CANDIDATE||!x9o_fresh_qos_shape(plan.qos))return X9Q_UNSUPPORTED;
 // The actual legacy command handler advertises/tests CID <=49; the
 // independent DDM wrapper's <=50 range must not broaden this route.
 if(!out||!room||!at_d2_cid||at_d2_cid>49)return X9Q_ARG;
 int k=snprintf(out,room,"AT+CGEQREQ=%u,2,0,0,0,0,2,0,\"0E0\",\"0E0\",3,0,1,1,0\r",at_d2_cid);
 if(k<0||(unsigned)k>=room){bzero(out,room);return X9Q_ARG;}
 len=(unsigned)k+1;return 0;
}
static unsigned x9o_start_route(const x9o_modem_plan &plan){
 // An encoder candidate is not an executable before-ACT transaction. All
 // present QoS (even zero/subscribed) must wait for qualified CID/callback
 // binding; absence keeps the existing ordinary Internet path unchanged.
 return plan.kind==X9O_NONE?0:X9Q_UNSUPPORTED;
}
#endif
