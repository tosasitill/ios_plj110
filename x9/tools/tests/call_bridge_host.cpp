/* Synthetic vectors only: no device numbers, vendor/Apple bytes or network.
 * Compile the production transport, not a parallel Python implementation. */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
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
static const unsigned X9BB_NODE=1,CBQ_PORT_BASE=0x4000,CBQ_MAX_CLIENTS=8;
static const uint16_t X9Q_UNSUPPORTED=94,X9Q_INTERNAL=3,X9Q_NOT_READY=16,X9Q_ARG=48,X9Q_BUSY=2;
static struct {uint64_t generation;uint8_t sim_slot;} x9q_clients[CBQ_MAX_CLIENTS];
static uint64_t fake_ms;
static bool x9w_control_ready;
static uint64_t x9q_ms(){return fake_ms;}
static unsigned qmi_replies,qmi_error;
static uint8_t qmi_response[64];static unsigned qmi_response_len;
static unsigned qmi_indications;static uint8_t qmi_indication_states[32];
static void coachz_baseband_qrtr(uint32_t local,uint32_t node,uint32_t port,const void *p,unsigned n){
 assert(local>=CBQ_PORT_BASE&&node==X9BB_NODE&&port==X9BB_PORT_BASE+9&&n<=sizeof(qmi_response));
 memcpy(qmi_response,p,n);qmi_response_len=n;qmi_replies++;
 const uint8_t *q=static_cast<const uint8_t *>(p);
 if(n==11&&q[0]==4&&q[3]==0x54){
  assert(qmi_indications<sizeof(qmi_indication_states));qmi_indication_states[qmi_indications++]=q[10];
 }
}
struct x9q_packet {
 x9q_packet(const uint8_t *,uint16_t error){qmi_error=error;}
 void send(uint32_t,uint32_t){qmi_replies++;}
};
#include "../../kernel/src/iokit/Kernel/arm/X9Calls.inc"

