/* Synthetic vectors only: no device numbers, vendor/Apple bytes or network.
 * Compile the production transport, not a parallel Python implementation. */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include "../../kernel/src/bsd/dev/x9_modem_bridge.h"
static int lock_depth;
#define LCK_GRP_DECLARE(n, s) int n
#define LCK_MTX_DECLARE(n, g) int n
static void lck_mtx_lock(int *){assert(!lock_depth);lock_depth=1;}
static void lck_mtx_unlock(int *){assert(lock_depth);lock_depth=0;}
static int x9_at_enabled(){return 1;}
static int sweet_x9_arg(const char *,int){return 1;}
int x9_md_at_online(){return 1;}
static unsigned writes,last_length;
static int tx_result;
static uint8_t last_frame[X9_MIPC_FRAME_MAX];
static int x9md_tx(unsigned a,unsigned b,unsigned c,unsigned d,unsigned e,const uint8_t *p,unsigned n){
 assert(!lock_depth&&a==5&&b==217&&c==0&&d==16+n&&e==0);
 writes++;last_length=n;memcpy(last_frame,p,n);return tx_result;
}
#include "../../kernel/src/bsd/dev/x9_mipc_bridge.inc"
static const unsigned X9BB_PORT_BASE=0x2000;
static const uint16_t X9Q_UNSUPPORTED=94;
static unsigned qmi_replies,qmi_error;
struct x9q_packet {
 x9q_packet(const uint8_t *,uint16_t error){qmi_error=error;}
 void send(uint32_t,uint32_t){qmi_replies++;}
};
#include "../../kernel/src/iokit/Kernel/arm/X9Calls.inc"

