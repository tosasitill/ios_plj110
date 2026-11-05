/* SPDX-License-Identifier: MIT */
#pragma once
#include <kern/cpu_data.h>
#include <pexpert/pexpert.h>
extern "C" vm_offset_t phystokv(uint64_t);
extern "C" void clean_dcache64(uint64_t,unsigned,int);

/* Set once by the resident GPU init (SweetX9CSFResident.inc) after AMBA_ENABLE
 * reads back COHERENCY_PROTOCOL=ACE-Lite, before any render VM exists; never
 * cleared afterwards. When set: AS TRANSCFG.PTW_SH = outer shareable (stock
 * kbase_mmu_hw_configure for system_coherency != NONE) and render-VM leaf PTEs
 * outer shareable (kbase SHARE_BOTH_BITS, 2<<8), so GPU page walks and client
 * accesses are snooped and the CPU-side clean/invalidate can be skipped. */
extern bool x9_gpu_acelite_active;

/* Only held, wired driver allocations: never user VAs or arbitrary MMIO.
 * The frozen Apple-coherent cache API may be a DSB-only implementation.
 * Publish the actual CPU lines to PoC before noncoherent GPU DMA. Keep each
 * bounded run and its closing DSB on the same CPU. */
static inline void x9_gpu_clean(uint64_t address,unsigned bytes,int physical)
{
 if(!bytes)return;
 /* A/B (boot-arg token x9r3): the pre-0efed90 Apple cache API. */
 static int legacy=-1;
 if(legacy<0){int v=0;legacy=PE_parse_boot_argn("x9r3",&v,sizeof(v))?1:0;}
 if(legacy){clean_dcache64(address,bytes,physical);return;}
 if(physical)address=phystokv(address);
 while(bytes) {
  unsigned run=bytes>4096?4096:bytes;
  disable_preemption();
  uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
  uint64_t line=4ULL<<((ctr>>16)&15),end=address+run;
  __asm__ volatile("dsb sy":::"memory");
  for(uint64_t at=address&~(line-1);at<end;at+=line)
   __asm__ volatile("dc cvac, %0"::"r"(at):"memory");
  __asm__ volatile("dsb sy":::"memory");
  enable_preemption();address+=run;bytes-=run;
 }
}
