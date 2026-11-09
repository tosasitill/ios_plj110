/* SPDX-License-Identifier: MIT */
#include "X9CSFRenderQueue.h"
#include <libkern/libkern.h>
/* MT6993 a16w mali_kbase_csf_registers.h/scheduler.c: GPU arch>=14.8.4
 * uses the64-bit endpoint request at+0x38. This module is X9/e8800010 only.
 * It changes firmware-owned interfaces through a real DMA accessor; it does
 * not allocate AS0/AS1, delegate power, or bypass required ownership gates. */
namespace {
constexpr uint32_t state_mask=7,cs_start=1,csg_start=1,csg_suspend=2,csg_resume=3;
constexpr uint32_t faults=0xc0000000u,oom=1u<<26;
static bool valid(const X9CSFQueueIO *io) {
 return io&&io->read32&&io->write32&&io->clean&&io->healthy&&io->doorbell&&io->now_ns&&io->yield;
}
static bool read(const X9CSFQueueIO *io,uint32_t va,uint32_t *value){return io->read32(io->cookie,va,value);}
static bool write(const X9CSFQueueIO *io,uint32_t va,uint32_t value){return io->write32(io->cookie,va,value);}
static bool write64(const X9CSFQueueIO *io,uint32_t va,uint64_t value) {
 return write(io,va,(uint32_t)value)&&write(io,va+4,(uint32_t)(value>>32));
}
static bool clean(const X9CSFQueueIO *io,uint32_t va,uint32_t bytes){return io->clean(io->cookie,va,bytes);}
static bool set_state(const X9CSFQueueIO *io,uint32_t va,unsigned state) {
 uint32_t old;return read(io,va,&old)&&write(io,va,(old&~state_mask)|state);
}
static bool notify(const X9CSFQueueIO *io,X9CSFRenderGroup *group,bool streams,uint32_t *global_req,uint32_t *group_req) {
 const auto &f=group->interface;
 if(streams) {
  uint32_t old,ack,mask=group->streams_touched;
  if(!read(io,f.group_input+8,&old)||!read(io,f.group_output+8,&ack))return false;
  *group_req=(old&~mask)|((ack^mask)&mask);
  if(!write(io,f.group_input+8,*group_req)||!clean(io,f.group_input,0x70))return false;
 }
 uint32_t old,ack,mask=1u<<f.group_index;
 if(!read(io,f.global_input+8,&old)||!read(io,f.global_output+8,&ack))return false;
 *global_req=(old&~mask)|((ack^mask)&mask);
 return write(io,f.global_input+8,*global_req)&&clean(io,f.global_input,4096)&&io->doorbell(io->cookie);
}
static void ack_snapshot(const X9CSFQueueIO *io,const X9CSFRenderGroup *g,unsigned state,bool streams,
 uint32_t global_req,uint32_t group_req) {
 const auto &f=g->interface;
 uint32_t gr=0,ga=0,cr=0,ca=0,dr=0,da=0;
 bool ok=read(io,f.global_input+8,&gr)&&read(io,f.global_output+8,&ga)&&
  read(io,f.group_input,&cr)&&read(io,f.group_output,&ca)&&
  read(io,f.group_input+8,&dr)&&read(io,f.group_output+8,&da);
 IOLog("X9 CSF ACK TIMEOUT: slot=%u want=%u streams=%u suspended=%u running=%u read=%u global=%08x/%08x expected=%08x CSG=%08x/%08x CSDB=%08x/%08x expected=%08x\n",
  f.group_index,state,streams,g->suspended,g->running,ok,gr,ga,global_req,cr,ca,dr,da,group_req);
 for(unsigned i=0;i<3;i++)if(g->streams_touched&(1u<<i)) {
  uint32_t req=0,ack=0,fault=0,fatal=0;
  bool valid=read(io,f.streams[i].input,&req)&&read(io,f.streams[i].output,&ack)&&
   read(io,f.streams[i].output+0x80,&fault)&&read(io,f.streams[i].output+0x84,&fatal);
  IOLog("X9 CSF ACK TIMEOUT: slot=%u stream=%u read=%u REQ=%08x ACK=%08x FAULT=%08x FATAL=%08x\n",
   f.group_index,i,valid,req,ack,fault,fatal);
 }
}
static IOReturn wait_ack(const X9CSFQueueIO *io,X9CSFRenderGroup *g,unsigned state,bool streams,
 uint32_t global_req,uint32_t group_req,uint64_t epoch,uint64_t end) {
 for(;;) {
  IOReturn health=x9_csf_group_health(io,g,epoch);if(health!=kIOReturnSuccess)return health;
  uint32_t global_ack,group_ack,ack;
  if(!read(io,g->interface.global_output+8,&global_ack)||!read(io,g->interface.group_output+8,&group_ack)||
     !read(io,g->interface.group_output,&ack))return kIOReturnIOError;
  bool ready=!((global_ack^global_req)&(1u<<g->interface.group_index));
  if(streams) {
   ready=ready&&!((group_ack^group_req)&g->streams_touched);
   for(unsigned i=0;i<3;i++)if(g->streams_touched&(1u<<i)) {
    uint32_t cs_ack;if(!read(io,g->interface.streams[i].output,&cs_ack))return kIOReturnIOError;
    if((cs_ack&state_mask)!=state)ready=false;
   }
  }else ready=ready&&(ack&state_mask)==state;
  if(ready)return kIOReturnSuccess;
  if(io->now_ns(io->cookie)>=end){ack_snapshot(io,g,state,streams,global_req,group_req);return kIOReturnTimeout;}
  io->yield(io->cookie);
 }
}
static bool deadline(const X9CSFQueueIO *io,uint64_t timeout,uint64_t *end) {
 if(!timeout||timeout>10000000000ULL)return false;
 uint64_t now=io->now_ns(io->cookie);if(timeout>UINT64_MAX-now)return false;*end=now+timeout;return true;
}
}
IOReturn x9_csf_group_health(const X9CSFQueueIO *io,const X9CSFRenderGroup *g,uint64_t epoch) {
 if(!valid(io)||!g||!epoch||g->firmware_epoch!=epoch)return kIOReturnBadArgument;
 if(g->uncertain||!io->healthy(io->cookie))return kIOReturnNotReady;
 uint32_t req,ack;
 if(!read(io,g->interface.global_input,&req)||!read(io,g->interface.global_output,&ack))return kIOReturnIOError;
 if((req^ack)&(1u<<27))return kIOReturnIOError; /* global fatal */
 for(unsigned i=0;i<3;i++)if(g->streams_touched&(1u<<i)) {
  if(!read(io,g->interface.streams[i].input,&req)||!read(io,g->interface.streams[i].output,&ack))return kIOReturnIOError;
  if((req^ack)&faults)return kIOReturnIOError;
  /* Do not fake an OOM acknowledgment or reset a faulting group. The owner
   * must handle the heap event or retain resources on failed quiescence. */
  if((req^ack)&oom) {
   /* rev252cg: name it. A CS tiler-heap OOM event: the stream wants more heap chunks than the fixed
    * pre-linked heap has (no heap-grow handling here; 91/92.txt: submit kIOReturnNoMemory -> device lost). */
   static unsigned logged;
   if(logged<8){logged++;IOLog("X9 CSF TILER OOM: slot=%u stream=%u REQ=%08x ACK=%08x;heap exhausted, no grow handler\n",
    g->interface.group_index,i,req,ack);}
   return kIOReturnNoMemory;
  }
 }
 return kIOReturnSuccess;
}
IOReturn x9_csf_group_configure(const X9CSFQueueIO *io,X9CSFRenderGroup *g,const X9CSFGroupInterface *f,
 const X9RenderGroup *request,const X9CSFStreamBinding bindings[3],uint64_t suspend_va,uint64_t epoch,bool reuse) {
 if(!valid(io)||!g||!f||!request||!bindings||!epoch||g->touched||g->uncertain||
    f->group_index>=16||f->stream_count!=3||request->stream_count!=3||
    request->abi!=X9_RENDER_ABI||!request->compute_mask||!request->fragment_mask||request->tiler_mask!=1||
    !request->max_compute||request->max_compute>__builtin_popcountll(request->compute_mask)||
    !request->max_fragment||request->max_fragment>__builtin_popcountll(request->fragment_mask)||
    request->max_tiler!=1||request->priority>1||!suspend_va||(suspend_va&4095)||
    suspend_va>UINT32_MAX-f->suspend_bytes)return kIOReturnBadArgument;
 if(!io->healthy(io->cookie))return kIOReturnNotReady;
 uint32_t req,ack;
 if(!read(io,f->group_input,&req)||!read(io,f->group_output,&ack))return kIOReturnBusy;
 if(reuse?(((req^ack)&state_mask)||((req&state_mask)!=0&&(req&state_mask)!=csg_suspend)):((req|ack)&state_mask)!=0)return kIOReturnBusy;
 for(unsigned i=0;i<3;i++) {
  const auto &b=bindings[i];
  if(!b.ring||(b.ring&4095)||b.ring_bytes<4096||b.ring_bytes>(1u<<20)||
     (b.ring_bytes&(b.ring_bytes-1))||b.ring>=1ULL<<48||b.ring_bytes>(1ULL<<48)-b.ring||
     !b.user_input||!b.user_output||((b.user_input|b.user_output)&4095)||
     b.user_input>UINT32_MAX-4096u||b.user_output>UINT32_MAX-4096u||
     (b.user_input<b.user_output+4096&&b.user_output<b.user_input+4096))return kIOReturnBadArgument;
  if(!read(io,f->streams[i].input,&req)||!read(io,f->streams[i].output,&ack)||((req^ack)&(faults|oom)))return kIOReturnBusy;
  /* A suspended group's streams keep REQ/ACK=START; kbase's program_cs rewrites them. */
  if(reuse?((req&state_mask)>cs_start||(ack&state_mask)>cs_start):((req|ack)&state_mask)!=0)return kIOReturnBusy;
 }
 /* Mark device exposure before the first host input write. Even failure before
  * ringing must go through owner-controlled STOP/TERMINATE and AS cleanup. */
 *g={};g->interface=*f;g->firmware_epoch=epoch;g->touched=true;g->streams_touched=7;
 memcpy(g->streams,bindings,sizeof(g->streams));
 for(unsigned i=0;i<3;i++) {
  uint32_t input=f->streams[i].input;const auto &b=bindings[i];
  if(!write64(io,input+0x10,b.ring)||!write(io,input+0x18,b.ring_bytes)||
     !write64(io,input+0x30,b.user_input)||!write64(io,input+0x38,b.user_output)||
     !write(io,input+4,1u<<8)||!write(io,input+0xc,0xffffffffu)||!clean(io,input,0x60))goto failed;
 }
 if(!write64(io,f->group_input+0x18,0)||!write64(io,f->group_input+0x20,request->compute_mask)||
    !write64(io,f->group_input+0x28,request->fragment_mask)||!write(io,f->group_input+0x30,1)||
    !write64(io,f->group_input+0x38,(uint64_t)request->max_compute|((uint64_t)request->max_fragment<<8)|
      (1ULL<<16)|((uint64_t)request->priority<<28))||
    !write64(io,f->group_input+0x40,suspend_va)||!write64(io,f->group_input+0x48,0)||
    !write(io,f->group_input+0x50,1)||!write(io,f->group_input+4,0xffffffffu)||
    !clean(io,f->group_input,0x70))goto failed;
 return kIOReturnSuccess;
failed:g->uncertain=true;return kIOReturnIOError;
}
IOReturn x9_csf_group_start(const X9CSFQueueIO *io,X9CSFRenderGroup *g,uint64_t epoch,uint64_t timeout) {
 if(!valid(io)||!g||!g->touched||g->running||g->uncertain||g->firmware_epoch!=epoch)return kIOReturnBadArgument;
 uint64_t end;if(!deadline(io,timeout,&end))return kIOReturnBadArgument;
 for(unsigned i=0;i<3;i++) {
  uint32_t input=g->interface.streams[i].input,old;
  if(!read(io,input,&old)||!write(io,input,(old&~state_mask)|cs_start|(1u<<8)|(1u<<10)|(1u<<12))||!clean(io,input,0x60))goto failed;
 }
 if(!set_state(io,g->interface.group_input,g->suspended?csg_resume:csg_start)||!clean(io,g->interface.group_input,0x70))goto failed;
 {
  uint32_t global=0,group=0;
  if(!notify(io,g,true,&global,&group))goto failed;
  IOReturn kr=wait_ack(io,g,g->suspended?csg_resume:csg_start,false,global,group,epoch,end);
  if(kr!=kIOReturnSuccess){g->uncertain=true;return kr;}
  kr=wait_ack(io,g,cs_start,true,global,group,epoch,end);
  if(kr!=kIOReturnSuccess){g->uncertain=true;return kr;}
 }
 g->running=true;g->suspended=false;return kIOReturnSuccess;
failed:g->uncertain=true;return kIOReturnIOError;
}
IOReturn x9_csf_group_halt(const X9CSFQueueIO *io,X9CSFRenderGroup *g,uint64_t epoch,bool suspend,uint64_t timeout) {
 if(!valid(io)||!g||g->firmware_epoch!=epoch||g->uncertain)return kIOReturnBadArgument;
 if(!g->touched)return kIOReturnSuccess;
 uint64_t end;if(!deadline(io,timeout,&end))return kIOReturnBadArgument;
 /* SUSPEND preserves all stream register state atomically. Issuing per-stream
  * STOP first would change saved state and break RESUME. TERMINATE matches the
  * proven CS0 STOP then CSG TERMINATE protocol, extended to all three streams. */
 if(!suspend&&!g->suspended) {
  for(unsigned i=0;i<3;i++)if(!set_state(io,g->interface.streams[i].input,0)||!clean(io,g->interface.streams[i].input,0x60))goto failed;
  uint32_t global=0,group=0;if(!notify(io,g,true,&global,&group))goto failed;
  IOReturn kr=wait_ack(io,g,0,true,global,group,epoch,end);
  if(kr!=kIOReturnSuccess){g->uncertain=true;return kr;}
 }
 if(!set_state(io,g->interface.group_input,suspend?csg_suspend:0)||!clean(io,g->interface.group_input,0x70))goto failed;
 {
  uint32_t global=0,group=0;if(!notify(io,g,false,&global,&group))goto failed;
  IOReturn kr=wait_ack(io,g,suspend?csg_suspend:0,false,global,group,epoch,end);
  if(kr!=kIOReturnSuccess){g->uncertain=true;return kr;}
 }
 g->running=false;g->suspended=suspend;if(!suspend){g->touched=false;g->streams_touched=0;}return kIOReturnSuccess;
failed:g->uncertain=true;return kIOReturnIOError;
}
