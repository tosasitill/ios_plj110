/* X9 MTU3 EP0 read-only console prototype. No DMA, no storage operations. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
#include "x9_usb_log_protocol.h"
#include "x9_usb_device.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern uint32_t sweet_log_copy(uint64_t *,char *,uint32_t);
extern uint64_t sweet_log_position(void);
extern void sweet_watchdog_pet(void);
extern void thread_terminate_self(void);
#define BIT(n) (1U<<(n))
#define EP0 0x100
#define FIFO 0x300
#define EPISR 0x80
#define POWER 0x2404
#define COMMON 0x241c
#define CONFIG 0x800
#define SETUP BIT(17)
#define RX BIT(16)
#define TX BIT(18)
#define DATAEND BIT(19)
#define DPHTX BIT(20)
#define STALL BIT(25)
#define SENTSTALL BIT(22)
#define W1C (RX|SETUP|SENTSTALL)
static volatile uint32_t *mac,*ippc,*phy;
static int channel;
static uint8_t data[X9LOG_BATCH_FRAME];static unsigned sent,length,want,configured;
static enum {X9USB_IDLE,X9USB_SEND,X9USB_SEND_END} phase;
static uint32_t rr(unsigned off){return mac[off/4];}
static void wr(unsigned off,uint32_t value){mac[off/4]=value;__asm__ volatile("dsb sy":::"memory");}
static uint32_t epcsr(void){return rr(EP0)&~W1C;}
static void fifo_read(uint8_t *p,unsigned n){while(n){uint32_t v=rr(FIFO);unsigned k=n<4?n:4;for(unsigned i=0;i<k;i++)p[i]=(uint8_t)(v>>(8*i));p+=k;n-=k;}}
static void fifo_write(const uint8_t *p,unsigned n){while(n>=4){wr(FIFO,p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24);p+=4;n-=4;}if(n>=2){*(volatile uint16_t *)((uintptr_t)mac+FIFO)=p[0]|(uint16_t)p[1]<<8;p+=2;n-=2;}if(n)*(volatile uint8_t *)((uintptr_t)mac+FIFO)=*p;__asm__ volatile("dsb sy":::"memory");}
static uint64_t getpos(void *x){(void)x;return sweet_log_position();}
static uint32_t copylog(void *x,uint64_t *cursor,char *dst,uint32_t n){(void)x;return sweet_log_copy(cursor,dst,n);}
static const struct x9log_reader reader={NULL,getpos,copylog};
static unsigned strdesc(unsigned index){const char *s=index==1?"HelioDarwin":index==2?"X9 USB log":index==3?"X9USBLOG1":NULL;if(index==0){data[0]=4;data[1]=3;data[2]=9;data[3]=4;return 4;}if(!s)return 0;unsigned n=2;while(*s&&n<126){data[n++]=(uint8_t)*s++;data[n++]=0;}data[0]=(uint8_t)n;data[1]=3;return n;}
static void transmit(void){unsigned n=length-sent;if(n>64)n=64;fifo_write(data+sent,n);sent+=n;phase=(n<64||(sent==length&&length==want))?X9USB_SEND_END:X9USB_SEND;wr(EP0,epcsr()|TX);}
static void stall(unsigned ack){wr(EP0,epcsr()|STALL|ack);phase=X9USB_IDLE;}
static void setup(void)
{
 uint8_t s[8];fifo_read(s,8);unsigned type=s[0],req=s[1],value=s[2]|(unsigned)s[3]<<8,index=s[4]|(unsigned)s[5]<<8,n=s[6]|(unsigned)s[7]<<8,count=0;const uint8_t *src=NULL;
 static const uint8_t dev[]={18,1,0,2,0,0,0,64,9,0x12,1,0,0,1,1,2,3,1};
 static const uint8_t cfg[]={9,2,18,0,1,1,0,0x80,250,9,4,0,0,0,0xff,0,0,0};
 static const uint8_t qual[]={10,6,0,2,0,0,0,64,1,0};
 phase=X9USB_IDLE;sent=0;want=n;
 if(type==0x80&&req==6){if(value==0x100&&!index){src=dev;count=sizeof(dev);}else if(value==0x200&&!index){src=cfg;count=sizeof(cfg);}else if((value>>8)==3)count=strdesc(value&255);else if(value==0x600&&!index){src=qual;count=sizeof(qual);}else if(value==0x700&&!index){src=cfg;count=sizeof(cfg);}if(!count||!n){stall(n?0:SETUP);return;}if(src)memcpy(data,src,count);if(value==0x700)data[1]=7;}
 else if(type==0&&req==5&&!n&&value<128&&!index)wr(CONFIG,(rr(CONFIG)&~(0x7fU<<24))|value<<24);
 else if(type==0&&req==9&&!n&&value<=1&&!index)configured=value;
 else if(type==0x80&&req==8&&n==1&&!value&&!index){data[0]=(uint8_t)configured;count=1;}
 else if((type==0x80||type==0x81)&&req==0&&n==2&&!value&&!index){data[0]=data[1]=0;count=2;}
 else if(type==0x81&&req==10&&n==1&&!value&&!index){data[0]=0;count=1;}
 else if(type==1&&req==11&&!n&&!value&&!index){}
 else if(type==0xc0&&req==X9LOG_BATCH_REQ&&configured&&n==X9LOG_BATCH_FRAME){int k=x9log_read_capacity(&reader,(uint32_t)value|((uint32_t)index<<16),data,sizeof(data),2);if(k<0){stall(0);return;}count=(unsigned)k;}
 else if(type==0xc0&&req==X9LOG_REQ&&configured&&n==64){int k=x9log_read(&reader,(uint32_t)value|((uint32_t)index<<16),data,2);if(k<0){stall(0);return;}count=(unsigned)k;}
 else{stall(n?0:SETUP);return;}
 if(!n){wr(EP0,epcsr()|SETUP|DATAEND);return;}
 if(type&0x80){length=count<n?count:n;wr(EP0,epcsr()|SETUP|DPHTX);phase=X9USB_SEND;transmit();}else stall(0);
}
static void poll(void)
{
 uint32_t common=rr(COMMON);if(common)wr(COMMON,common);if(common&(BIT(2)|BIT(5))){phase=X9USB_IDLE;configured=0;sent=0;wr(CONFIG,rr(CONFIG)&~(0x7fU<<24));wr(EP0,64);}
 uint32_t isr=rr(EPISR);if(!(isr&(BIT(0)|BIT(16))))return;wr(EPISR,isr&(BIT(0)|BIT(16)));uint32_t csr=rr(EP0);if(isr&BIT(16)){phase=X9USB_IDLE;sent=0;}
 if(csr&SENTSTALL){wr(EP0,(csr&~(W1C|STALL))|SENTSTALL);phase=X9USB_IDLE;csr=rr(EP0);}
 if(csr&SETUP){if(rr(0x108)!=8){stall(SETUP);return;}setup();return;}
 if((phase==X9USB_SEND||phase==X9USB_SEND_END)&&!(csr&TX)){if(phase==X9USB_SEND_END){wr(EP0,epcsr()|DATAEND);phase=X9USB_IDLE;}else transmit();}
}
static void usb_delay(unsigned us){delay_for_interval(us,NSEC_PER_USEC);}
#define P(o) phy[(o)/4]
#define PU(o,mask,bits) (P(o)=(P(o)&(mask))|(bits))
/* Shared bring-up; exactly one worker owns this controller per boot. The
 * full device worker keeps SOFT_CONN clear until its DMA/config is ready. */
