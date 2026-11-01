/* SPDX-License-Identifier: MIT */
#pragma once
#include <kern/cpu_data.h>
extern "C" vm_offset_t phystokv(uint64_t);

/* Only held, wired driver allocations: never user VAs or arbitrary MMIO.
 * The frozen Apple-coherent cache API may be a DSB-only implementation.
 * Publish the actual CPU lines to PoC before noncoherent GPU DMA. Keep each
 * bounded run and its closing DSB on the same CPU. */
static inline void x9_gpu_clean(uint64_t address,unsigned bytes,int physical)
{
 if(!bytes)return;
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