static unsigned incoming(uint8_t *p,unsigned mask=0x20,unsigned tx=0x2345){
 x9cw_header(p,64,0x8901,mask,tx);
 x9cw_scalar(p+16,0x100,0x12345678,4);
 x9cw_put16(p+24,0x8101);x9cw_put16(p+26,2);p[28]='X';
 x9cw_scalar(p+32,0x102,129,4);x9cw_scalar(p+40,0x103,0x90abcdef,4);
 x9cw_scalar(p+48,0x104,20,4);x9cw_scalar(p+56,0x105,0,1);return 64;
}
static void rx_incoming(unsigned tx=0x2345){uint8_t p[64];incoming(p,0x20,tx);x9mi_rx(p,sizeof(p));}
static uint32_t event(){
 uint8_t out[X9_MIPC_FRAME_MAX];uint32_t n,token;
 assert(x9_md_mipc_call_event(out,sizeof(out),&n,&token)==1&&n==64&&token);
 assert(out[8]==0x20);return token;
}
static void scalar_ind(unsigned id){uint8_t p[24];x9cw_header(p,sizeof(p),id,1,0);x9cw_scalar(p+16,0x100,1,1);x9mi_rx(p,sizeof(p));}
static void unchanged_other_inboxes(){
 scalar_ind(0x4601);scalar_ind(0x4202);
 uint8_t out[24];uint32_t n,token;
 assert(x9_md_mipc_message_event(out,sizeof(out),&n,&token)==1&&n==24&&!token&&x9cw_u16(out+10)==0x4601);
 assert(x9_md_mipc_event(out,sizeof(out),&n)==1&&n==24&&x9cw_u16(out+10)==0x4202);
 assert(!x9_mm_poisoned&&!x9_mi_poisoned);
}
static void codec_golden(){
 uint8_t cmd[64],rsp[48];incoming(cmd);
 struct x9cw_incoming c;assert(x9cw_incoming(cmd,sizeof(cmd),&c));
 assert(c.call_id==0x12345678&&c.sequence==0x90abcdef&&c.mask==0x20&&c.txid==0x2345&&c.number_length==2);
 assert(x9cw_approve(cmd,sizeof(cmd),rsp,sizeof(rsp),1,0,0)==40);
 const uint8_t golden[]={
  0x84,0x19,0x54,0x24,0,0,0,0,0x20,0,0x02,0x89,0x45,0x23,0x18,0,
  0x00,0x01,0x01,0,1,0,0,0,
  0x02,0x01,0x04,0,0x78,0x56,0x34,0x12,
  0x03,0x01,0x04,0,0xef,0xcd,0xab,0x90};
 assert(sizeof(golden)==40&&!memcmp(rsp,golden,sizeof(golden)));
 assert(x9cw_approve(cmd,sizeof(cmd),rsp,sizeof(rsp),0,1,0x11223344)==48);
 assert(rsp[20]==0&&x9cw_u16(rsp+24)==0x101&&x9cw_u32(rsp+28)==0x11223344);
 assert(x9cw_u16(rsp+32)==0x102&&x9cw_u32(rsp+36)==0x12345678);
 assert(!x9cw_approve(cmd,sizeof(cmd),rsp,39,1,0,0));
 assert(!x9cw_approve(cmd,sizeof(cmd),rsp,sizeof(rsp),2,0,0));
}
static void codec_invalid(){
 uint8_t p[80],rsp[48];struct x9cw_incoming c;
 for(unsigned n=0;n<64;n++){incoming(p);assert(!x9cw_incoming(p,n,&c));}
 incoming(p);p[29]='Y';assert(!x9cw_incoming(p,64,&c));
 incoming(p);p[28]=0;assert(!x9cw_incoming(p,64,&c));
 incoming(p);p[8]=0x30;assert(!x9cw_incoming(p,64,&c));
 incoming(p);p[18]=3;assert(!x9cw_incoming(p,64,&c));
 incoming(p);p[18]=255;assert(!x9cw_incoming(p,64,&c));
 incoming(p);x9cw_scalar(p+64,0x8100,1,4);x9cw_put16(p+14,56);assert(!x9cw_incoming(p,72,&c));
 incoming(p);x9cw_scalar(p+64,0x106,1,4);x9cw_put16(p+14,56);assert(!x9cw_incoming(p,72,&c));
 assert(!x9cw_approve(p,72,rsp,sizeof(rsp),1,0,0));
 /* libmipc ignores these received fields. They are not SIM selectors. */
 incoming(p);p[0]=0;p[4]=1;p[9]=7;p[23]=0x5a;assert(x9cw_incoming(p,64,&c));
}
static void action_codec(){
 uint8_t req[]={0,0x12,0x34,0x21,0,4,0,1,1,0,7},out[48];unsigned id,call;
 assert(x9cw_qmi_action(req,sizeof(req),&id,&call)&&id==0x21&&call==7);
 assert(x9cw_action(req,sizeof(req),out,sizeof(out),7,0x12345678,1,1)==32);
 assert(out[8]==2&&x9cw_u16(out+10)==0x905&&x9cw_u32(out+20)==0&&x9cw_u32(out+28)==0x12345678);
 assert(x9cw_request(out,32,0x905,2)&&!x9cw_request(out,32,0x905,1));
 assert(!x9cw_action(req,sizeof(req),out,sizeof(out),6,0x12345678,1,1));
 assert(!x9cw_action(req,sizeof(req),out,sizeof(out),7,0x12345678,1,2));
 assert(!x9cw_action(req,sizeof(req),out,sizeof(out),7,0,1,1));
 req[3]=0x22;assert(x9cw_action(req,sizeof(req),out,sizeof(out),7,0x12345678,0,1)==16);
 assert(x9cw_u16(out+10)==0x907&&x9cw_u16(out+14)==0&&x9cw_request(out,16,0x907,2));
 req[8]=4;assert(!x9cw_qmi_action(req,sizeof(req),&id,&call));
 req[8]=1;req[3]=0x20;assert(!x9cw_action(req,sizeof(req),out,sizeof(out),7,1,0,1));
}
static const struct x9cw_dial_profile measured_route={1,2,1,0,0,0};
static unsigned dial_qmi(uint8_t *p,unsigned digits=5,bool call_type=true){
 unsigned n=10+digits+(call_type?4:0);bzero(p,n);
 p[1]=0x34;p[2]=0x12;p[3]=0x20;x9cw_put16(p+5,n-7);
 p[7]=1;x9cw_put16(p+8,digits);
 for(unsigned i=0;i<digits;i++)p[10+i]=(uint8_t)('1'+i%9);
 if(call_type){p[10+digits]=0x10;p[11+digits]=1;}
 return n;
}
static void dial_golden(){
 uint8_t q[64],p[104];unsigned n=dial_qmi(q);
 struct x9cw_dial_number d;assert(x9cw_qmi_dial(q,n,&d)&&d.length==5);
 assert(x9cw_dial(q,n,p,sizeof(p),&measured_route,0)==72);
 const uint8_t golden[]={
  0x84,0x19,0x54,0x24,0,0,0,0,2,0,1,9,0,0,0x38,0,
  0,0x81,6,0,'1','2','3','4','5',0,0,0,0,0,0,0,
  8,1,1,0,0,0,0,0,1,1,4,0,2,0,0,0,
  2,1,4,0,1,0,0,0,3,1,4,0,0,0,0,0,4,1,4,0,0,0,0,0};
 assert(sizeof(golden)==72&&!memcmp(p,golden,72));
 assert(x9cw_native_dial(p,72,2)&&x9cw_request(p,72,0x901,2));
 assert(!x9cw_request(p,72,0x901,1));
 assert(!x9cw_dial(q,n,p,71,&measured_route,0));
 assert(!x9cw_dial(q,n,p,sizeof(p),&measured_route,1));
 assert(!x9cw_dial(q,n,p,sizeof(p),NULL,0));
 struct x9cw_dial_profile wrong=measured_route;wrong.domain=5;
 assert(!x9cw_dial(q,n,p,sizeof(p),&wrong,0));
 wrong=measured_route;wrong.sim=0;assert(!x9cw_dial(q,n,p,sizeof(p),&wrong,0));
 /* QMI's string has no NUL; MIPC adds one. Optional type can be omitted. */
 n=dial_qmi(q,5,false);assert(x9cw_dial(q,n,p,sizeof(p),&measured_route,0)==72);
 n=dial_qmi(q,40);assert(x9cw_dial(q,n,p,sizeof(p),&measured_route,0)==104);
 assert(x9cw_request(p,104,0x901,2)&&p[60]==0);
 n=dial_qmi(q,1);assert(x9cw_dial(q,n,p,sizeof(p),&measured_route,0)==64);
 /* Same-buffer translation reads all private digits before writing header. */
 uint8_t alias[104];n=dial_qmi(alias);assert(x9cw_dial(alias,n,alias,104,&measured_route,0)==72);
 assert(!memcmp(alias,golden,72));
}
static void dial_invalid(){
 uint8_t q[80],p[104];struct x9cw_dial_number d;
 for(unsigned k=0;k<19;k++){dial_qmi(q);assert(!x9cw_qmi_dial(q,k,&d));}
 unsigned n=dial_qmi(q,0);assert(!x9cw_qmi_dial(q,n,&d));
 n=dial_qmi(q,41);assert(!x9cw_qmi_dial(q,n,&d));
 const uint8_t invalid[]={0,'+',',',';','*','#',0xff};
 for(uint8_t c:invalid){n=dial_qmi(q);q[12]=c;assert(!x9cw_qmi_dial(q,n,&d));}
 n=dial_qmi(q);q[0]=2;assert(!x9cw_qmi_dial(q,n,&d));
 n=dial_qmi(q);q[3]=0x22;assert(!x9cw_qmi_dial(q,n,&d));
 n=dial_qmi(q);q[18]=9;assert(!x9cw_qmi_dial(q,n,&d)); // emergency
 n=dial_qmi(q);q[15]=0x11;assert(!x9cw_qmi_dial(q,n,&d)); // CLIR not silently ignored
 n=dial_qmi(q);q[15]=1;assert(!x9cw_qmi_dial(q,n,&d)); // duplicate number
 n=dial_qmi(q);q[16]=4;assert(!x9cw_qmi_dial(q,n,&d)); // truncated TLV
 n=dial_qmi(q);q[n]=0xff;x9cw_put16(q+5,n+1-7);assert(!x9cw_qmi_dial(q,n+1,&d));
 assert(!x9cw_dial(q,n+1,p,sizeof(p),&measured_route,0));
}
static void dial_whitelist(){
 uint8_t q[64],p[112];unsigned n=dial_qmi(q);assert(x9cw_dial(q,n,p,112,&measured_route,0)==72);
 assert(x9_md_mipc_submit(p,72));
 /* Reject each changed byte in header route, tag, value, string or padding. */
 const unsigned offsets[]={8,16,18,20,25,26,31,32,34,36,37,40,42,44,48,50,52,56,58,60,64,66,68};
 for(unsigned at:offsets){p[at]^=0x80;assert(!x9_md_mipc_submit(p,72));p[at]^=0x80;}
 for(unsigned k=0;k<72;k++)assert(!x9cw_native_dial(p,k,2));
 x9cw_scalar(p+72,0x105,1,2);x9cw_put16(p+14,64);assert(!x9_md_mipc_submit(p,80));
 assert(writes==0&&x9_mi_next==1); // invalid attempts did not enqueue
}
static void dial_roundtrip(){
 uint8_t q[64],p[104],cnf[24],out[104];unsigned n=dial_qmi(q);
 assert(x9cw_dial(q,n,p,104,&measured_route,0)==72);
 uint64_t token=x9_md_mipc_submit(p,72);assert(token);
 x9mi_tick();assert(writes==1&&last_length==72&&last_frame[8]==2&&x9cw_u16(last_frame+10)==0x901);
 assert(x9cw_u16(last_frame+12)==token);
 /* Host timeout/cancel cannot release or repeat a sent dial. */
 x9_md_sim_at_cancel(token);uint32_t len,result;
 for(unsigned i=0;i<100;i++){assert(x9_md_mipc_take(token,out,104,&len)==0);x9mi_tick();}
 assert(writes==1&&x9_mi_slots[0].state==2);
 x9cw_header(cnf,24,0x902,2,(unsigned)token);cnf[9]=2;x9cw_scalar(cnf+16,0,0,4);
 x9mi_rx(cnf,9);assert(x9_md_mipc_take(token,out,104,&len)==0);
 x9mi_rx(cnf+9,15);assert(x9_md_mipc_take(token,out,104,&len)==1&&len==24);
 assert(x9cw_dial_cnf(out,len,&result)&&result==0);
 assert(!x9cw_qmi_action(out,len,&n,&result)); // native CNF is not a QMI success/id
 assert(x9_md_mipc_take(token,out,104,&len)==-1);
 x9mi_rx(cnf,24);for(unsigned i=0;i<10;i++)x9mi_tick();assert(writes==1);
}
static void dial_tx_failure(){
 uint8_t q[64],p[104],out[104];unsigned n=dial_qmi(q);uint32_t len;
 assert(x9cw_dial(q,n,p,104,&measured_route,0)==72);
 uint64_t token=x9_md_mipc_submit(p,72);assert(token);tx_result=-1;x9mi_tick();
 assert(writes==1&&x9_mi_slots[0].state==2&&x9_mi_poisoned);
 x9_md_sim_at_cancel(token);x9mi_online(1);
 assert(x9_md_mipc_take(token,out,104,&len)==0);
 for(unsigned i=0;i<100;i++)x9mi_tick();assert(writes==1);
 assert(!x9_md_mipc_submit(p,72));
}
static void dial_cnf_invalid(){
 uint8_t p[40];uint32_t result=0x12345678;
 x9cw_header(p,24,0x902,2,77);x9cw_scalar(p+16,0,0,4);
 for(unsigned n=0;n<24;n++)assert(!x9cw_dial_cnf(p,n,&result));
 assert(result==0x12345678&&x9cw_dial_cnf(p,24,&result)&&result==0);
 x9cw_put32(p+20,42);assert(x9cw_dial_cnf(p,24,&result)&&result==42); // native error remains native
 p[16]=1;assert(!x9cw_dial_cnf(p,24,&result));p[16]=0;
 p[18]=1;assert(!x9cw_dial_cnf(p,24,&result));p[18]=4;
 x9cw_scalar(p+24,0x100,1,1);x9cw_put16(p+14,16);assert(!x9cw_dial_cnf(p,32,&result));
 assert(!x9cw_dial_cnf(p,32,NULL));
}
static void request_whitelist(){
 uint8_t req[]={0,1,0,0x21,0,4,0,1,1,0,7},p[48];
 assert(x9cw_action(req,sizeof(req),p,sizeof(p),7,0x12345678,1,1)==32);
 assert(x9_md_mipc_submit(p,32));
 p[20]=1;assert(!x9_md_mipc_submit(p,32));p[20]=0;
 x9cw_put32(p+28,0);assert(!x9_md_mipc_submit(p,32));x9cw_put32(p+28,1);
 x9cw_scalar(p+32,0x102,16,4);x9cw_put16(p+14,24);assert(!x9_md_mipc_submit(p,40));
 x9cw_header(p,16,0x907,2,0);assert(x9_md_mipc_submit(p,16));
 x9cw_header(p,16,0x901,2,0);assert(!x9_md_mipc_submit(p,16));
 x9cw_header(p,16,0x8902,0x20,0x2345);assert(!x9_md_mipc_submit(p,16));
 x9cw_header(p,16,0x907,3,0);assert(!x9_md_mipc_submit(p,16));
}
static void no_auto_policy(){
 rx_incoming();for(unsigned i=0;i<10;i++)x9mi_tick();assert(!writes);
 uint32_t token=event();assert(x9_md_mipc_call_reply(token,2,0,0)==-1);
 for(unsigned i=0;i<10;i++)x9mi_tick();assert(!writes);
}
static void command_roundtrip(){
 rx_incoming();uint32_t token=event();int result=-1;
 assert(x9_md_mipc_call_reply(token,1,0,0)==1);
 assert(x9_md_mipc_call_reply_take(token,&result)==0);
 assert(x9_md_mipc_call_reply(token,1,0,0)==-1);
 x9mi_tick();assert(writes==1&&last_length==40&&last_frame[8]==0x20);
 assert(x9cw_u16(last_frame+10)==0x8902&&x9cw_u16(last_frame+12)==0x2345);
 assert(last_frame[20]==1&&x9cw_u32(last_frame+28)==0x12345678&&x9cw_u32(last_frame+36)==0x90abcdef);
 assert(x9_md_mipc_call_reply_take(token,&result)==1&&result==X9_AT_OK);
 assert(x9_md_mipc_call_reply_take(token,&result)==0&&x9_mc_slots[0].state==6);
 for(unsigned i=0;i<10;i++)x9mi_tick();assert(writes==1);
 for(uint8_t b:x9_mc_slots[0].frame)assert(!b);
}
static void failure_retained(){
 rx_incoming();uint32_t token=event();assert(x9_md_mipc_call_reply(token,1,0,0)==1);
 tx_result=-1;x9mi_tick();int result;
 assert(x9_md_mipc_call_reply_take(token,&result)==1&&result==X9_AT_OFFLINE);
 assert(x9_md_mipc_call_reply_take(token,&result)==0&&x9_mc_slots[0].state==6);
 assert(x9_md_mipc_call_reply(token,1,0,0)==-1);
 x9mi_online(1);for(unsigned i=0;i<10;i++)x9mi_tick();assert(writes==1);
}
static void duplicate_pending(){
 rx_incoming();rx_incoming();uint32_t token=event();rx_incoming();
 uint8_t out[64];uint32_t n,t;
 assert(x9_md_mipc_call_event(out,sizeof(out),&n,&t)==0);
 assert(!x9_mc_poisoned&&token==1&&x9_mc_next==1);
}
static void changed_duplicate(){
 rx_incoming();event();uint8_t p[64];incoming(p);p[28]='Y';x9mi_rx(p,sizeof(p));
 assert(x9_mc_poisoned);unchanged_other_inboxes();x9mi_tick();assert(!writes);
}
static void duplicate_replied(){
 command_roundtrip();rx_incoming();assert(x9_mc_poisoned);x9mi_tick();assert(writes==1);
 unchanged_other_inboxes();
}
static void overflow_isolated(){
 for(unsigned i=0;i<X9_MC_SLOTS+1;i++)scalar_ind(0x4901);
 assert(x9_mc_poisoned);unchanged_other_inboxes();
}
static void bounded_tombstones(){
 for(unsigned i=0;i<X9_MC_SLOTS;i++){
  rx_incoming(100+i);uint32_t token=event();int r;
  assert(x9_md_mipc_call_reply(token,1,0,0)==1);x9mi_tick();assert(x9_md_mipc_call_reply_take(token,&r)==1&&!r);
 }
 assert(writes==X9_MC_SLOTS);rx_incoming(999);assert(x9_mc_poisoned);unchanged_other_inboxes();
}
static void fifo(){
 rx_incoming(100);rx_incoming(101);uint32_t first=event(),second=event();
 assert(x9_md_mipc_call_reply(second,1,0,0)==1&&x9_md_mipc_call_reply(first,1,0,0)==1);
 x9mi_tick();assert(writes==1&&x9cw_u16(last_frame+12)==100);
 x9mi_tick();assert(writes==2&&x9cw_u16(last_frame+12)==101);
}
static void fragmented(){
 uint8_t p[64],out[64];incoming(p);uint32_t n,token;
 for(unsigned i=0;i<63;i++){
  x9mi_rx(p+i,1);assert(x9_md_mipc_call_event(out,sizeof(out),&n,&token)==0);
 }
 x9mi_rx(p+63,1);event();assert(!writes);
}
static void late_cnf(){
 uint8_t p[24],out[24];x9cw_header(p,16,0x907,2,0);
 uint64_t token=x9_md_mipc_submit(p,16);assert(token);x9mi_tick();
 uint32_t n;for(unsigned i=0;i<100;i++){
  assert(x9_md_mipc_take(token,out,sizeof(out),&n)==0);x9mi_tick();
 }
 assert(writes==1&&x9_mi_slots[0].state==2);
 x9cw_header(p,24,0x908,0x20,(unsigned)token);x9cw_scalar(p+16,0,0,4);p[9]=7;
 x9mi_rx(p,sizeof(p));assert(x9_md_mipc_take(token,out,sizeof(out),&n)==1&&n==24&&!memcmp(p,out,n));
 assert(x9_md_mipc_take(token,out,sizeof(out),&n)==-1&&writes==1);
}
static void sms_priority(){
 rx_incoming();uint32_t call=event();assert(x9_md_mipc_call_reply(call,1,0,0)==1);
 uint8_t p[24],out[24];x9cw_header(p,24,0x8601,1,321);x9cw_scalar(p+16,0x100,3,1);x9mi_rx(p,24);
 uint32_t n,sms;assert(x9_md_mipc_message_event(out,24,&n,&sms)==1&&sms);
 assert(x9_md_mipc_message_reply(sms,1,0)==1);
 x9mi_tick();assert(writes==1&&x9cw_u16(last_frame+10)==0x8602);
 x9mi_tick();assert(writes==2&&x9cw_u16(last_frame+10)==0x8902);
}
static void staged_ios(){
 uint8_t qmi[]={0,1,0,0x22,0,4,0,1,1,0,7};
 assert(!x9c_request(1,X9BB_PORT_BASE+5,qmi,sizeof(qmi))&&!qmi_replies);
 assert(x9c_request(1,X9BB_PORT_BASE+9,qmi,sizeof(qmi))&&qmi_replies==1&&qmi_error==94);
 uint8_t dial[64];unsigned n=dial_qmi(dial);
 assert(x9c_request(1,X9BB_PORT_BASE+9,dial,n)&&qmi_replies==2&&qmi_error==94);
 x9mi_tick();assert(!writes&&!x9_mi_next&&!x9_mc_next);
}
int main(int argc,char **argv){
 assert(argc==2);x9mi_online(1);
 struct {const char *name;void (*run)();} cases[]={
  {"codec_golden",codec_golden},{"codec_invalid",codec_invalid},{"action_codec",action_codec},
  {"dial_golden",dial_golden},{"dial_invalid",dial_invalid},{"dial_whitelist",dial_whitelist},
  {"dial_roundtrip",dial_roundtrip},{"dial_tx_failure",dial_tx_failure},{"dial_cnf_invalid",dial_cnf_invalid},
  {"request_whitelist",request_whitelist},{"no_auto_policy",no_auto_policy},{"command_roundtrip",command_roundtrip},
  {"failure_retained",failure_retained},{"duplicate_pending",duplicate_pending},{"changed_duplicate",changed_duplicate},
  {"duplicate_replied",duplicate_replied},{"overflow_isolated",overflow_isolated},{"bounded_tombstones",bounded_tombstones},
  {"fifo",fifo},{"fragmented",fragmented},{"late_cnf",late_cnf},{"sms_priority",sms_priority},{"staged_ios",staged_ios}};
 for(auto c:cases)if(!strcmp(argv[1],c.name)){c.run();assert(!lock_depth);return 0;}
 return 2;
}
