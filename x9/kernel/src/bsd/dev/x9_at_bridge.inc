/* Single physical-MD owner; callers only enqueue/take under a short mutex.
 * AT has no transaction id. A missing final result quarantines ttyC0 until boot. */
#include "x9_modem_bridge.h"
#define X9_AT_SLOTS 16u
static LCK_GRP_DECLARE(x9_at_grp,"x9_at_bridge");
static LCK_MTX_DECLARE(x9_at_lock,&x9_at_grp);
struct x9_at_slot {
 uint64_t token;
 uint32_t state,timeout_ms; /* free, queued, sent, completed */
 int result,cancelled;
 char command[X9_AT_COMMAND_MAX],response[X9_AT_RESPONSE_MAX];
};
static struct x9_at_slot x9_at_slots[X9_AT_SLOTS];
static uint64_t x9_at_next_token;
static int x9_at_online,x9_at_initialized,x9_at_poisoned,x9_at_active=-1;
static unsigned x9_at_sent_ms;
static int x9_at_enabled(void){return sweet_x9_arg("x9_bb",0)&&sweet_x9_arg("x9_qmi_at",1);}
int x9_md_at_online(void){
 lck_mtx_lock(&x9_at_lock);
 int on=x9_at_online&&x9_at_initialized&&!x9_at_poisoned;
 lck_mtx_unlock(&x9_at_lock);return on;
}
uint64_t x9_md_at_submit(const char *cmd,uint32_t timeout){
 if(!cmd||timeout<100||timeout>60000)return 0;
 size_t n=strnlen(cmd,X9_AT_COMMAND_MAX);
 if(n<3||n>=X9_AT_COMMAND_MAX||cmd[n-1]!='\r'||cmd[0]!='A'||cmd[1]!='T')return 0;
 for(size_t i=0;i<n-1;i++)if((unsigned char)cmd[i]<32||(unsigned char)cmd[i]>126)return 0;
 uint64_t token=0;
 lck_mtx_lock(&x9_at_lock);
 if(x9_at_online&&x9_at_initialized&&!x9_at_poisoned)for(unsigned i=0;i<X9_AT_SLOTS;i++)if(!x9_at_slots[i].state){
  struct x9_at_slot *s=&x9_at_slots[i];bzero(s,sizeof(*s));
  token=++x9_at_next_token;if(!token)token=++x9_at_next_token;
  s->token=token;s->timeout_ms=timeout;memcpy(s->command,cmd,n+1);s->state=1;break;
 }
 lck_mtx_unlock(&x9_at_lock);return token;
}
int x9_md_at_take(uint64_t token,char *out,uint32_t room,int *result){
 if(!token||!out||!room||!result)return -1;
 int found=-1;
 lck_mtx_lock(&x9_at_lock);
 for(unsigned i=0;i<X9_AT_SLOTS;i++)if(x9_at_slots[i].state&&x9_at_slots[i].token==token){
  struct x9_at_slot *s=&x9_at_slots[i];found=0;
  if(s->state==3){
   size_t n=strnlen(s->response,sizeof(s->response));*result=s->result;
   if(n>=room){*result=X9_AT_OVERFLOW;n=0;}
   memcpy(out,s->response,n);out[n]=0;bzero(s,sizeof(*s));found=1;
  }break;
 }
 lck_mtx_unlock(&x9_at_lock);return found;
}
void x9_md_at_cancel(uint64_t token){
 lck_mtx_lock(&x9_at_lock);
 for(unsigned i=0;i<X9_AT_SLOTS;i++)if(x9_at_slots[i].state&&x9_at_slots[i].token==token){
  /* Sent requests must drain their final result even after their client dies. */
  if(x9_at_slots[i].state==2)x9_at_slots[i].cancelled=1;
  else bzero(&x9_at_slots[i],sizeof(x9_at_slots[i]));break;
 }
 lck_mtx_unlock(&x9_at_lock);
}
/* MD owner only. Exceptions never leave a stale active slot for a new client. */
static void x9at_bridge_online(int online){
 lck_mtx_lock(&x9_at_lock);x9_at_online=online&&x9_at_enabled();
 if(!online){
  x9_at_initialized=0;x9_at_active=-1;x9at_pending=0;x9at_done=0;
  for(unsigned i=0;i<X9_AT_SLOTS;i++){
   struct x9_at_slot *s=&x9_at_slots[i];
   if(s->state&&s->state!=3){
    if(s->cancelled)bzero(s,sizeof(*s));
    else{s->result=X9_AT_OFFLINE;s->state=3;s->response[0]=0;}
   }
  }
 }
 lck_mtx_unlock(&x9_at_lock);
}
static void x9at_bridge_poison(void){
 lck_mtx_lock(&x9_at_lock);x9_at_poisoned=1;lck_mtx_unlock(&x9_at_lock);
 printf("X9 QMI AT: missing final result, ttyC0 quarantined until boot\n");
}
static int x9at_bridge_tick(unsigned ms,int allow_start){
 if(!x9_at_enabled())return 0;
 char cmd[X9_AT_COMMAND_MAX];int send_slot=-1;
 lck_mtx_lock(&x9_at_lock);
 if(allow_start)x9_at_initialized=1;
 if(x9_at_active>=0){
  struct x9_at_slot *s=&x9_at_slots[x9_at_active];
  if(x9at_done||ms-x9_at_sent_ms>=s->timeout_ms){
   s->result=!x9at_done?X9_AT_TIMEOUT:x9at_overflow?X9_AT_OVERFLOW:
     x9at_done>0?X9_AT_OK:X9_AT_MODEM_ERROR;
   if(!x9at_done)x9_at_poisoned=1;
   if(s->cancelled)bzero(s,sizeof(*s));
   else{memcpy(s->response,x9at_buf,x9at_len+1);s->state=3;}
   x9at_pending=0;x9at_done=0;x9_at_active=-1;
   if(x9_at_poisoned)printf("X9 QMI AT: transaction timed out, no late-reply reuse\n");
  }
  lck_mtx_unlock(&x9_at_lock);return 1;
 }
 if(x9_at_poisoned){
  for(unsigned i=0;i<X9_AT_SLOTS;i++)if(x9_at_slots[i].state==1){
   x9_at_slots[i].result=X9_AT_TIMEOUT;x9_at_slots[i].state=3;
  }
  lck_mtx_unlock(&x9_at_lock);return 1;
 }
 if(!x9at_pending&&allow_start&&x9_at_online){
  for(unsigned i=0;i<X9_AT_SLOTS;i++)if(x9_at_slots[i].state==1&&
    (send_slot<0||x9_at_slots[i].token<x9_at_slots[send_slot].token))send_slot=(int)i;
  if(send_slot>=0){
   struct x9_at_slot *s=&x9_at_slots[send_slot];
   memcpy(cmd,s->command,sizeof(cmd));s->state=2;x9_at_active=send_slot;
   x9_at_sent_ms=ms;x9at_reset();x9at_pending=1;
  }
 }
 lck_mtx_unlock(&x9_at_lock);
 if(send_slot<0)return 0;
 /* CCIF TX can sleep waiting for ring space: never hold the mailbox lock. */
 int rc=x9md_at_send(cmd);bzero(cmd,sizeof(cmd));
 if(rc<0){
  /* A transport failure is not evidence that no bytes reached the MD. */
  x9at_bridge_poison();x9at_bridge_online(0);
 }
 return 1;
}