static unsigned incoming(uint8_t *p,unsigned mask=0x22,unsigned tx=0x2345){
 x9cw_header(p,64,0x8901,mask,tx);
 x9cw_scalar(p+16,0x100,0x12345678,4);
 x9cw_put16(p+24,0x8101);x9cw_put16(p+26,2);p[28]='X';
 x9cw_scalar(p+32,0x102,129,4);x9cw_scalar(p+40,0x103,0x90abcdef,4);
 x9cw_scalar(p+48,0x104,20,4);x9cw_scalar(p+56,0x105,0,1);return 64;
}
static void rx_incoming(unsigned tx=0x2345){uint8_t p[64];incoming(p,0x22,tx);x9mi_rx(p,sizeof(p));}
static uint32_t event(){
 uint8_t out[X9_MIPC_FRAME_MAX];uint32_t n,token;
 assert(x9_md_mipc_call_event(out,sizeof(out),&n,&token)==1&&n==64&&token);
 assert(out[8]==0x22);return token;
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
 assert(c.call_id==0x12345678&&c.sequence==0x90abcdef&&c.mask==0x22&&c.response_mask==2&&c.sim==1&&c.txid==0x2345&&c.number_length==2);
 assert(x9cw_approve(cmd,sizeof(cmd),rsp,sizeof(rsp),1,0,0)==40);
 const uint8_t golden[]={
  0x84,0x19,0x54,0x24,0,0,0,0,2,0,0x02,0x89,0x45,0x23,0x18,0,
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
static void incoming_routes(){
 uint8_t p[64],rsp[48];struct x9cw_incoming c;
 /* Artificial numbers; live evidence proves route22 -> slot1 -> response02.
  * Change only the header mask, keeping the transaction/identity unchanged. */
 for(unsigned high=0;high<=0xf0;high+=0x10){
  for(unsigned low=1;low<=2;low++){
   incoming(p,high|low);
   assert(x9cw_incoming(p,64,&c)&&c.mask==(high|low)&&c.sim==low-1&&c.response_mask==low);
   assert(x9cw_approve(p,64,rsp,48,1,0,0)==40&&rsp[8]==low&&x9cw_u16(rsp+12)==0x2345);
  }
 }
 const unsigned rejected[]={0,0x10,0x20,0x30,3,0x23,4,8,0xff};
 for(unsigned mask:rejected){incoming(p,mask);assert(!x9cw_incoming(p,64,&c));assert(!x9cw_approve(p,64,rsp,48,1,0,0));}
 incoming(p,0x22);x9mi_rx(p,64);uint32_t token=event();
 assert(x9_mc_slots[0].mask==0x22&&x9_md_mipc_call_reply(token,1,0,0)==1);
 x9mi_tick();assert(writes==1&&last_frame[8]==2&&x9cw_u16(last_frame+12)==0x2345);
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
 x9mi_tick();assert(writes==1&&last_length==40&&last_frame[8]==2);
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
 /* Valid MD RING event6 has only100/u32. A malformed progress frame
  * would quarantine at the first input, never exercising FIFO capacity. */
 uint8_t p[24];x9cw_header(p,24,0x4902,1,0);x9cw_scalar(p+16,0x100,6,4);
 for(unsigned i=0;i<X9_MC_SLOTS;i++){x9mi_rx(p,24);assert(!x9_mc_poisoned);}
 x9mi_rx(p,24);
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
/* Full synthetic MD frames, never padded/reconstructed Android DUMP tails.
 * These cases have been added for future authorized execution; compilation
 * alone does NOT validate the new parser or lifecycle. */
static unsigned progress_frame(uint8_t *p,unsigned event,unsigned id=7,unsigned mode=20,unsigned mask=2){
 x9cw_header(p,512,0x4901,mask,0);unsigned at=16;
 const unsigned tags[]={0x100,0x101,0x102,0x103},values[]={id,0,mode,129};
 for(unsigned i=0;i<4;i++,at+=8)x9cw_scalar(p+at,tags[i],values[i],4);
 x9cw_put16(p+at,0x8104);x9cw_put16(p+at+2,1);at+=8;
 x9cw_scalar(p+at,0x105,0x100000,4);at+=8;
 x9cw_put16(p+at,0x8106);x9cw_put16(p+at+2,408);p[at+4]=0xff;at+=416;
 x9cw_scalar(p+at,0x107,0,4);at+=8;x9cw_scalar(p+at,0x109,event,4);at+=8;
 x9cw_scalar(p+at,0x10a,16,4);at+=8;
 x9cw_put16(p+at,0x810b);x9cw_put16(p+at+2,1);at+=8;
 assert(at==512);return at;
}
static void rx_progress(unsigned event,unsigned id=7,unsigned mode=20){
 uint8_t p[512];progress_frame(p,event,id,mode);x9mi_rx(p,sizeof(p));
}
static void progress_framing(){
 uint8_t p[520];struct x9cw_progress c;progress_frame(p,130,7,0);
 assert(x9cw_progress(p,512,&c)&&c.valid&&c.id==7&&c.event==130&&c.mask==2&&c.has_cause&&c.cause==16);
 for(unsigned n=0;n<512;n++)assert(!x9cw_progress(p,n,&c));
 p[50]=2;assert(!x9cw_progress(p,512,&c));progress_frame(p,130,7,0);
 x9cw_scalar(p+512,0x8100,1,4);x9cw_put16(p+14,504);assert(!x9cw_progress(p,520,&c));
}
static void outbound_states(){
 struct x9cw_outbound_state s={};struct x9cw_progress c={};unsigned id;
 assert(!x9cw_outbound_id(&s,&id)&&!s.count_known);
 c.id=7;c.mask=2;c.event=130;assert(x9cw_outbound_apply(&s,&c)==1&&s.ios_state==1&&s.epoch==1);
 c.mode=20;c.event=2;assert(x9cw_outbound_apply(&s,&c)==1&&s.ios_state==5);
 c.event=132;assert(x9cw_outbound_apply(&s,&c)==1&&s.ios_state==3);
 c.event=6;assert(x9cw_outbound_apply(&s,&c)==1&&x9cw_outbound_apply(&s,&c)==1);
 assert(x9cw_outbound_id(&s,&id)&&id==7);
 c.event=133;assert(x9cw_outbound_apply(&s,&c)==1&&s.ended&&s.ios_state==9&&!s.count_known);
 c.event=6;assert(x9cw_outbound_apply(&s,&c)==0&&s.ended&&!x9cw_outbound_id(&s,&id));
 c.event=130;c.mode=0;assert(x9cw_outbound_apply(&s,&c)==1&&s.epoch==2);
 c.id=8;assert(x9cw_outbound_apply(&s,&c)==-1&&s.blocked);
 s={};c.id=256;assert(x9cw_outbound_apply(&s,&c)==1&&!x9cw_outbound_id(&s,&id));
 s={};c.mask=0x20;assert(x9cw_outbound_apply(&s,&c)==0&&!s.seen); // no CMD mask normalization
}
/* Complete artificial producer frames, not reconstructed logger prefixes.
 * Removing optional10a changes the declared body and moves the final TLV. */
static unsigned reason_frame(uint8_t *p,unsigned event,unsigned direction=0,
                             unsigned cause=16,bool has_cause=true,unsigned id=7,unsigned mode=20){
 unsigned n=progress_frame(p,event,id,mode);x9cw_scalar(p+24,0x101,direction,4);
 x9cw_scalar(p+496,0x10a,cause,4);
 if(!has_cause){memmove(p+496,p+504,8);n-=8;x9cw_put16(p+14,n-16);}
 return n;
}
static void end_reason_provenance(){
 uint8_t p[512];struct x9cw_progress c={};
 unsigned n=reason_frame(p,133);assert(!x9cw_progress(p,n-1,&c)&&!c.valid);
 assert(!x9cw_end_reason_metadata(&c).known&&!x9cw_end_reason_metadata(nullptr).known);
 assert(x9cw_progress(p,n,&c)&&c.valid);
 auto r=x9cw_end_reason_metadata(&c);assert(r.known&&r.apple==145);
 struct x9cw_progress retained=c;
 assert(!x9cw_progress(p,n-1,&c)&&!memcmp(&c,&retained,sizeof(c))); // failed parse does not grant new provenance
 for(unsigned event:{0u,2u,6u,130u,132u}){
  n=reason_frame(p,event);assert(x9cw_progress(p,n,&c)&&c.valid&&!x9cw_end_reason_metadata(&c).known);
 }
 n=reason_frame(p,133,0,17);assert(x9cw_progress(p,n,&c)&&!x9cw_end_reason_metadata(&c).known);
 n=reason_frame(p,133,0,0,false);assert(x9cw_progress(p,n,&c)&&!c.has_cause&&!x9cw_end_reason_metadata(&c).known);
 n=reason_frame(p,133,0,16,true,7,0);assert(x9cw_progress(p,n,&c)&&!x9cw_end_reason_metadata(&c).known);
 c={};c.event=133;c.mode=20;c.has_cause=1;c.cause=16;
 assert(!x9cw_end_reason_metadata(&c).known); // a manually constructed object is not a parsed frame
}
static void outbound_end_reason(){
 uint8_t p[512];struct x9cw_progress c;struct x9cw_outbound_state s;
 auto begin=[&](){s={};unsigned n=reason_frame(p,130,0,16,true,7,0);
  assert(x9cw_progress(p,n,&c)&&x9cw_outbound_apply(&s,&c)==1&&!s.end_reason.known);};
 for(unsigned which=0;which<4;which++){
  begin();unsigned n=reason_frame(p,133,0,which==1?17:16,which!=2);
  assert(x9cw_progress(p,n,&c));if(which==3)c.valid=0;
  assert(x9cw_outbound_apply(&s,&c)==1&&s.ended&&s.ios_state==9);
  assert(s.end_reason.known==(which==0)&&s.end_reason.apple==(which==0?145:0));
  auto reason=s.end_reason;uint64_t rev=s.revision;
  for(unsigned cause:{17u,16u}){
   n=reason_frame(p,133,0,cause);assert(x9cw_progress(p,n,&c));
   assert(x9cw_outbound_apply(&s,&c)==0&&s.revision==rev&&s.end_reason.known==reason.known&&s.end_reason.apple==reason.apple);
  }
  n=reason_frame(p,130,0,16,true,8,0);assert(x9cw_progress(p,n,&c));
  assert(x9cw_outbound_apply(&s,&c)==1&&s.epoch==2&&!s.end_reason.known&&!s.end_reason.apple);
 }
 for(unsigned which=0;which<5;which++){
  begin();unsigned n=reason_frame(p,133);assert(x9cw_progress(p,n,&c));
  if(which==0)c.id=8;else if(which==1)c.direction=1;else if(which==2)c.mode=0;
  else if(which==3)c.mask=1;else s.revision=UINT64_MAX;
  int result=x9cw_outbound_apply(&s,&c);
  assert(result==(which==3?0:-1)&&!s.end_reason.known&&!s.end_reason.apple);
 }
}
static void incoming_end_reason(){
 uint8_t p[512];struct x9cw_progress c;struct x9cw_incoming cmd;
 incoming(p);assert(x9cw_incoming(p,64,&cmd));cmd.call_id=7;
 struct x9cw_inbound_state s;
 auto begin=[&](){s={};assert(x9cw_inbound_begin(&s,&cmd)==1);
  unsigned n=reason_frame(p,0,1);assert(x9cw_progress(p,n,&c));
  assert(x9cw_inbound_apply(&s,&c)==1&&!s.end_reason.known);};
 for(unsigned which=0;which<4;which++){
  begin();unsigned n=reason_frame(p,133,1,which==1?17:16,which!=2);
  assert(x9cw_progress(p,n,&c));if(which==3)c.valid=0;
  assert(x9cw_inbound_apply(&s,&c)==1&&s.ended&&s.ios_state==9);
  assert(s.end_reason.known==(which==0)&&s.end_reason.apple==(which==0?145:0));
  auto reason=s.end_reason;uint64_t rev=s.revision;
  for(unsigned cause:{17u,16u}){
   n=reason_frame(p,133,1,cause);assert(x9cw_progress(p,n,&c));
   assert(x9cw_inbound_apply(&s,&c)==0&&s.revision==rev&&s.end_reason.known==reason.known&&s.end_reason.apple==reason.apple);
  }
  cmd.sequence++;assert(x9cw_inbound_begin(&s,&cmd)==1&&s.pending&&!s.end_reason.known&&!s.end_reason.apple);
 }
 for(unsigned which=0;which<5;which++){
  begin();unsigned n=reason_frame(p,133,1);assert(x9cw_progress(p,n,&c));
  if(which==0)c.id=8;else if(which==1)c.direction=0;else if(which==2)c.mode=0;
  else if(which==3)c.mask=1;else s.revision=UINT64_MAX;
  int result=x9cw_inbound_apply(&s,&c);
  assert(result==((which==1||which==3)?0:-1)&&!s.end_reason.known&&!s.end_reason.apple);
 }
 // Complete END cannot attach a cause to an MT that has not observed ECPI0.
 s={};assert(x9cw_inbound_begin(&s,&cmd)==1);unsigned n=reason_frame(p,133,1);
 assert(x9cw_progress(p,n,&c)&&x9cw_inbound_apply(&s,&c)==-1&&!s.end_reason.known);
}
static unsigned current_frame(uint8_t *p,unsigned count,unsigned result=0){
 assert(count<=1);x9cw_header(p,704,0x90a,2,0);unsigned at=32;
 x9cw_scalar(p+16,0,result,4);x9cw_scalar(p+24,0x100,count,4);
 const unsigned tags[]={0x10b,0x10c,0x10d,0x10e,0x10f,0x110,0x8111,0x112,0x113,0x8114,0x115};
 const uint32_t words[]={7,0,20,0,0,129,0,0,0x100000,0,0};
 if(count)for(unsigned j=0;j<11;j++){
  unsigned base=(0x7000-j*8)|(tags[j]&0x8000);
  x9cw_put16(p+at,tags[j]);x9cw_put16(p+at+12,base);x9cw_put16(p+at+14,count);at+=16;
  if(j==6){x9cw_put16(p+at,base);x9cw_put16(p+at+2,1);at+=8;}
  else if(j==9){x9cw_put16(p+at,base);x9cw_put16(p+at+2,408);p[at+4]=0xff;at+=416;}
  else{x9cw_scalar(p+at,base,words[j],4);at+=8;}
 }
 x9cw_put16(p+14,at-16);return at;
}
static void current_arrays(){
 uint8_t p[712];struct x9cw_current c;unsigned n=current_frame(p,1);
 assert(n==704&&x9cw_current(p,n,&c)&&c.count==1&&c.calls[0].id==7&&c.calls[0].mode==20);
 for(unsigned k=0;k<n;k++)assert(!x9cw_current(p,k,&c));
 p[44]^=0x80;assert(!x9cw_current(p,n,&c));n=current_frame(p,1);
 p[46]=2;assert(!x9cw_current(p,n,&c));n=current_frame(p,1);
 p[36]=1;assert(!x9cw_current(p,n,&c));n=current_frame(p,1);
 x9cw_scalar(p+n,0x7001,1,4);x9cw_put16(p+14,n-8);assert(!x9cw_current(p,n+8,&c));
 n=current_frame(p,0);assert(n==32&&x9cw_current(p,n,&c)&&!c.count&&!c.result);
 n=current_frame(p,0,42);assert(x9cw_current(p,n,&c)&&c.result==42);
}
static void current_number_owner(){
 uint8_t p[704],out[704];struct x9cw_current c;unsigned n=current_frame(p,1);
 struct x9cw_number missing={};unsigned untouched=99;
 assert(!x9cw_number_presentation(&missing,&untouched)&&untouched==99);
 const uint8_t *descriptor,*number,*detail;unsigned k;
 assert(x9cw_tlv(p,n,0x8111,&descriptor,&k)&&descriptor&&!k);
 assert(x9cw_tlv(p,n,x9cw_u16(descriptor+8),&number,&k)&&number&&k==1);
 unsigned at=(unsigned)(number-p);x9cw_put16(p+at-2,4);memcpy(p+at,"123",4);
 assert(x9cw_tlv(p,n,0x8114,&descriptor,&k)&&descriptor&&!k);
 assert(x9cw_tlv(p,n,x9cw_u16(descriptor+8),&detail,&k)&&detail&&k==408);
 p[detail-p]=1;assert(x9cw_current(p,n,&c));
 assert(c.calls[0].number.length==3&&!memcmp(c.calls[0].number.value,"123",4));
 unsigned presentation=99;assert(x9cw_number_presentation(&c.calls[0].number,&presentation)&&presentation==1);
 bzero(p,sizeof(p));assert(!memcmp(c.calls[0].number.value,"123",4)); // no borrowed RX bytes
 for(unsigned raw=0;raw<256;raw++){
  c.calls[0].number.presentation=(uint8_t)raw;presentation=99;
  int known=x9cw_number_presentation(&c.calls[0].number,&presentation);
  assert(known==(raw<=4));assert(presentation==(raw<=4?(raw<2?raw:2):99));
 }
 // The production owned-current path, not an unsolicited or stale query.
 x9cw_header(p,16,0x909,2,0);uint64_t token=x9_md_mipc_submit(p,16);assert(token);x9mi_tick();
 n=current_frame(p,1);x9cw_put16(p+12,(unsigned)token);x9mi_rx(p,n);
 uint32_t got;assert(x9_md_mipc_take(token,out,sizeof(out),&got)==1);
 assert(x9_mc_current.count==1&&x9_mc_current.calls[0].number.presentation==255);
 assert(!x9cw_number_presentation(&x9_mc_current.calls[0].number,&presentation));
}
static void progress_number_owner(){
 uint8_t p[512];struct x9cw_progress c;progress_frame(p,130,7,0);
 x9cw_put16(p+50,4);memcpy(p+52,"456",4);p[68]=2; //8106/detail_info[0], not TYPE105
 assert(x9cw_progress(p,sizeof(p),&c)&&c.number.length==3&&!memcmp(c.number.value,"456",4));
 unsigned presentation;assert(x9cw_number_presentation(&c.number,&presentation)&&presentation==2);
 bzero(p,sizeof(p));assert(!memcmp(c.number.value,"456",4));
 progress_frame(p,130,7,0);p[50]=5; // would consume next TLV: malformed whole frame
 assert(!x9cw_progress(p,sizeof(p),&c));assert(!memcmp(c.number.value,"456",4));
}
static void empty_baseline(){
 uint8_t p[704],out[704];x9cw_header(p,16,0x909,2,0);
 uint64_t t=x9_md_mipc_submit(p,16);assert(t);x9mi_tick();
 unsigned n=current_frame(p,0);x9cw_put16(p+12,(unsigned)t);x9mi_rx(p,n);
 uint32_t k;assert(x9_md_mipc_take(t,out,sizeof(out),&k)==1&&k==32);
 struct x9cw_outbound_state s;assert(x9_md_mipc_call_snapshot(&s)==1&&s.count_known&&!s.native_count);
}
static void status_fence(){
 uint8_t p[704],out[704];x9cw_header(p,16,0x909,2,0);
 uint64_t t=x9_md_mipc_submit(p,16);assert(t);x9mi_tick();rx_progress(130,7,0);
 unsigned n=current_frame(p,0);x9cw_put16(p+12,(unsigned)t);x9mi_rx(p,n);
 uint32_t k;assert(x9_md_mipc_take(t,out,sizeof(out),&k)==1);
 assert(!x9_mc_outbound.count_known&&!x9_mc_outbound.blocked&&x9_mc_outbound.seen);
}
static void dial_join_order(){
 uint8_t q[64],p[24],r[18];unsigned n=dial_qmi(q);struct x9cw_outbound_state s={};
 struct x9cw_dial_join j={};assert(!x9cw_dial_join_begin(&j,&s,&measured_route));
 s.count_known=1;assert(x9cw_dial_join_begin(&j,&s,&measured_route)&&x9cw_dial_join_sent(&j,77));
 x9cw_header(p,24,0x902,2,76);x9cw_scalar(p+16,0,0,4);
 assert(!x9cw_dial_join_cnf(&j,p,24)&&!j.confirmed);
 x9cw_put16(p+12,77);assert(x9cw_dial_join_cnf(&j,p,24));
 assert(!x9cw_dial_join_response(&j,&s,q,n,r,18)); // CNF alone is not success
 struct x9cw_progress c={};c.id=7;c.mask=2;c.event=130;assert(x9cw_outbound_apply(&s,&c)==1);
 assert(x9cw_dial_join_response(&j,&s,q,n,r,18)==18&&r[0]==2&&r[14]==0x10&&r[17]==7);
 assert(x9cw_u16(r+1)==x9cw_u16(q+1)&&x9cw_u16(r+3)==0x20&&x9cw_u16(r+5)==11);
}
static void ios_join(){
 empty_baseline();uint8_t q[64],p[24];unsigned n=dial_qmi(q);x9q_clients[0].sim_slot=1;
 assert(x9c_dial_submit(0,q,n,measured_route));uint64_t t=x9c_dial_job.token;x9mi_tick();
 assert(!x9_md_mipc_submit(last_frame,last_length)); // staging lease blocks a second mutation
 x9cw_header(p,24,0x902,2,(unsigned)t);x9cw_scalar(p+16,0,0,4);x9mi_rx(p,24);x9c_poll();
 assert(!qmi_replies&&x9c_dial_job.used);rx_progress(130,7,0);x9c_poll();
 assert(qmi_replies==1&&qmi_response_len==18&&qmi_response[17]==7&&x9c_dial_job.used);
 x9c_poll();assert(qmi_replies==1&&!x9c_dial_submit(0,q,n,measured_route));
}
static void ios_timeout(){
 empty_baseline();uint8_t q[64],p[24];unsigned n=dial_qmi(q);x9q_clients[0].sim_slot=1;
 assert(x9c_dial_submit(0,q,n,measured_route));uint64_t t=x9c_dial_job.token;x9mi_tick();
 fake_ms=60001;x9c_poll();assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&x9c_dial_job.token==t);
 x9cw_header(p,24,0x902,2,(unsigned)t);x9cw_scalar(p+16,0,0,4);x9mi_rx(p,24);
 rx_progress(130,7,0);x9c_poll();assert(qmi_replies==1&&!x9c_dial_job.token&&x9c_dial_job.held);
}
static void ios_deleted(){
 empty_baseline();uint8_t q[64],p[24];unsigned n=dial_qmi(q);x9q_clients[0].sim_slot=1;
 assert(x9c_dial_submit(0,q,n,measured_route));uint64_t t=x9c_dial_job.token;x9mi_tick();
 x9c_del(0);x9q_clients[0].generation++;x9cw_header(p,24,0x902,2,(unsigned)t);x9cw_scalar(p+16,0,0,4);
 x9mi_rx(p,24);rx_progress(130,7,0);x9c_poll();assert(!qmi_replies&&!x9c_dial_job.token&&x9c_dial_job.used);
}
static void dial_presend_fence(){
 empty_baseline();uint8_t q[64],out[24];unsigned n=dial_qmi(q);x9q_clients[0].sim_slot=1;
 assert(x9c_dial_submit(0,q,n,measured_route));uint64_t t=x9c_dial_job.token;unsigned before=writes;
 rx_progress(130,7,0);x9mi_tick();assert(writes==before);uint32_t k;
 assert(x9_md_mipc_take(t,out,sizeof(out),&k)==-3&&!k);
 assert(x9_md_mipc_take(t,out,sizeof(out),&k)==-1&&x9_mc_dial_token==t);
}
/* Manual vectors compiled with the real production adapter/MD writer.
 * No device identifiers or fixtures; execution is not part of the build. */
static void voice_bind_codec(){
 uint8_t q[14]={0,77,0,0x44,0,4,0,1,1,0,1};unsigned sim=9;
 assert(x9cw_qmi_bind(q,11,&sim)&&sim==1);
 q[10]=0;assert(x9cw_qmi_bind(q,11,&sim)&&!sim);
 q[10]=2;sim=9;assert(!x9cw_qmi_bind(q,11,&sim)&&sim==9);
 q[10]=1;q[8]=4;q[5]=7;assert(!x9cw_qmi_bind(q,14,&sim)); // not u32 NAS/DMS enum
}
static void bind_voice(){
 uint8_t q[]={0,77,0,0x44,0,4,0,1,1,0,1};
 assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q))&&qmi_error==0);
 assert(x9c_clients[0].bound&&x9c_clients[0].sim==1&&!x9q_clients[0].sim_slot&&!writes);
 qmi_replies=0;
}
static void end_request(){
 uint8_t q[]={0,78,0,0x21,0,4,0,1,1,0,7};
 assert(x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q))&&x9c_end_job.stage==1);
 assert(!qmi_replies);
}
static void end_query_cnf(){
 uint8_t p[704];unsigned n=current_frame(p,1);x9cw_put16(p+12,(unsigned)x9c_end_job.token);
 x9mi_rx(p,n);x9c_poll();assert(x9c_end_job.stage==2&&x9c_end_job.token&&x9_mc_end_token);
}
static void end_target_codec(){
 uint8_t p[704];struct x9cw_current c;current_frame(p,1);assert(x9cw_current(p,704,&c));
 struct x9cw_outbound_state s={};struct x9cw_progress e={};struct x9cw_end_target a,b;
 e.mask=2;e.id=7;e.event=130;assert(x9cw_outbound_apply(&s,&e)==1);
 e.event=132;e.mode=20;assert(x9cw_outbound_apply(&s,&e)==1);
 assert(!x9cw_end_target(&s,&c,&a));s.count_known=1;s.native_count=1;
 assert(x9cw_end_target(&s,&c,&a)&&a.native_id==7&&a.sim==1&&a.epoch==1);
 b=a;b.revision++;assert(!x9cw_end_matches(&a,&b));
 c.calls[0].flags|=1;assert(!x9cw_end_target(&s,&c,&b));c.calls[0].flags&=~1u;
 c.calls[0].number_type=2;assert(x9cw_end_target(&s,&c,&b)); // NUMBER is not a multiparty flag
 c.calls[0].id=8;assert(!x9cw_end_target(&s,&c,&b));c.calls[0].id=7;
 c.calls[0].direction=1;assert(!x9cw_end_target(&s,&c,&b));c.calls[0].direction=0;
 c.calls[0].state=3;assert(!x9cw_end_target(&s,&c,&b));c.calls[0].state=0;
 s.ended=1;assert(!x9cw_end_target(&s,&c,&b));
 uint8_t r[24];uint32_t result=99;x9cw_header(r,24,0x906,2,41);x9cw_scalar(r+16,0,0,4);
 assert(x9cw_end_cnf(r,24,41,&result)&&!result);
 assert(!x9cw_end_cnf(r,24,40,&result));r[10]=8;assert(!x9cw_end_cnf(r,24,41,&result));
}
static void current_metadata(){
 uint8_t p[704];struct x9cw_current c;unsigned n=current_frame(p,1);
 /* Change the three actual array element values, keeping descriptors,
  * ID and state unchanged. Each remains a u32 in the MIPC wire. */
 for(unsigned at=16;at<n;){
  unsigned tag=x9cw_u16(p+at),len=x9cw_u16(p+at+2);
  if(tag==0x6fe0)x9cw_put32(p+at+4,2);       //10f NUMBER_TYPE
  if(tag==0x6fc8)x9cw_put32(p+at+4,6);       //112 CALL_RAT
  if(tag==0x6fc0)x9cw_put32(p+at+4,0x100001);//113 TYPE: high bits retained and multiparty
  at+=len?((len+11)&~7u):16;
 }
 assert(x9cw_current(p,n,&c)&&c.calls[0].number_type==2&&c.calls[0].rat==6&&c.calls[0].flags==0x100001);
 struct x9cw_outbound_state s={};struct x9cw_progress e={};struct x9cw_end_target target;
 e.mask=2;e.id=7;e.event=130;assert(x9cw_outbound_apply(&s,&e)==1);
 e.event=132;e.mode=20;assert(x9cw_outbound_apply(&s,&e)==1);s.count_known=1;s.native_count=1;
 assert(!x9cw_end_target(&s,&c,&target));c.calls[0].number_type=0;
 assert(!x9cw_end_target(&s,&c,&target)); // NONE cannot authorize a multiparty end
 c.calls[0].flags&=~1u;c.calls[0].number_type=2;assert(x9cw_end_target(&s,&c,&target));
}
static void ios_end_roundtrip(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();x9mi_tick();
 assert(writes==1&&x9cw_u16(last_frame+10)==0x909);end_query_cnf();
 uint64_t token=x9c_end_job.token;x9mi_tick();assert(writes==2&&last_length==32&&
  x9cw_u16(last_frame+10)==0x905&&x9cw_u32(last_frame+20)==0&&x9cw_u32(last_frame+28)==7);
 rx_progress(133);x9c_poll();assert(!qmi_replies&&x9c_end_job.stage==2);
 uint8_t p[704];x9cw_header(p,24,0x906,0x22,(unsigned)token);x9cw_scalar(p+16,0,0,4);
 x9mi_rx(p,24);x9c_poll();assert(qmi_replies==1&&!qmi_error&&x9c_end_job.stage==4&&x9_mc_end_token==token);
 token=x9c_end_job.token;x9mi_tick();assert(writes==3&&x9cw_u16(last_frame+10)==0x909);
 unsigned n=current_frame(p,0);x9cw_put16(p+12,(unsigned)token);x9mi_rx(p,n);x9c_poll();
 assert(qmi_replies==1&&!x9c_end_job.used&&!x9_mc_end_token&&x9_mc_outbound.ended&&x9_mc_outbound.count_known);
}
static void ios_end_stale_query(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();uint64_t token=x9c_end_job.token;x9mi_tick();
 rx_progress(6);uint8_t p[704];unsigned n=current_frame(p,1);x9cw_put16(p+12,(unsigned)token);
 x9mi_rx(p,n);x9c_poll();assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!x9c_end_job.used&&!x9_mc_end_token);
 x9mi_tick();assert(writes==1); // stale query cannot authorize a hangup
}
static void ios_end_presend(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();x9mi_tick();end_query_cnf();
 rx_progress(6);x9mi_tick();x9c_poll();assert(writes==1&&!x9c_end_job.used&&!x9_mc_end_token&&
  qmi_replies==1&&qmi_error==X9Q_NOT_READY);
}
static void ios_end_duplicate(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();uint64_t query=x9c_end_job.token;
 end_request();assert(x9c_end_job.token==query&&!qmi_replies&&!writes);
 x9mi_tick();end_query_cnf();uint64_t token=x9c_end_job.token;
 uint8_t q[]={0,78,0,0x21,0,4,0,1,1,0,7};
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));
 assert(x9c_end_job.token==token&&!qmi_replies);x9mi_tick();
 uint8_t p[24];x9cw_header(p,24,0x906,2,(unsigned)token);x9cw_scalar(p+16,0,0,4);
 x9mi_rx(p,24);x9c_poll();assert(qmi_replies==1&&!qmi_error&&x9c_end_job.used&&!x9c_end_job.token);
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));
 assert(qmi_replies==1&&writes==2); // no BUSY + late-success pair or second mutation
}
static void ios_end_abandon(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();x9mi_tick();end_query_cnf();
 x9c_del(0);x9q_clients[0].generation++;x9mi_tick();x9c_poll();
 assert(writes==1&&!qmi_replies&&!x9c_end_job.used&&!x9_mc_end_token&&!x9c_clients[0].bound);
}
static void ios_end_late_cnf(){
 bind_voice();rx_progress(130,7,0);rx_progress(132);end_request();x9mi_tick();end_query_cnf();
 uint64_t token=x9c_end_job.token;x9mi_tick();x9c_del(0);x9q_clients[0].generation++;
 rx_progress(133);uint8_t p[704];x9cw_header(p,24,0x906,2,(unsigned)token);x9cw_scalar(p+16,0,0,4);
 x9mi_rx(p,24);x9c_poll();assert(!qmi_replies&&x9c_end_job.stage==4&&x9_mc_end_token==token);
 token=x9c_end_job.token;x9mi_tick();unsigned n=current_frame(p,0);x9cw_put16(p+12,(unsigned)token);
 x9mi_rx(p,n);x9c_poll();assert(!qmi_replies&&!x9c_end_job.used&&!x9_mc_end_token);
}
static void incoming_states(){
 uint8_t p[64];incoming(p);struct x9cw_incoming c;
 assert(x9cw_incoming(p,64,&c));struct x9cw_inbound_state s={};unsigned id;
 assert(x9cw_inbound_begin(&s,&c)==1&&s.pending&&!s.seen&&!s.ios_state&&!s.epoch);
 assert(!x9cw_inbound_id(&s,&id));
 struct x9cw_progress e={};e.id=c.call_id;e.direction=1;e.mode=20;e.mask=2;e.event=0;
 assert(x9cw_inbound_apply(&s,&e)==1&&s.seen&&!s.pending&&s.ios_state==2&&s.epoch==1);
 assert(!x9cw_inbound_id(&s,&id)); // real u32 id cannot be truncated to Apple u8
 s.native_id=e.id=7;assert(x9cw_inbound_id(&s,&id)&&id==7);
 uint64_t rev=s.revision;assert(x9cw_inbound_apply(&s,&e)==0&&s.revision==rev);
 e.event=132;assert(x9cw_inbound_apply(&s,&e)==1&&s.ios_state==3);
 e.event=6;assert(x9cw_inbound_apply(&s,&e)==1&&s.ios_state==3);
 assert(x9cw_inbound_apply(&s,&e)==1&&s.ios_state==3);
 e.event=133;e.has_cause=1;e.cause=16;
 assert(x9cw_inbound_apply(&s,&e)==1&&s.ended&&s.ios_state==9&&s.cause==16);
 assert(!x9cw_inbound_id(&s,&id));rev=s.revision;
 for(unsigned evt:{0u,132u,6u,133u}){e.event=evt;assert(x9cw_inbound_apply(&s,&e)==0&&s.revision==rev&&s.ended);}
 c.sequence++;c.call_id=8;
 assert(x9cw_inbound_begin(&s,&c)==1&&s.pending&&!s.seen&&s.epoch==1);
 e.id=8;e.event=0;assert(x9cw_inbound_apply(&s,&e)==1&&s.epoch==2&&s.ios_state==2);
}
static void incoming_state_invalid(){
 uint8_t p[64];incoming(p);struct x9cw_incoming c;assert(x9cw_incoming(p,64,&c));
 struct x9cw_inbound_state s={};struct x9cw_progress e={};
 e.id=c.call_id;e.direction=1;e.mode=20;e.mask=2;e.event=0;
 assert(x9cw_inbound_apply(&s,&e)<0&&s.blocked); // no complete CMD identity
 s={};assert(x9cw_inbound_begin(&s,&c)==1);e.event=132;
 assert(x9cw_inbound_apply(&s,&e)<0&&s.blocked); // no observed INCOMING transition
 s={};assert(x9cw_inbound_begin(&s,&c)==1);e.event=0;e.id++;
 assert(x9cw_inbound_apply(&s,&e)<0&&s.blocked);e.id--;
 s={};assert(x9cw_inbound_begin(&s,&c)==1);assert(x9cw_inbound_begin(&s,&c)<0&&s.blocked);
 s={};c.mode=22;assert(x9cw_inbound_begin(&s,&c)<0&&s.blocked);c.mode=20;
 s={};c.sim=0;assert(x9cw_inbound_begin(&s,&c)==0&&!s.pending&&!s.blocked);c.sim=1;
 s={};assert(x9cw_inbound_begin(&s,&c)==1);assert(x9cw_inbound_apply(&s,&e)==1);
 e.event=132;assert(x9cw_inbound_apply(&s,&e)==1);e.event=0;
 assert(x9cw_inbound_apply(&s,&e)<0&&s.blocked); // no active -> incoming regression
 s={};s.revision=UINT64_MAX;assert(x9cw_inbound_begin(&s,&c)<0&&s.blocked);
}
static void incoming_observer(){
 uint8_t p[512];incoming(p);x9cw_scalar(p+16,0x100,7,4);x9mi_rx(p,64);x9mi_rx(p,64);
 assert(x9_mc_inbound.pending&&x9_mc_next==1&&!x9_mc_inbound.blocked&&!writes);
 struct x9cw_inbound_state s;assert(x9_md_mipc_call_incoming_snapshot(&s)==1&&s.pending);
 for(unsigned evt:{0u,132u,6u,6u,133u}){
  unsigned n=progress_frame(p,evt,7,20);x9cw_scalar(p+24,0x101,1,4);x9mi_rx(p,n);x9c_poll();
  assert(x9c_incoming_observed&&x9c_inbound.native_id==7&&!x9c_inbound.blocked);
 }
 assert(x9c_inbound.ended&&x9c_inbound.ios_state==9&&x9c_inbound.epoch==1);
 assert(!x9_mc_outbound.seen&&x9_mc_outbound.blocked&&!x9_mc_outbound.count_known);
 x9mi_tick();assert(!writes&&!qmi_replies); // no auto-approval, answer or publication
}
static void register_codec(){
 uint8_t q[15]={0,9,0,3,0,8,0,0x13,1,0,0,0x14,1,0,1};struct x9cw_voice_register r;
 assert(x9cw_qmi_register(q,15,&r)&&r.present==3&&!r.call_events&&r.handover==1);
 q[11]=0x13;assert(!x9cw_qmi_register(q,15,&r));q[11]=0x14;
 q[12]=4;assert(!x9cw_qmi_register(q,15,&r));q[12]=1;
 q[14]=2;assert(!x9cw_qmi_register(q,15,&r));
 uint8_t frame[24];assert(x9cw_call_register(frame,24,0x8901,1,2)==24&&frame[8]==0xff);
 assert(x9cw_call_register_request(frame,24,2)&&x9_md_mipc_submit(frame,24));
 frame[8]=2;assert(!x9cw_call_register_request(frame,24,2)&&!x9_md_mipc_submit(frame,24));
 assert(x9cw_call_register(frame,24,0x4901,1,2)==24&&frame[8]==2);
 assert(x9cw_call_register_request(frame,24,2));frame[23]=1;
 assert(!x9cw_call_register_request(frame,24,2)&&!x9_md_mipc_submit(frame,24));
 assert(!x9cw_call_register(frame,24,0x491f,1,2));
}
static void register_transition(){
 uint8_t p[32];x9cw_header(p,24,0x308,0,9);x9cw_scalar(p+16,0,0,4);
 struct x9cw_call_init s={};s.stage=X9_CI_STATUS;s.token=9;s.deadline=100;
 assert(x9cw_call_init_cnf(&s,p,24,1)==1&&s.stage==X9_CI_EVENT&&!s.registered&&!s.token);
 s.token=10;x9cw_put16(p+12,10);assert(x9cw_call_init_cnf(&s,p,24,2)==1&&s.registered&&!s.baseline_known);
 s={};s.stage=X9_CI_STATUS;s.token=9;s.deadline=100;x9cw_put16(p+12,9);
 assert(x9cw_call_init_cnf(&s,p,24,100)<0&&s.held&&!s.registered&&!s.token);
 s={};s.stage=X9_CI_STATUS;s.token=10;s.deadline=100;
 assert(x9cw_call_init_cnf(&s,p,24,1)<0&&s.held); // cannot borrow another token
 x9cw_header(p,32,0x308,0,7);x9cw_scalar(p+16,0,21,4);x9cw_scalar(p+24,0x100,0,1);
 uint32_t r;assert(x9cw_call_register_cnf(p,32,0x307,7,&r)&&r==21);
 p[20]=0;assert(!x9cw_call_register_cnf(p,32,0x307,7,&r)); // failure TLV is not success
}
static void register_begin(unsigned slot=0){
 x9w_control_ready=true;uint8_t q[]={0,90,0,3,0,4,0,0x13,1,0,0};
 assert(x9c_request(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+9,q,sizeof(q)));
}
static void register_cnf(uint64_t token,unsigned request,unsigned result=0){
 uint8_t p[24];x9cw_header(p,24,request+1,0,(unsigned)token);x9cw_scalar(p+16,0,result,4);
 x9mi_rx(p,24);x9c_poll();
}
static void register_sequence(){
 bind_voice();x9c_poll();x9mi_tick();assert(!writes&&!x9c_cmd_init.token); //44 alone sends nothing
 register_begin();x9c_poll();uint64_t token=x9c_cmd_init.token;assert(token&&!qmi_replies);
 x9mi_tick();assert(writes==1&&last_frame[8]==0xff&&x9cw_u16(last_frame+20)==0x8901);
 register_cnf(token,0x30b);assert(x9c_cmd_init.confirmed&&x9c_init[1].stage==X9_CI_STATUS);
 for(unsigned id:{0x4901u,0x4902u}){
  token=x9c_init[1].token;x9mi_tick();assert(last_frame[8]==2&&x9cw_u16(last_frame+20)==id);
  register_cnf(token,0x307);assert(!qmi_replies);
 }
 assert(x9c_init[1].registered&&!x9c_init[1].baseline_known);
 token=x9c_init[1].token;x9mi_tick();assert(x9cw_u16(last_frame+10)==0x909&&last_length==16);
 uint8_t p[704];unsigned n=current_frame(p,0);x9cw_put16(p+12,(unsigned)token);
 x9mi_rx(p,n);x9c_poll();assert(qmi_replies==1&&!qmi_error&&x9c_clients[0].registered&&x9c_init[1].baseline_known);
 assert(writes==4&&!x9c_init[0].stage&&!x9c_init[0].registered);
 unsigned before=qmi_replies;register_begin();x9c_poll();x9mi_tick();assert(qmi_replies==before&&writes==4);
}
static void register_timeout(){
 bind_voice();register_begin();x9c_poll();uint64_t token=x9c_cmd_init.token;x9mi_tick();
 fake_ms=25000;x9c_poll();assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&x9c_cmd_init.held);
 register_cnf(token,0x30b);assert(!x9c_cmd_init.token&&!x9c_cmd_init.confirmed&&!x9c_init[1].token);
 x9mi_tick();assert(writes==1&&qmi_replies==1&&!x9c_clients[0].registered);
}
static void register_deleted(){
 bind_voice();register_begin();x9c_poll();uint64_t token=x9c_cmd_init.token;x9mi_tick();
 x9c_del(0);x9q_clients[0].generation++;register_cnf(token,0x30b);x9mi_tick();
 assert(writes==1&&!qmi_replies&&x9c_cmd_init.confirmed&&!x9c_init[1].token);
}
static void register_unsupported(){
 uint8_t q[]={0,90,0,3,0,4,0,0x13,1,0,1};bind_voice();
 x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));assert(qmi_error==X9Q_UNSUPPORTED);
 q[7]=0x13;q[10]=0;x9c_del(0);x9c_request(CBQ_PORT_BASE,X9BB_PORT_BASE+9,q,sizeof(q));
 assert(qmi_error==X9Q_NOT_READY);x9c_poll();x9mi_tick();assert(!writes&&!x9c_cmd_init.token);
}
static unsigned handover_frame(uint8_t *p,unsigned state,unsigned mask=2){
 x9cw_header(p,32,0x4902,mask,0);x9cw_scalar(p+16,0x100,3,4);
 x9cw_scalar(p+24,0x115,state,4);return 32;
}
static void handover_codec(){
 uint8_t p[40],q[11];struct x9cw_handover e;
 const unsigned mapped[]={1,3,2,4};
 for(unsigned state=0;state<4;state++){
  handover_frame(p,state);assert(x9cw_handover(p,32,&e)==1&&e.sim==1&&e.native_state==state&&e.ios_state==mapped[state]);
  assert(x9cw_handover_indication(q,11,e.ios_state)==11);
  const uint8_t expected[]={4,0,0,0x54,0,4,0,1,1,0,(uint8_t)mapped[state]};
  assert(!memcmp(q,expected,11));
 }
 for(unsigned high=0;high<=0xf0;high+=0x10)for(unsigned low=1;low<=2;low++){
  handover_frame(p,0,high|low);assert(x9cw_handover(p,32,&e)==1&&e.sim==low-1);
 }
 handover_frame(p,4);assert(x9cw_handover(p,32,&e)<0);
 for(unsigned mask:{0u,0x20u,3u,4u,8u,0xffu}){handover_frame(p,0,mask);assert(x9cw_handover(p,32,&e)<0);}
 handover_frame(p,0);p[26]=1;assert(x9cw_handover(p,32,&e)<0);
 handover_frame(p,0);p[24]=0;assert(x9cw_handover(p,32,&e)<0);
 handover_frame(p,0);x9cw_scalar(p+32,0x116,0,4);x9cw_put16(p+14,24);assert(x9cw_handover(p,40,&e)<0);
 handover_frame(p,0);p[20]=6;assert(x9cw_handover(p,32,&e)==0); // RING is not SRVCC
 assert(!x9cw_handover_indication(q,10,1)&&!x9cw_handover_indication(q,11,0)&&!x9cw_handover_indication(q,11,10));
}
static void handover_begin(unsigned slot=0,unsigned value=1,unsigned transaction=91){
 uint8_t q[]={0,(uint8_t)transaction,0,3,0,4,0,0x14,1,0,(uint8_t)value};
 x9c_request(CBQ_PORT_BASE+slot,X9BB_PORT_BASE+9,q,sizeof(q));
}
static void handover_concurrent_bootstrap(){
 bind_voice();register_begin();handover_begin();assert(!qmi_replies);
 x9c_poll();x9mi_tick();register_cnf(x9c_cmd_init.token,0x30b);
 for(unsigned id:{0x4901u,0x4902u}){
  uint64_t token=x9c_init[1].token;x9mi_tick();assert(x9cw_u16(last_frame+20)==id);register_cnf(token,0x307);
 }
 uint64_t token=x9c_init[1].token;x9mi_tick();
 uint8_t p[704];handover_frame(p,0);x9mi_rx(p,32);handover_frame(p,1);x9mi_rx(p,32);x9c_poll();
 assert(!qmi_replies&&!qmi_indications&&x9c_reg_wait[0][1].event_count==2);
 unsigned n=current_frame(p,0);x9cw_put16(p+12,(unsigned)token);x9mi_rx(p,n);x9c_poll();
 assert(qmi_replies==4&&qmi_indications==2&&qmi_indication_states[0]==1&&qmi_indication_states[1]==3);
 assert(x9c_clients[0].registered&&x9c_clients[0].handover&&writes==4);
 assert(!x9c_init[1].baseline_known&&x9_mc_outbound.blocked); // handover crossed snapshot
 handover_begin(0,0,92);x9c_poll();assert(!x9c_clients[0].handover);
 handover_frame(p,2);x9mi_rx(p,32);x9c_poll();assert(qmi_indications==2);
 register_begin();x9c_poll();assert(!x9c_clients[0].handover); //13 alone never enables14
}
static void handover_subscription_fence(){
 bind_voice();x9w_control_ready=true;
 x9c_cmd_init.confirmed=true;x9c_init[1].stage=X9_CI_DONE;x9c_init[1].registered=1;
 uint8_t p[32];handover_frame(p,0);x9mi_rx(p,32); // already queued at acceptance
 handover_begin();x9c_poll();assert(qmi_replies==1&&!qmi_indications&&x9c_clients[0].handover);
 handover_frame(p,1);x9mi_rx(p,32);x9c_poll();assert(qmi_indications==1&&qmi_indication_states[0]==3);
 handover_frame(p,0);x9mi_rx(p,32);x9c_del(0);x9q_clients[0].generation++;bind_voice();
 handover_begin();x9c_poll();assert(qmi_indications==1); // no queued event replay to new generation
 handover_frame(p,3,1);x9mi_rx(p,32);x9c_poll();assert(qmi_indications==1); // independent SIM
 handover_frame(p,3);x9mi_rx(p,32);x9c_poll();assert(qmi_indications==2&&qmi_indication_states[1]==4);
}
static void handover_pending_timeout(){
 bind_voice();x9w_control_ready=true;handover_begin();x9c_poll();x9mi_tick();
 uint8_t p[32];handover_frame(p,0);x9mi_rx(p,32);x9c_poll();assert(!qmi_indications);
 uint64_t token=x9c_cmd_init.token;fake_ms=25000;x9c_poll();register_cnf(token,0x30b);
 assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!qmi_indications&&!x9c_clients[0].handover);
 assert(!x9c_reg_wait[0][0].event_count&&x9c_cmd_init.held);
}
static void handover_pending_overflow(){
 bind_voice();x9w_control_ready=true;handover_begin();x9c_poll();x9mi_tick();
 uint8_t p[32];
 for(unsigned i=0;i<8;i++){
  handover_frame(p,i&1);x9mi_rx(p,32);x9c_poll();
  assert(!qmi_replies&&!qmi_indications&&x9c_reg_wait[0][0].event_count==i+1);
 }
 handover_frame(p,0);x9mi_rx(p,32);x9c_poll();
 assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!qmi_indications&&!x9c_clients[0].handover);
 assert(!x9c_reg_wait[0][0].event_count&&!x9_mc_poisoned);
 register_cnf(x9c_cmd_init.token,0x30b);assert(qmi_replies==1&&!x9c_init[1].token);
}
static void handover_transport_loss(){
 bind_voice();x9w_control_ready=true;handover_begin();x9c_poll();x9mi_tick();
 uint64_t token=x9c_cmd_init.token;uint8_t p[32];
 handover_frame(p,0);x9mi_rx(p,32);x9c_poll();assert(x9c_reg_wait[0][0].event_count==1);
 handover_frame(p,4);x9mi_rx(p,32);assert(x9_mc_poisoned&&x9_md_mipc_online());
 x9c_poll();assert(qmi_replies==1&&qmi_error==X9Q_NOT_READY&&!qmi_indications&&!x9c_clients[0].registered);
 assert(!x9c_reg_wait[0][0].event_count&&x9c_cmd_init.held);
 register_cnf(token,0x30b);assert(!x9c_cmd_init.token&&!x9c_cmd_init.confirmed&&!x9c_init[1].token);
}
static void handover_request_identity(){
 bind_voice();x9w_control_ready=true;handover_begin();handover_begin();assert(!qmi_replies);
 handover_begin(0,0,92);assert(qmi_replies==1&&qmi_error==X9Q_BUSY);
 handover_begin(0,0,91);assert(qmi_replies==2&&qmi_error==X9Q_ARG);
 assert(x9c_reg_wait[0][0].answered&&!x9c_clients[0].handover);
 x9c_poll();x9mi_tick();assert(!writes&&!x9c_cmd_init.token);
}
static void handover_rx_fence(){
 rx_progress(130,7,0);rx_progress(2);rx_progress(132);
 uint8_t current[704],out[704];x9cw_header(current,16,0x909,2,0);
 uint64_t token=x9_md_mipc_submit(current,16);assert(token);x9mi_tick();
 unsigned size=current_frame(current,1);x9cw_put16(current+12,(unsigned)token);x9mi_rx(current,size);
 uint32_t n;assert(x9_md_mipc_take(token,out,sizeof(out),&n)==1);
 uint8_t p[32];struct x9cw_end_target t;assert(x9_md_mipc_call_end_snapshot(&t)==1);
 uint8_t q[]={0,8,0,0x21,0,4,0,1,1,0,7};
 assert(x9cw_action(q,11,p,32,7,7,1,1)==32);token=x9_md_mipc_call_end_submit(p,32,&t);assert(token);
 handover_frame(p,0);x9mi_rx(p,32);x9mi_tick();
 assert(x9_md_mipc_take(token,p,32,&n)==-3); // no queued pre-handover END TX
 assert(x9_mc_outbound.blocked&&!x9_mc_outbound.count_known&&x9_mc_inbound.blocked);
}
int main(int argc,char **argv){
 assert(argc==2);x9mi_online(1);
 struct {const char *name;void (*run)();} cases[]={
  {"codec_golden",codec_golden},{"codec_invalid",codec_invalid},{"incoming_routes",incoming_routes},{"action_codec",action_codec},
  {"dial_golden",dial_golden},{"dial_invalid",dial_invalid},{"dial_whitelist",dial_whitelist},
  {"dial_roundtrip",dial_roundtrip},{"dial_tx_failure",dial_tx_failure},{"dial_cnf_invalid",dial_cnf_invalid},
  {"progress_framing",progress_framing},{"outbound_states",outbound_states},{"current_arrays",current_arrays},
  {"status_fence",status_fence},{"dial_join_order",dial_join_order},{"ios_join",ios_join},
  {"ios_timeout",ios_timeout},{"ios_deleted",ios_deleted},{"dial_presend_fence",dial_presend_fence},
  {"voice_bind_codec",voice_bind_codec},{"end_target_codec",end_target_codec},{"ios_end_roundtrip",ios_end_roundtrip},
  {"current_metadata",current_metadata},
  {"current_number_owner",current_number_owner},{"progress_number_owner",progress_number_owner},
  {"end_reason_provenance",end_reason_provenance},{"outbound_end_reason",outbound_end_reason},{"incoming_end_reason",incoming_end_reason},
  {"ios_end_stale_query",ios_end_stale_query},{"ios_end_presend",ios_end_presend},
  {"ios_end_abandon",ios_end_abandon},{"ios_end_late_cnf",ios_end_late_cnf},
  {"ios_end_duplicate",ios_end_duplicate},
  {"incoming_states",incoming_states},{"incoming_state_invalid",incoming_state_invalid},{"incoming_observer",incoming_observer},
  {"register_codec",register_codec},{"register_transition",register_transition},{"register_sequence",register_sequence},
  {"register_timeout",register_timeout},{"register_deleted",register_deleted},{"register_unsupported",register_unsupported},
  {"handover_codec",handover_codec},{"handover_concurrent_bootstrap",handover_concurrent_bootstrap},
  {"handover_subscription_fence",handover_subscription_fence},{"handover_pending_timeout",handover_pending_timeout},
  {"handover_pending_overflow",handover_pending_overflow},{"handover_transport_loss",handover_transport_loss},
  {"handover_request_identity",handover_request_identity},
  {"handover_rx_fence",handover_rx_fence},
  {"request_whitelist",request_whitelist},{"no_auto_policy",no_auto_policy},{"command_roundtrip",command_roundtrip},
  {"failure_retained",failure_retained},{"duplicate_pending",duplicate_pending},{"changed_duplicate",changed_duplicate},
  {"duplicate_replied",duplicate_replied},{"overflow_isolated",overflow_isolated},{"bounded_tombstones",bounded_tombstones},
  {"fifo",fifo},{"fragmented",fragmented},{"late_cnf",late_cnf},{"sms_priority",sms_priority},{"staged_ios",staged_ios}};
 for(auto c:cases)if(!strcmp(argv[1],c.name)){c.run();assert(!lock_depth);return 0;}
 return 2;
}
