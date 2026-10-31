/* SPDX-License-Identifier: MIT */
#include "X9CSFQueueAccess.h"
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
namespace {
static void *address(X9CSFQueueAccess *a,uint32_t va,uint32_t bytes) {
 if(!a||!a->owned||!bytes)return nullptr;
 for(unsigned i=0;i<a->range_count;i++) {
  const auto &r=a->ranges[i];
  if(va>=r.gpu_va&&(uint64_t)va+bytes<=(uint64_t)r.gpu_va+r.bytes)
   return (uint8_t *)r.memory->getBytesNoCopy()+(va-r.gpu_va);
 }
 return nullptr;
}
static bool host_input(const X9CSFQueueAccess *a,uint32_t va,uint32_t bytes) {
 const auto &f=a->interface;
 if(va>=f.global_input&&(uint64_t)va+bytes<=(uint64_t)f.global_input+4096)return true;
 if(va>=f.group_input&&(uint64_t)va+bytes<=(uint64_t)f.group_input+0x70)return true;
 for(unsigned i=0;i<3;i++)if(va>=f.streams[i].input&&(uint64_t)va+bytes<=(uint64_t)f.streams[i].input+0x60)return true;
 return false;
}
static void cache(void *ptr,uint32_t bytes,bool invalidate) {
 uint64_t ctr;__asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));
 uint64_t line=4ULL<<((ctr>>16)&15);
 uintptr_t begin=(uintptr_t)ptr&~(line-1),end=(uintptr_t)ptr+bytes;
 __asm__ volatile("dsb sy":::"memory");
 for(uintptr_t at=begin;at<end;at+=line) {
  if(invalidate)__asm__ volatile("dc ivac,%0"::"r"(at):"memory");
  else __asm__ volatile("dc cvac,%0"::"r"(at):"memory");
 }
 __asm__ volatile("dsb sy":::"memory");
}
static bool read32(void *cookie,uint32_t va,uint32_t *value) {
 auto *a=(X9CSFQueueAccess *)cookie;
 if(!value||(va&3))return false;auto *ptr=(volatile uint32_t *)address(a,va,4);if(!ptr)return false;
 /* Input fields are CPU-owned: invalidating them could discard an adjacent
  * host write. Only MCU-owned output/metadata lines are invalidated. */
 if(!host_input(a,va,4))cache((void *)ptr,4,true);
 *value=*ptr;__asm__ volatile("dmb ishld":::"memory");return true;
}
static bool write32(void *cookie,uint32_t va,uint32_t value) {
 auto *a=(X9CSFQueueAccess *)cookie;
 if((va&3)||!host_input(a,va,4))return false;
 auto *ptr=(volatile uint32_t *)address(a,va,4);if(!ptr)return false;
 *ptr=value;return true;
}
static bool clean(void *cookie,uint32_t va,uint32_t bytes) {
 auto *a=(X9CSFQueueAccess *)cookie;
 if(!host_input(a,va,bytes))return false;void *ptr=address(a,va,bytes);if(!ptr)return false;
 cache(ptr,bytes,false);return true;
}
static uint32_t reg(vm_offset_t base,unsigned offset) {
 uint32_t v=*(volatile uint32_t *)(base+offset);__asm__ volatile("dmb oshld":::"memory");return v;
}
static bool healthy(void *cookie) {
 auto *a=(X9CSFQueueAccess *)cookie;if(!a||!a->owned||!a->epoch||!a->gpu)return false;
 uint32_t mmu=reg(a->gpu,0x2000),irq=reg(a->gpu,0x20),mcu=reg(a->gpu,0x704);
 /* Exact pan168 CSF fault rules: upper MMU IRQ bits are not JM bus faults. */
 return reg(a->gpu,0)==0xe8800010u&&mmu!=UINT32_MAX&&irq!=UINT32_MAX&&mcu!=UINT32_MAX&&
  !(mmu&0xffffu)&&!(irq&3u)&&mcu!=3&&reg(a->gpu,0x700)==2;
}
static bool doorbell(void *cookie) {
 auto *a=(X9CSFQueueAccess *)cookie;if(!healthy(a)||!a->kernel_doorbell)return false;
 __asm__ volatile("dsb sy":::"memory");
 *(volatile uint32_t *)a->kernel_doorbell=1;
 __asm__ volatile("dsb sy":::"memory");return true;
}
static uint64_t now(void *) {uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns;}
static void yield(void *) {
 // Sleep, do not busy-spin. Each of the four START/SUSPEND ACK stages used
 // to add an unconditional1ms on a miss, even for sub-millisecond replies.
 static int event;uint64_t deadline;clock_interval_to_deadline(100,NSEC_PER_USEC,&deadline);
 (void)assert_wait_deadline(&event,THREAD_UNINT,deadline);thread_block(THREAD_CONTINUE_NULL);
}
}
bool x9_csf_queue_access_init(X9CSFQueueAccess *out,const X9CSFSharedRange *ranges,uint32_t count,
 const X9CSFGroupInterface *interface,vm_offset_t gpu,vm_offset_t db,uint64_t epoch) {
 if(!out||!ranges||!interface||!count||count>64||!gpu||!db||!epoch||interface->stream_count!=3)return false;
 for(unsigned i=0;i<count;i++) {
  const auto &r=ranges[i];
  if(!r.memory||!r.bytes||((r.gpu_va|r.bytes)&4095)||!r.memory->getBytesNoCopy()||
     r.memory->getLength()<r.bytes||(uint64_t)r.gpu_va+r.bytes>1ULL<<32)return false;
  for(unsigned j=0;j<i;j++)if(r.gpu_va<(uint64_t)ranges[j].gpu_va+ranges[j].bytes&&
     ranges[j].gpu_va<(uint64_t)r.gpu_va+r.bytes)return false;
 }
 bzero(out,sizeof(*out));memcpy(out->ranges,ranges,count*sizeof(*ranges));out->range_count=count;
 out->interface=*interface;out->gpu=gpu;out->kernel_doorbell=db;out->epoch=epoch;out->owned=true;
 if(!healthy(out)||!address(out,interface->global_input,4096)||!address(out,interface->global_output,4096)||
    !address(out,interface->group_input,0x70)||!address(out,interface->group_output,0x20))goto bad;
 for(unsigned i=0;i<3;i++)if(!address(out,interface->streams[i].input,0x60)||!address(out,interface->streams[i].output,0xe0))goto bad;
 return true;
bad:out->owned=false;return false;
}
X9CSFQueueIO x9_csf_queue_access_io(X9CSFQueueAccess *a) {
 return {a,read32,write32,clean,healthy,doorbell,now,yield};
}
void x9_csf_queue_access_invalidate(X9CSFQueueAccess *a) {if(a){a->owned=false;a->epoch=0;}}