int x9_mtu3_init(int connect)
{
 mac=(volatile uint32_t *)(uintptr_t)ml_io_map(0x16751000,0x3000);ippc=(volatile uint32_t *)(uintptr_t)ml_io_map(0x16753000,0x1000);phy=(volatile uint32_t *)(uintptr_t)ml_io_map(0x16760000,0x1000);if(!mac||!ippc||!phy)return -1;
 volatile uint32_t *ip=(volatile uint32_t *)((uintptr_t)ippc+0xe00);ip[8/4]&=~BIT(0);ip[0x50/4]=(ip[0x50/4]&~(BIT(0)|BIT(1)|BIT(2)|BIT(9)|BIT(7)))|BIT(11)|BIT(12);__asm__ volatile("dsb sy":::"memory");usb_delay(1000);
 if((ip[0x10/4]&(BIT(0)|BIT(8)))!=(BIT(0)|BIT(8))||!(ip[0x14/4]&1))return -2;
 /* A previous stage may still own an active connection. Quiesce it before
  * touching PHY or device reset, and hold it disconnected during setup. */
 wr(POWER,rr(POWER)&~BIT(6));
 wr(0x0c,0xffffffff);wr(0x8c,0xffffffff);wr(0x2418,0);
 wr(EPISR,0xffffffff);wr(COMMON,0xffffffff);usb_delay(30000);
 PU(0x344,0xffffffff,0x20);usb_delay(250);PU(0x368,0xffffffff,0x40000);PU(0x368,0xfffffff7,0);PU(0x368,0xffffffff,8);usb_delay(30);PU(0x368,0xfffbffff,0);PU(0x368,0xfffffff7,0);PU(0x368,0xfbffffff,0);PU(0x36c,0xfffeffff,0);PU(0x320,0xfffffcff,0);PU(0x368,0xfffbffff,0);PU(0x368,0xff41c30b,0);PU(0x318,0xff7fffff,0);PU(0x318,0xffffffff,0x100000);PU(0x36c,0xffffffcf,0x24);PU(0x300,0xf8ffffff,0);PU(0x318,0x3dffffff,0);PU(0x318,0xffffffff,0x40000000);usb_delay(800);
 ip[0x98/4]|=1;usb_delay(1);ip[0x98/4]&=~1U;uint32_t info=rr(0xc10);if(!(info&31)||!((info>>8)&31))return -3;
 wr(0x141c,rr(0x141c)&~BIT(0));wr(0xc,0xffffffff);wr(0x8c,0xffffffff);wr(CONFIG,rr(CONFIG)&~(0x7fU<<24));wr(EP0,64);wr(EPISR,0x10001);wr(COMMON,0xffffffff);wr(0x88,0x10001);wr(0x2418,BIT(2)|BIT(5));wr(POWER,(rr(POWER)&~BIT(6))|(connect?BIT(6):0)|BIT(5)|BIT(0));return 0;
}
uint32_t x9_mtu3_read(unsigned off){return rr(off);}
void x9_mtu3_write(unsigned off,uint32_t v){wr(off,v);}
uint32_t x9_mtu3_ip_read(unsigned off){return ippc[(0xe00+off)/4];}
void x9_mtu3_fifo_read(uint8_t *p,unsigned n){fifo_read(p,n);}
void x9_mtu3_fifo_write(const uint8_t *p,unsigned n){fifo_write(p,n);}
static void thread_main(void *arg,wait_result_t why){(void)arg;(void)why;int e=x9_mtu3_init(1);if(e){printf("X9 USB LOG init failed %d\n",e);thread_terminate_self();return;}printf("X9 USB LOG polling active; batch4096 poll1ms\n");for(;;){poll();uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);(void)assert_wait_deadline(&channel,THREAD_UNINT,deadline);thread_block(THREAD_CONTINUE_NULL);}}
void x9_usb_log_start(void){thread_t thread;if(kernel_thread_start(thread_main,NULL,&thread)==KERN_SUCCESS)thread_deallocate(thread);else printf("X9 USB LOG thread unavailable\n");}
