/* soc721a HIF reimplementation, based on actual PLJ110 gps_drv_dl_v062.ko
 * and OPPO gps_hif/{common,host}. start/end descs at +0x10/+0x40, payload
 * at +0x70, 4096B/channel; trans bits interleave TX/RX (2*channel + dir).
 * Target owns received/finished masks and end descs; AP NEVER clears them.
 * CCIF4 busy is W1S, TCHNUM gets channel index 4, ACK is W1C bit4.
 * This does physical mapped I/O only with provider-confirmed ownership.
 * There is deliberately no provider/automatic power-up/ready registration. */
#include "x9_gps_hif.h"
#define HIF_AP_PATTERN 0x11111111u
#define HIF_MCU_PATTERN 0x22222222u
#define HIF_DEADLINE_NS 1500000000ULL
static void hif_barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static uint32_t hif_read(volatile uint8_t *p, unsigned at)
{uint32_t v=*(volatile uint32_t *)(uintptr_t)(p+at);hif_barrier();return v;}
static void hif_write(volatile uint8_t *p, unsigned at, uint32_t v)
{*(volatile uint32_t *)(uintptr_t)(p+at)=v;hif_barrier();}
static int hif_poison(struct x9_gps_hif *h, int e)
{
 h->fault=e ? (uint32_t)e : EIO;
 /* Keep descriptors, mappings and lease intact for uncertain DMA. */
 x9_gps_fault(h->generation,(int)h->fault);return (int)h->fault;
}
static int hif_timeouts(struct x9_gps_hif *,uint64_t);
static int hif_owned(struct x9_gps_hif *h, uint64_t now)
{
 if(!h||!h->prepared||!h->ops.check_owned)return ENXIO;
 if(h->fault)return (int)h->fault;
 if(!now||now>mach_absolute_time()||now<h->last_time)return EINVAL;
 struct x9_gps_status status;x9_gps_copy_status(&status);
 if(status.generation!=h->generation||status.fault)return hif_poison(h,ESTALE);
 int e=h->ops.check_owned(h->ops.context,h->generation,0);
 if(e)return hif_poison(h,e);
 h->last_time=now;
 if(hif_read(h->ap,0)!=HIF_AP_PATTERN||hif_read(h->mcu,0)!=HIF_MCU_PATTERN)
  return hif_poison(h,EBADMSG);
 if(hif_read(h->ap,4)!=h->pending)return hif_poison(h,EPROTO);
 return h->started ? hif_timeouts(h,now) : 0;
}
static int hif_control(void *ctx,uint8_t type,const uint8_t *p,unsigned n,uint64_t now)
{
 struct x9_gps_hif_route *r=ctx;
 return r->hif->ops.control(r->hif->ops.context,r->channel,type,p,n,now);
}
static int hif_mnl(void *ctx,const uint8_t *p,unsigned n,uint64_t now)
{
 struct x9_gps_hif_route *r=ctx;
 return r->hif->ops.mnl(r->hif->ops.context,p,n,now);
}
int x9_gps_hif_prepare(struct x9_gps_hif *h,const struct x9_gps_hif_ops *ops,
    volatile void *ap,volatile void *mcu,volatile void *ccif,uint64_t generation)
{
 if(!h||!ops||!ops->check_owned||!ops->control||!ops->mnl||!ops->other_ccif||
    !ap||!mcu||!ccif||((uintptr_t)ap&0x3fff)||((uintptr_t)mcu&0x3fff)||
    ((uintptr_t)ccif&3)||!generation)return EINVAL;
 if(h->prepared)return EBUSY;
 struct x9_gps_handoff layout;int e=x9_gps_copy_handoff(&layout);if(e)return e;
 struct x9_gps_status status;x9_gps_copy_status(&status);
 if(status.generation!=generation||status.fault)return ESTALE;
 e=ops->check_owned(ops->context,generation,1);if(e)return e;
 struct x9_gps_hif_ops saved_ops=*ops;
 bzero(h,sizeof(*h));h->layout=layout;h->ops=saved_ops;
 h->ap=ap;h->mcu=mcu;h->ccif=ccif;h->generation=generation;
 /* The whole host region is AP-owned under reset. Do not touch MCU end
  * descs or target masks; real firmware initializes those after release. */
 for(unsigned i=0;i<layout.region_size;i++)h->ap[i]=0;
 hif_barrier();hif_write(h->ap,0,HIF_AP_PATTERN);
 for(unsigned i=0;i<2;i++) {
  h->route[i].hif=h;h->route[i].channel=i+1;
  if(!i)x9_gps_mcudl_stream_init_mnl(&h->stream[i],generation,hif_control,hif_mnl,&h->route[i]);
  else x9_gps_mcudl_stream_init(&h->stream[i],generation,hif_control,&h->route[i]);
 }
 h->prepared=1;return 0;
}
static unsigned hif_desc(unsigned trans)
{return 0x10+(trans/2)*16+(trans&1)*0x30;}
/* Caller already checked generation/ownership and channel. */
static int hif_submit(struct x9_gps_hif *h,unsigned trans,const uint8_t *p,unsigned n,uint64_t now)
{
 uint32_t bit=1u<<trans;
 if(h->pending&bit)return EBUSY;
 if(h->id[trans]==UINT32_MAX)return hif_poison(h,EOVERFLOW);
 uint32_t busy=hif_read(h->ccif,4);
 if(busy&~0xffu)return hif_poison(h,EIO);
 if(busy&(1u<<4))return EAGAIN; /* no writes; one serial worker, no spin */
 unsigned ch=trans/2,desc=hif_desc(trans),buf=0x70+ch*4096;
 uint32_t id=h->id[trans]+1;
 hif_write(h->ccif,4,1u<<4); /* W1S, never read-modify-write */
 if(!(trans&1))for(unsigned i=0;i<n;i++)h->ap[buf+i]=p[i];
 hif_barrier();
 hif_write(h->ap,desc,id);
 hif_write(h->ap,desc+4,h->layout.mcu_bus_base+
     ((trans&1)?h->layout.mcu2ap_offset:h->layout.ap2mcu_offset)+buf);
 hif_write(h->ap,desc+8,n);hif_write(h->ap,desc+12,0);
 h->id[trans]=id;h->length[trans]=n;h->pending|=bit;
 if(!(trans&1))h->tx_time[ch]=now;
 hif_write(h->ap,4,h->pending);hif_write(h->ccif,12,4);
 return 0; /* remote reception/completion still unknown */
}
int x9_gps_hif_send(struct x9_gps_hif *h,unsigned ch,const void *p,unsigned n,uint64_t now)
{
 if(ch>=3||!p||!n||n>4096)return EINVAL;
 int e=hif_owned(h,now);if(e)return e;
 if(h->dispatching)return EBUSY;
 if(!h->started)return ENXIO;
 /* MGMT must reserve its application ACK before publishing to MCU. */
 if(ch==0)return ENOTSUP;
 if(h->command_ack!=1||h->command)return EAGAIN;
 return hif_submit(h,ch*2,p,n,now);
}
int x9_gps_hif_management(struct x9_gps_hif *h,unsigned cmd,uint64_t now)
{
 if(cmd!=1&&cmd!=2)return ENOTSUP;
 int e=hif_owned(h,now);if(e)return e;
 if(h->dispatching)return EBUSY;
 if(!h->started)return ENXIO;
 if(h->command||h->pending&1)return EBUSY;
 if(h->commands_used&(1u<<cmd))return EALREADY;
 if(cmd==1&&(h->pending&0x2au)!=0x2au)return EAGAIN;
 if(cmd==2&&!(h->pending&2u))return EAGAIN;
 if(cmd==2&&h->command_ack!=1)return EAGAIN;
 uint32_t old_ack=h->command_ack;
 h->command=cmd;h->command_time=now;h->command_ack=0;
 uint8_t p=(uint8_t)cmd;e=hif_submit(h,0,&p,1,now);
 if(e){h->command=0;h->command_time=0;h->command_ack=old_ack;return e;}
 h->commands_used|=1u<<cmd;return 0;
}
int x9_gps_hif_start(struct x9_gps_hif *h,uint64_t now)
{
 int e=hif_owned(h,now);if(e)return e;
 if(h->started||h->dispatching)return EBUSY;
 /* Do not reuse a prior target session's sticky masks/end descriptors. */
 if(hif_read(h->mcu,4)||hif_read(h->mcu,8))return hif_poison(h,ESTALE);
 for(unsigned i=0x10;i<0x70;i+=4)
  if(hif_read(h->mcu,i))return hif_poison(h,ESTALE);
 h->started=1;h->start_time=now;
 /* Poll rearms listeners serially as CCIF4 becomes idle. They must all be
  * armed before sending cmd1, just like vendor's add-listeners-first path. */
 return 0;
}
static int hif_expired(uint64_t now,uint64_t then)
{uint64_t ns;if(!then)return 0;absolutetime_to_nanoseconds(now-then,&ns);return ns>HIF_DEADLINE_NS;}
static int hif_timeouts(struct x9_gps_hif *h,uint64_t now)
{
 if((!(h->commands_used&(1u<<1))&&hif_expired(now,h->start_time))||
     hif_expired(now,h->command_time))return hif_poison(h,ETIMEDOUT);
 for(unsigned ch=0;ch<3;ch++)if(hif_expired(now,h->tx_time[ch]))return hif_poison(h,ETIMEDOUT);
 return 0;
}
static int hif_receive(struct x9_gps_hif *h,unsigned ch,unsigned n,uint64_t now)
{
 if(ch) return x9_gps_mcudl_receive(&h->stream[ch-1],h->scratch,n,now);
 /* Live Android cmd1 reply is exactly one byte 01. Optional status, when
  * present, must be zero. Unknown management replies never acknowledge us. */
 if(!n||n>2||!h->command||h->scratch[0]!=h->command)return ENOMSG;
 if(n==2&&h->scratch[1])return EIO;
 h->command_ack=h->command;h->command=0;h->command_time=0;return 0;
}
static int hif_complete(struct x9_gps_hif *h,unsigned trans,uint64_t now)
{
 unsigned d=hif_desc(trans);uint32_t id=hif_read(h->mcu,d),n=hif_read(h->mcu,d+4);
 if(id!=h->id[trans])return 0; /* old sticky end desc, leave current request */
 if(hif_read(h->mcu,d+12)||n>h->length[trans]||(!(trans&1)&&n!=h->length[trans]))
  return hif_poison(h,EBADMSG);
 unsigned ch=trans/2;
 uint32_t expected_addr=h->layout.mcu_bus_base+
     ((trans&1)?h->layout.mcu2ap_offset:h->layout.ap2mcu_offset)+0x70+ch*4096;
 if(hif_read(h->ap,d)!=id||hif_read(h->ap,d+4)!=expected_addr||
     hif_read(h->ap,d+8)!=h->length[trans]||hif_read(h->ap,d+12))return hif_poison(h,EPROTO);
 if(trans&1) {
  for(unsigned i=0;i<n;i++)h->scratch[i]=h->mcu[0x70+ch*4096+i];
  hif_barrier();
  if(hif_read(h->mcu,d)!=id||hif_read(h->mcu,d+4)!=n||
      (hif_read(h->mcu,4)&(1u<<trans)))return hif_poison(h,EPROTO);
 }
 h->pending&=~(1u<<trans);hif_write(h->ap,4,h->pending);h->completions++;
 if(!(trans&1)){h->tx_time[ch]=0;h->tx_bytes+=n;return 0;}
 h->rx_bytes+=n;
 if(!n)return 0;
 h->dispatching=1;int e=hif_receive(h,ch,n,now);h->dispatching=0;
 /* Unknown/unsolicited MGMT is not an ACK; keep waiting for our command. */
 if(ch==0&&e==ENOMSG)return 0;
 return e ? hif_poison(h,e) : hif_owned(h,now);
}
int x9_gps_hif_poll(struct x9_gps_hif *h,uint64_t now)
{
 int e=hif_owned(h,now);if(e)return e;
 if(!h->started)return ENXIO;
 if(h->dispatching)return EBUSY;
 uint32_t rch=hif_read(h->ccif,16);
 if(rch&~0xffu)return hif_poison(h,EIO);
 /* ACK observed CCIF4 before taking completion snapshot: a new completion
  * after the ACK keeps its interrupt. No blanket ACK/clear on other bits. */
 if(rch&(1u<<4)) {
  hif_write(h->ccif,20,1u<<4);
  uint32_t received=hif_read(h->mcu,4),finished=hif_read(h->mcu,8);
  if((received|finished)&~0x3fu)return hif_poison(h,EPROTO);
  for(unsigned t=0;t<6;t++)if((h->pending&finished&~received)&(1u<<t)) {
   e=hif_complete(h,t,now);if(e)return e;
  }
 }
 for(unsigned ch=0;ch<8;ch++)if(ch!=4&&(rch&(1u<<ch))) {
  h->dispatching=1;e=h->ops.other_ccif(h->ops.context,ch,now);h->dispatching=0;
  if(e)return hif_poison(h,e);
  e=hif_owned(h,now);if(e)return e;
  /* Reset/log/fw-own events need an actual handler; never invent an ACK. */
  hif_write(h->ccif,20,1u<<ch);
 }
 /* A deinit reply does not cancel already posted DMA descriptors. Stop
  * posting new RX and retain the lease until the board proves quiesce. */
 if(h->command_ack==2||h->command==2)return 0;
 for(unsigned ch=0;ch<3;ch++)if(!(h->pending&(2u<<(2*ch)))) {
  e=hif_submit(h,2*ch+1,NULL,4096,now);
  if(e==EAGAIN)return 0;if(e)return e;
 }
 if(!(h->commands_used&(1u<<1))) {
  e=x9_gps_hif_management(h,1,now);if(e==EAGAIN)return 0;if(e)return e;
 }
 return 0;
}
int x9_gps_hif_management_result(struct x9_gps_hif *h,unsigned cmd)
{
 if(!h||!h->started)return ENXIO;
 if(h->fault)return (int)h->fault;
 if(cmd!=1&&cmd!=2)return ENOTSUP;
 uint64_t now=mach_absolute_time();int e=hif_owned(h,now);if(e)return e;
 return h->command_ack==cmd ? 0 : EAGAIN;
}
#undef HIF_AP_PATTERN
#undef HIF_MCU_PATTERN
#undef HIF_DEADLINE_NS
