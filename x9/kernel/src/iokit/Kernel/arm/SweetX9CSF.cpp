/* MT6993 CSF host-memory preparation, from OPPO a16w firmware/MMU sources.
 * Parses stock CSF0.3, creates private image buffers and a 4-KiB AArch64
 * page tables using16-KiB XNU allocations. Optional bounded hardware phase
 * uses live SMMU bypass, installs AS0, boots MCU and verifies two PINGs.
 * Optional private AS1/CSG0/CS0 probes include real v14 compute arithmetic.
 * Never changes SMMU translation; shader dispatch requires x9_cs_compute=1. */
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include "x9_csf_firmware.inc"
#include "X9MetalABI.h"
#include "X9Desktop.h"
#include "X9CSFResident.h"
#include "X9RenderMemory.h"
extern "C" int sweet_early_is_x9(void);
extern "C" void sweet_x9_csf_prepare(void);
extern "C" int sweet_x9_csf_boot(vm_offset_t gpu);
extern "C" vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern "C" void clean_dcache64(uint64_t,unsigned,int);

namespace {
static X9MetalJob *active_metal_job;
static X9DesktopJob *active_desktop_job;
static bool metal_startup_verified;
constexpr unsigned gpu_page=4096, host_page=16384, max_regions=32, max_tables=32;
constexpr uint64_t pa_mask=0x0000fffffffff000ULL;
constexpr uint32_t flag_protected=32, flag_shared=1u<<30, supported_flags=0xc000003f;
struct Region {
 uint32_t flags,begin,end,data_begin,data_end,exe;
 IOBufferMemoryDescriptor *memory;
};
struct Table {
 IOBufferMemoryDescriptor *memory;
 uint64_t *entries,pa;
};
/* Hold original descriptor references indefinitely if DMA cleanup is uncertain. */
static IOBufferMemoryDescriptor *quarantined[64];
static unsigned quarantine_count;
struct Plan {
 Region regions[max_regions];Table tables[max_tables];
 struct Borrowed { IOMemoryDescriptor *memory; IODirection direction; } borrowed[2];
 unsigned borrowed_count=0;
 unsigned region_count,table_count,entry_count,protected_count,mapped_pages;
 uint64_t mapped_bytes;bool device_exposed;
 Plan():region_count(0),table_count(0),entry_count(0),protected_count(0),mapped_pages(0),mapped_bytes(0),device_exposed(false) {
  bzero(regions,sizeof(regions));bzero(tables,sizeof(tables));
 }
 ~Plan() {
  /* A retained descriptor plus our unmatched prepare keeps borrowed pages
   * wired on uncertainty. Never complete a descriptor still visible to DMA. */
  if(!device_exposed)for(unsigned i=0;i<borrowed_count;i++) {
   borrowed[i].memory->complete(borrowed[i].direction);borrowed[i].memory->release();
  }
  if(device_exposed) {
   for(unsigned i=0;i<region_count;i++)if(regions[i].memory&&quarantine_count<64)
    quarantined[quarantine_count++]=regions[i].memory;
   for(unsigned i=0;i<table_count;i++)if(tables[i].memory&&quarantine_count<64)
    quarantined[quarantine_count++]=tables[i].memory;
   IOLog("X9 CSF BOOT: quarantined descriptors=%u; DMA buffers remain wired\n",quarantine_count);
   return;
  }
  for(unsigned i=0;i<region_count;i++)if(regions[i].memory)regions[i].memory->release();
  for(unsigned i=0;i<table_count;i++)if(tables[i].memory)tables[i].memory->release();
 }
};
static uint32_t get32(const unsigned char *p) {
 return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static const char *parse(Plan &p) {
 const unsigned char *b=csf_firmware_bytes;size_t bytes=sizeof(csf_firmware_bytes);
 if(bytes<20||bytes>(4u<<20)||get32(b)!=0xc3f13a6e||b[4]!=3||b[5]!=0)return "header/version";
 uint32_t end=get32(b+16),at=20;
 if(end<20||end>bytes||(end&3))return "entry table bounds";
 while(at<end) {
  if(end-at<4||p.entry_count++>=128)return "entry count/header";
  uint32_t h=get32(b+at),size=(h>>8)&255,type=h&255;
  if(size<4||(size&3)||size>end-at)return "entry length";
  if(type==0) {
   if(size<28||p.region_count==max_regions)return "region length/count";
   const unsigned char *e=b+at+4;
   Region r={get32(e),get32(e+4),get32(e+8),get32(e+12),get32(e+16),0,NULL};
   if(r.flags&~supported_flags||!(r.flags&3))return "region flags";
   if(r.begin>=r.end||((r.begin|r.end)&4095)||r.end-r.begin>(4u<<20))return "region VA";
   if(r.data_begin>r.data_end||r.data_end>bytes||r.data_end-r.data_begin>r.end-r.begin)return "region file range";
   unsigned name=24;
   while(name<size&&b[at+name])name++;
   if(name==size||size-name-1<4)return "region name/execution field";
   r.exe=get32(b+at+name+1);
   for(unsigned i=0;i<p.region_count;i++)
    if(r.begin<p.regions[i].end&&p.regions[i].begin<r.end)return "overlapping virtual regions";
   if(r.flags&flag_protected)p.protected_count++;
   else p.mapped_bytes+=(uint64_t)r.end-r.begin;
   if(p.mapped_bytes>(16u<<20))return "image allocation budget";
   p.regions[p.region_count++]=r;
  }else if(type!=1&&type!=3&&type!=4&&type!=6&&type!=7&&type!=8&&type!=9&&!(h&0x80000000u)) {
   return "unknown required entry";
  }
  at+=size;
 }
 unsigned shared=0;
 for(unsigned i=0;i<p.region_count;i++)if((p.regions[i].flags&flag_shared)&&!(p.regions[i].flags&flag_protected)) {
  if(p.regions[i].begin!=0x04000000u)return "shared interface address";
  shared++;
 }
 return at==end&&p.mapped_bytes&&shared==1?NULL:"missing shared interface";
}
static bool physical(IOBufferMemoryDescriptor *m,uint64_t off,uint64_t *pa) {
 IOByteCount length=0;
 uint64_t value=m->getPhysicalSegment(off,&length,kIOMemoryMapperNone);
 if(!value||length<gpu_page||(value&4095)||(value&~pa_mask))return false;
 *pa=value;return true;
}
static int table_alloc(Plan &p) {
 if(p.table_count==max_tables)return -1;
 IOBufferMemoryDescriptor *m=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut,host_page,host_page);
 if(!m)return -1;
 uint64_t pa;
 if(!physical(m,0,&pa)||(pa&(host_page-1))){m->release();return -1;}
 void *va=m->getBytesNoCopy();
 if(!va){m->release();return -1;}
 bzero(va,host_page);
 unsigned i=p.table_count++;p.tables[i]={m,(uint64_t *)va,pa};return (int)i;
}
static int table_find(const Plan &p,uint64_t pa) {
 for(unsigned i=0;i<p.table_count;i++)if(p.tables[i].pa==pa)return (int)i;
 return -1;
}
static uint64_t attributes(uint32_t flags) {
 /* Bring-up explicitly uses software-maintained, nonshared GPU mappings.
  * Ordinary cached mode uses index0; all other modes conservatively use5.
  * DT system-coherency0 means ACE_LITE (NONE is31), not a proof of disabled
  * coherence. AMBA setup/shared mappings remain a separate enablement step. */
 unsigned cache=(flags>>3)&3,index=cache==1?0:5;
 uint64_t result=(uint64_t)index<<2;
 result|=(flags&2)?(1ULL<<6):(3ULL<<6);
 if(!(flags&4))result|=1ULL<<54;
 return result|0x400ULL|3;
}
static bool map_page(Plan &p,uint32_t address,uint64_t physical,uint32_t flags) {
 unsigned table=0;
 for(unsigned level=0;level<3;level++) {
  unsigned slot=((uint64_t)address>>(39-level*9))&511;
  uint64_t old=p.tables[table].entries[slot];int next;
  if(!old) {
   next=table_alloc(p);if(next<0)return false;
   /* Same descriptor bits as stock entry_set_pte (type3 + access bit). */
   p.tables[table].entries[slot]=p.tables[next].pa|0x400ULL|3;
  }else {
   if((old&3)!=3)return false;
   next=table_find(p,old&pa_mask);if(next<0)return false;
  }
  table=(unsigned)next;
 }
 unsigned slot=(address>>12)&511;
 if(p.tables[table].entries[slot])return false;
 p.tables[table].entries[slot]=physical|attributes(flags);p.mapped_pages++;return true;
}
static const char *populate(Plan &p) {
 if(PAGE_SIZE!=host_page)return "requires 16-KiB host pages";
 if(table_alloc(p)!=0)return "root table allocation";
 for(unsigned i=0;i<p.region_count;i++) {
  Region &r=p.regions[i];
  if(r.flags&flag_protected) {
   IOLog("X9 CSF: region=%u va=%08x..%08x protected; left unmapped\n",i,r.begin,r.end);continue;
  }
  uint32_t bytes=r.end-r.begin;
  uint32_t allocated=(bytes+host_page-1)&~(host_page-1);
  r.memory=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut,allocated,host_page);
  if(!r.memory)return "image buffer allocation";
  void *va=r.memory->getBytesNoCopy();if(!va)return "image CPU mapping";
  /* Initialize the whole owned allocation; file bytes are copied unchanged.
   * No bytes from the previous page owner can become firmware-visible. */
  bzero(va,allocated);memcpy(va,csf_firmware_bytes+r.data_begin,r.data_end-r.data_begin);
  for(uint32_t off=0;off<bytes;off+=gpu_page) {
   uint64_t pa;
   if(!physical(r.memory,off,&pa)||!map_page(p,r.begin+off,pa,r.flags))return "page-address/table allocation";
  }
  IOLog("X9 CSF: region=%u va=%08x..%08x flags=%08x data=%u host16K=%u gpu4K=%u exe=%08x prepared\n",
   i,r.begin,r.end,r.flags,r.data_end-r.data_begin,allocated/host_page,bytes/gpu_page,r.exe);
 }
 return (uint64_t)p.mapped_pages*gpu_page==p.mapped_bytes?NULL:"mapped byte count";
}
/* Hardware phase: currently observed SMMUv3 bypass is rechecked before
 * publishing any DMA address. Only e880 (MT6993 G1-Ultra14.8) is supported.
 * All offsets/commands below come from the stock v14.8 register map. */
static uint32_t gr(vm_offset_t b,unsigned off){return *(volatile uint32_t *)(b+off);}
static uint64_t gr64(vm_offset_t b,unsigned off){return *(volatile uint64_t *)(b+off);}
static void gw(vm_offset_t b,unsigned off,uint32_t value) {
 *(volatile uint32_t *)(b+off)=value;__asm__ volatile("dsb sy" ::: "memory");
}
static void gw64(vm_offset_t b,unsigned off,uint64_t value) {
 *(volatile uint64_t *)(b+off)=value;__asm__ volatile("dsb sy" ::: "memory");
}
static unsigned sleep_cookie;
static void sleep_ms(void) {
 uint64_t until;clock_interval_to_deadline(1,NSEC_PER_MSEC,&until);
 (void)assert_wait_deadline(&sleep_cookie,THREAD_UNINT,until);thread_block(THREAD_CONTINUE_NULL);
}
/* Short render cache commands frequently complete below 1 ms. Sleep in
 * 100 us quanta for the first 2 ms, then return to the original 1 ms backoff.
 * This changes observation latency only: callers retain absolute deadlines,
 * completion/error checks and all cache/AS ordering. No busy-spin loop. */
static void render_cache_wait(unsigned *polls) {
 if((*polls)++>=20){sleep_ms();return;}
 uint64_t until;clock_interval_to_deadline(100,NSEC_PER_USEC,&until);
 (void)assert_wait_deadline(&sleep_cookie,THREAD_UNINT,until);thread_block(THREAD_CONTINUE_NULL);
}
static uint64_t deadline_ms(unsigned ms) {
 uint64_t until;clock_interval_to_deadline(ms,NSEC_PER_MSEC,&until);return until;
}
static void invalidate_cpu(void *base,size_t length) {
 if(!length)return;
 uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
 size_t line=(size_t)4<<((ctr>>16)&15);
 uintptr_t start=(uintptr_t)base&~(line-1),end=(uintptr_t)base+length;
 __asm__ volatile("dsb sy" ::: "memory");
 for(uintptr_t at=start;at<end;at+=line)__asm__ volatile("dc ivac, %0" :: "r"(at) : "memory");
 __asm__ volatile("dsb sy" ::: "memory");
}
static bool bypass(vm_offset_t smmu) {
 return gr(smmu,0x20)==0&&gr(smmu,0x24)==0&&gr(smmu,0x44)==0;
}
static bool idle_as(vm_offset_t gpu,unsigned ms=1000,unsigned as=0) {
 uint64_t end=deadline_ms(ms);
 do {
  uint32_t v=gr(gpu,0x2428+as*0x40);
  if(v==0xffffffffu)return false;
  if(!(v&1))return true;
  sleep_ms();
 }while(mach_absolute_time()<end);
 return false;
}
/* CSF kbase_mmu_interrupt consumes only IRQ bits0..15 (page faults).
 * CSF bus faults are reported through GPU_IRQ/GPU_FAULTSTATUS, unlike JM's
 * legacy MMU_BUS_ERROR(n)=1<<(n+16). Do not treat upper MMU bits as bus faults. */
static bool no_faults(vm_offset_t gpu) {
 uint32_t mmu=gr(gpu,0x2000),irq=gr(gpu,0x20),mcu=gr(gpu,0x704);
 if(mmu==0xffffffffu||irq==0xffffffffu||mcu==0xffffffffu||
    (mmu&0xffffu)||(irq&3)||mcu==3) {
  IOLog("X9 CSF BOOT: fault irq=%08x mmu=%08x mcu=%08x GPUstatus=%08x GPUaddr=%llx AS0status=%08x AS0addr=%llx AS1status=%08x AS1addr=%llx\n",
   irq,mmu,mcu,gr(gpu,0x3c),gr64(gpu,0x40),gr(gpu,0x241c),gr64(gpu,0x2420),gr(gpu,0x245c),gr64(gpu,0x2460));return false;
 }
 return true;
}
static unsigned char *cpu_address(Plan &p,uint32_t va,size_t bytes) {
 for(unsigned i=0;i<p.region_count;i++) {
  Region &r=p.regions[i];
  if(r.memory&&va>=r.begin&&(uint64_t)va+bytes<=r.end)
   return (unsigned char *)r.memory->getBytesNoCopy()+(va-r.begin);
 }
 return NULL;
}
static bool host_set(Plan &p,uint32_t va,uint32_t value) {
 unsigned char *where=cpu_address(p,va,4);
 if(!where||(va&3))return false;
 memcpy(where,&value,4);return true;
}
static bool add_shared(Plan &p,uint32_t begin,uint32_t bytes=host_page,uint32_t flags=0xc0000003u) {
 if(!bytes||(bytes&(host_page-1))||bytes>0x100000u||begin>0xffffffffu-bytes)return false;
 if(p.region_count==max_regions)return false;
 for(unsigned i=0;i<p.region_count;i++)
  if(begin<p.regions[i].end&&p.regions[i].begin<begin+bytes)return false;
 Region &r=p.regions[p.region_count++];
 r={flags,begin,begin+bytes,0,0,0,NULL};
 r.memory=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut,bytes,host_page);
 if(!r.memory||!r.memory->getBytesNoCopy())return false;
 bzero(r.memory->getBytesNoCopy(),bytes);
 for(unsigned off=0;off<bytes;off+=gpu_page) {
  uint64_t pa;
  if(!physical(r.memory,off,&pa)||!map_page(p,begin+off,pa,r.flags))return false;
 }
 p.mapped_bytes+=bytes;return true;
}
static bool trace_setup(Plan &p) {
 /* Give each optional trace buffer valid data/index storage, but leave all
  * enable masks0. All pointer fixups are to our own firmware metadata. */
 const unsigned char *b=csf_firmware_bytes;unsigned at=20,end=get32(b+16),count=0;
 uint32_t cursor=0x04010000;
 while(at<end) {
  unsigned header=get32(b+at),size=(header>>8)&255;
  if((header&255)==3) {
   if(size<36||count==8)return false;
   const unsigned char *e=b+at+4;
   uint32_t pointers[5];for(unsigned j=0;j<5;j++)pointers[j]=get32(e+4+j*4);
   uint32_t bits=get32(e+24);if(bits>1024)return false;
   uint32_t data=cursor,index=cursor+host_page;cursor+=2*host_page;
   if(!add_shared(p,data)||!add_shared(p,index))return false;
   if(!host_set(p,pointers[0],host_page)||!host_set(p,pointers[1],index)||
      !host_set(p,pointers[2],index+64)||!host_set(p,pointers[3],data))return false;
   for(unsigned j=0;j<(bits+31)/32;j++)if(!host_set(p,pointers[4]+j*4,0))return false;
   count++;
  }
  at+=size;
 }
 IOLog("X9 CSF BOOT: trace buffers=%u allocated with valid pointers and disabled masks\n",count);
 return true;
}
static bool sync_to_device(Plan &p,unsigned pa_bits) {
 if(pa_bits<32||pa_bits>48)return false;
 uint64_t limit=1ULL<<pa_bits;
 for(unsigned i=0;i<p.region_count;i++) {
  Region &r=p.regions[i];if(!r.memory)continue;
  for(unsigned off=0;off<r.end-r.begin;off+=gpu_page) {
   uint64_t pa;
   if(!physical(r.memory,off,&pa)||pa>limit-gpu_page)return false;
   clean_dcache64(pa,gpu_page,1);
  }
 }
 for(unsigned i=0;i<p.table_count;i++) {
  if(p.tables[i].pa>limit-gpu_page)return false;
  clean_dcache64(p.tables[i].pa,gpu_page,1);
 }
 __asm__ volatile("dsb sy" ::: "memory");return true;
}
static bool power_l2(vm_offset_t gpu,uint64_t cores,bool enable) {
 if(!cores)return true;
 uint64_t status=gr64(gpu,0x820);
 /* HOST_POWER interface only. Never use the absent legacy v14 L2_PWRON. */
 if(!(status&1)||(status&(1ULL<<16))||(status&(1ULL<<41)))return false;
 gw64(gpu,0x830,cores);gw(gpu,0x828,enable?0x10:0x11);
 uint64_t end=deadline_ms(1000);
 do {
  uint64_t ready=gr64(gpu,0x908),transition=gr64(gpu,0x910);
  if(ready==~0ULL||transition==~0ULL)return false;
  if(!(transition&cores)&&(enable?(ready&cores)==cores:!(ready&cores)))return true;
  sleep_ms();
 }while(mach_absolute_time()<end);
 IOLog("X9 CSF BOOT: L2 power timeout enable=%u ready=%llx trans=%llx\n",
  enable,gr64(gpu,0x908),gr64(gpu,0x910));return false;
}
/* Stock kbase_mmu_get_as_setup, attributes0..6:
 * impl=88, force=8f, write-alloc=8d, outer-impl=88,
 * outer-WA=8d, noncache=4c, shared=08. */
static constexpr uint64_t memattr=0x00084c8d888d8f88ULL;
static constexpr uint64_t transcfg=0x42000006ULL;
static bool install_as(vm_offset_t gpu,uint64_t root,unsigned as=0) {
 unsigned base=as*0x40;
 if(!idle_as(gpu,1000,as))return false;
 gw64(gpu,0x2430+base,transcfg);gw64(gpu,0x2400+base,root);gw64(gpu,0x2408+base,memattr);
 gw(gpu,0x2418+base,1);
 if(!idle_as(gpu,1000,as))return false;
 uint64_t actual_root=gr64(gpu,0x2400+base),actual_attr=gr64(gpu,0x2408+base),actual_cfg=gr64(gpu,0x2430+base);
 bool ok=actual_root==root&&actual_attr==memattr&&actual_cfg==transcfg&&no_faults(gpu);
 static unsigned as1_updates;
 if(as!=1||!ok||++as1_updates<=4||as1_updates%120==0)
  IOLog("X9 CSF BOOT: AS%u UPDATE root=%llx attr=%llx cfg=%llx MMUraw=%08x ASstatus=%08x\n",
   as,actual_root,actual_attr,actual_cfg,gr(gpu,0x2000),gr(gpu,0x2428+base));
 return ok;
}
static bool remove_as(vm_offset_t gpu,uint64_t saved_attr,uint64_t saved_cfg,unsigned as=0) {
 unsigned base=as*0x40;
 if(!idle_as(gpu,1000,as))return false;
 /* Stock mmu_disable_as: AArch64 page-table attributes with MODE_UNMAPPED. */
 gw64(gpu,0x2430+base,(transcfg&~15ULL)|1);gw64(gpu,0x2400+base,0);gw(gpu,0x2418+base,1);
 if(!idle_as(gpu,1000,as)||(gr64(gpu,0x2430+base)&15)!=1||gr64(gpu,0x2400+base))return false;
 gw64(gpu,0x2408+base,saved_attr);
 /* Reset mode0 is reserved. Leave a valid unmapped mode1 when it was0. */
 if((saved_cfg&15)==1) {
  gw64(gpu,0x2430+base,saved_cfg);gw(gpu,0x2418+base,1);
 if(!idle_as(gpu,1000,as))return false;
 }
 return true;
}
static bool stop_mcu(vm_offset_t gpu) {
 gw(gpu,0x700,0);
 uint64_t end=deadline_ms(2000);
 do {if(gr(gpu,0x704)==0)return true;sleep_ms();}while(mach_absolute_time()<end);
 IOLog("X9 CSF BOOT: MCU stop not confirmed, status=%08x\n",gr(gpu,0x704));return false;
}
static bool flush_gpu(vm_offset_t gpu) {
 if(gr(gpu,0x20)&(1u<<17))return false; /* Do not accept an old completion. */
 gw(gpu,0x30,0x23304u); /* FLUSH_CACHES: L2+LSC clean/invalidate, others invalidate */
 uint64_t end=deadline_ms(1000);unsigned cache_polls=0;
 do {
  uint32_t irq=gr(gpu,0x20);
  if(irq==0xffffffffu)return false;
  if(irq&(1u<<17)){gw(gpu,0x24,1u<<17);return true;}
  render_cache_wait(&cache_polls);
 }while(mach_absolute_time()<end);
 return false;
}
static bool capabilities(Plan &p,uint32_t *input_va,uint32_t *output_va) {
 unsigned char *header=cpu_address(p,0x04000000,36);
 if(!header)return false;
 invalidate_cpu(header,36);
 uint32_t version=get32(header),groups=get32(header+16),stride=get32(header+20);
 *input_va=get32(header+8);*output_va=get32(header+12);
 IOLog("X9 CSF BOOT: global version=%08x features=%08x input=%08x output=%08x groups=%u stride=%u\n",
  version,get32(header+4),*input_va,*output_va,groups,stride);
 if(!version||version==0xffffffffu||!groups||groups>16||stride<64||
    (uint64_t)0x1000+(uint64_t)groups*stride>0x10000||
    ((*input_va|*output_va)&4095)||*input_va==*output_va||
    !cpu_address(p,*input_va,4096)||!cpu_address(p,*output_va,4096))return false;
 for(unsigned i=0;i<groups;i++) {
  unsigned char *group=cpu_address(p,0x04001000+i*stride,stride);
  if(!group)return false;
  invalidate_cpu(group,stride);
  uint32_t streams=get32(group+20),ss=get32(group+24);
  if(!streams||streams>32||ss<12||64ULL+(uint64_t)streams*ss>stride)return false;
  IOLog("X9 CSF BOOT: group=%u streams=%u stream-stride=%u suspend=%u\n",i,streams,ss,get32(group+12));
 }
 return true;
}
static bool ping(Plan &p,vm_offset_t gpu,vm_offset_t doorbell,uint32_t in,uint32_t out,unsigned round) {
 unsigned char *input=cpu_address(p,in,64),*output=cpu_address(p,out,64);
 if(!input||!output)return false;
 invalidate_cpu(input,64);invalidate_cpu(output,64);
 uint32_t ack=get32(output),req=get32(input);
 uint32_t wanted=(req&~0x100u)|((ack^0x100u)&0x100u);
 uint32_t mask=get32(input+4)|0x100u;
 memcpy(input,&wanted,4);memcpy(input+4,&mask,4);
 /* Input and output use distinct 4-KiB pages; only the host writes input. */
 for(unsigned i=0;i<p.region_count;i++) {
  Region &r=p.regions[i];
  if(r.memory&&in>=r.begin&&(uint64_t)in+64<=r.end) {
   uint64_t pa;if(!physical(r.memory,(in-r.begin)&~4095u,&pa))return false;
   clean_dcache64(pa,gpu_page,1);break;
  }
 }
 __asm__ volatile("dsb sy" ::: "memory");
 gw(doorbell,0,1);
 uint64_t end=deadline_ms(1000);
 do {
  invalidate_cpu(output,64);
  ack=get32(output);
  if((ack&0x100u)==(wanted&0x100u)) {
   IOLog("X9 CSF BOOT: ping=%u req=%08x ack=%08x completed\n",round,wanted,ack);
   if(gr(gpu,0x1000)&0x80000000u)gw(gpu,0x1004,0x80000000u);
   return true;
  }
  if(!no_faults(gpu))return false;
  sleep_ms();
 }while(mach_absolute_time()<end);
 IOLog("X9 CSF BOOT: ping=%u timeout req=%08x ack=%08x\n",round,wanted,ack);return false;
}
#include "SweetX9CSFQueue.inc"
/* Return1 after boot+two real firmware PINGs;0 for a clean skip/failure;
 * -1 requires retaining outer power references (cleanup not confirmed). */
static int boot(Plan &p,vm_offset_t gpu,vm_offset_t smmu,vm_offset_t doorbell) {
 uint32_t id=gr(gpu,0),mmu=gr(gpu,0x14),as_present=gr(gpu,0x18);
 uint64_t saved_cfg=gr64(gpu,0x2430),saved_attr=gr64(gpu,0x2408);
 uint32_t gpu_mask=gr(gpu,0x28),job_mask=gr(gpu,0x1008);
 uint64_t l2_present=gr64(gpu,0x900),l2_initial=gr64(gpu,0x908),l2_owned=0;
 bool installed=false,mcu_started=false,masked=false,success=false;
 QueueProbe queue;
 unsigned pa_bits=(mmu>>8)&255,va_bits=mmu&255;
 IOLog("X9 CSF BOOT: entry ID=%08x VA=%u PA=%u AS=%x L2=%llx ready=%llx cfg=%llx PWR_STATUS=%llx\n",
  id,va_bits,pa_bits,as_present,l2_present,l2_initial,saved_cfg,gr64(gpu,0x820));
 IOLog("X9 CSF BOOT: AMBA_FEATURES=%08x AMBA_ENABLE=%08x (read only); DT0=ACE_LITE capability\n",
  gr(gpu,0x300),gr(gpu,0x304));
 if((id>>16)!=0xe880||va_bits!=48||pa_bits<32||pa_bits>48||!(as_present&1)||
    !bypass(smmu)||gr(gpu,0x700)!=0||gr(gpu,0x704)!=0||
    gr64(gpu,0x2400)||((saved_cfg&15)!=0&&(saved_cfg&15)!=1)||
    !l2_present||l2_present==~0ULL||l2_initial==~0ULL||
    gr(gpu,0x1000)&0x80000001u||gr(gpu,0x20)&(1u<<17)||!no_faults(gpu)) {
  IOLog("X9 CSF BOOT: ownership/bypass/idle gate rejected; no hardware writes\n");return 0;
 }
 if(!trace_setup(p)||!queue_prepare(p,queue,gpu,pa_bits)||!sync_to_device(p,pa_bits)) {
  IOLog("X9 CSF BOOT: trace/queue setup or DMA range/cache synchronization rejected\n");return 0;
 }
 IOLog("X9 CSF BOOT: SMMU bypass selected for direct DMA; owned buffers cleaned to PoC\n");
 if(!bypass(smmu)||gr(gpu,0x704)!=0||!idle_as(gpu))return 0;
 if(!shader_delegate(queue,gpu)) {
  IOLog("X9 SHADER: delegation gate/command failed before L2/AS publication\n");
  return queue.power_uncertain?-1:0;
 }
 l2_owned=l2_present&~l2_initial;
 if(!power_l2(gpu,l2_owned,true)) {
  IOLog("X9 CSF BOOT: host L2 power command not confirmed; retaining power\n");return -1;
 }
 if(!bypass(smmu)||gr(gpu,0x704)!=0) {
  if(queue.delegated_domains) {
   IOLog("X9 SHADER: runtime ownership changed after delegation; outer power retained\n");return -1;
  }
  IOLog("X9 CSF BOOT: bypass/MCU state changed after L2 request; AS0 not installed\n");
  return power_l2(gpu,l2_owned,false)?0:-1;
 }
 gw(gpu,0x28,gpu_mask&~((1u<<17)|(1u<<19)|(1u<<20)));
 gw(gpu,0x1008,job_mask&~0x80000001u);masked=true;
 p.device_exposed=true;installed=true;
 if(!install_as(gpu,p.tables[0].pa)) {
  IOLog("X9 CSF BOOT: AS0 install failed\n");goto cleanup;
 }
 IOLog("X9 CSF BOOT: AS0 root=%llx MEMATTR=%llx TRANSCFG=%llx installed\n",p.tables[0].pa,memattr,transcfg);
 /* AUTO follows original boot_csf_firmware. Compute remains separately gated. */
 gw(gpu,0x700,2);mcu_started=true;
 {
  uint64_t end=deadline_ms(3000);bool irq=false;
  do {
   if(!no_faults(gpu))break;
   if(gr(gpu,0x1000)&0x80000000u){irq=true;break;}
   sleep_ms();
  }while(mach_absolute_time()<end);
  if(!irq) {
   IOLog("X9 CSF BOOT: MCU boot timeout/fault status=%08x jobirq=%08x\n",gr(gpu,0x704),gr(gpu,0x1000));goto cleanup;
  }
  gw(gpu,0x1004,0x80000000u);
  uint32_t input,output;
  if(!capabilities(p,&input,&output)) {
   IOLog("X9 CSF BOOT: firmware global interface invalid\n");goto cleanup;
  }
  if(!ping(p,gpu,doorbell,input,output,1)||!ping(p,gpu,doorbell,input,output,2))goto cleanup;
  success=true;
  IOLog("X9 CSF BOOT: firmware boot + global capabilities + two PING acknowledgements verified\n");
  if(queue.requested) {
   int result=queue_run(p,queue,gpu,doorbell,input,output);
   IOLog("X9 CSF QUEUE: result=%d (1 all requested command-stream writes verified;0 skip/failure)\n",result);
  }
 }
cleanup:
 if(queue.range_uncertain||queue.stop_uncertain||queue.power_uncertain) {
  IOLog("X9 CSF QUEUE: range/stop outcome uncertain; no retry, further GPU commands or resource release\n");goto retain;
 }
 if(!queue_stop(p,queue,gpu,doorbell)) {
  IOLog("X9 CSF QUEUE: STOP/TERMINATE not confirmed; MCU, AS0/AS1 and power references retained\n");goto retain;
 }
 if(mcu_started&&!stop_mcu(gpu))goto retain;
 if(installed) {
  if(!flush_gpu(gpu)) {
   IOLog("X9 CSF BOOT: GPU cache flush not confirmed\n");goto retain;
  }
  queue_post_flush(p,queue);
  shader_post_flush(queue);
  if(!shader_quiesce(queue,gpu)) {
   IOLog("X9 SHADER: shader power cleanup not confirmed; DMA buffers and outer power retained\n");goto retain;
  }
  if(queue.as_installed) {
   if(!remove_as(gpu,queue.saved_attr,queue.saved_cfg,1)) {
    IOLog("X9 CSF QUEUE: AS1 detach not confirmed\n");goto retain;
   }
   queue.memory.device_exposed=false;
   for(unsigned i=0;i<queue.memory.region_count;i++)if(queue.memory.regions[i].memory)
    invalidate_cpu(queue.memory.regions[i].memory->getBytesNoCopy(),
                   queue.memory.regions[i].end-queue.memory.regions[i].begin);
   IOLog("X9 CSF QUEUE: private AS1 detached; task buffers no longer device-owned\n");
  }
  if(!remove_as(gpu,saved_attr,saved_cfg)) {
   IOLog("X9 CSF BOOT: cache flush/AS0 detach not confirmed\n");goto retain;
  }
  p.device_exposed=false;
  if(gr(gpu,0x1000)&0x80000000u)gw(gpu,0x1004,0x80000000u);
  if(gr(gpu,0x20)&(1u<<19))gw(gpu,0x24,1u<<19);
  uint32_t ours=gr(gpu,0x2000)&(queue.as_installed?3u:1u);
  if(ours)gw(gpu,0x2004,ours);
 }
 if(masked){gw(gpu,0x28,gpu_mask);gw(gpu,0x1008,job_mask);}
 for(unsigned i=0;i<p.region_count;i++)if(p.regions[i].memory)
  invalidate_cpu(p.regions[i].memory->getBytesNoCopy(),p.regions[i].end-p.regions[i].begin);
 if(!power_l2(gpu,l2_owned,false)) {
  IOLog("X9 CSF BOOT: L2 release incomplete; AS0 detached but outer power retained\n");return -1;
 }
 IOLog("X9 CSF BOOT: stopped, GPU caches flushed, AS0 detached, owned L2 released; result=%u\n",success);
 return success?1:0;
retain:
 IOLog("X9 CSF BOOT: cleanup uncertain; DMA buffers and outer power references retained, no reset\n");
 return -1;
}

#include "SweetX9CSFResident.inc"
#include "SweetX9RenderEngine.inc"

}
extern "C" void sweet_x9_csf_prepare(void) {
 int enabled=0;
 if(!sweet_early_is_x9()||!PE_parse_boot_argn("x9_csf_prepare",&enabled,sizeof(enabled))||enabled!=1)return;
 int boot_enabled=0;
 if(PE_parse_boot_argn("x9_csf_boot",&boot_enabled,sizeof(boot_enabled))&&boot_enabled==1) {
  IOLog("X9 CSF: image preparation deferred into powered MCU startup path\n");return;
 }
 Plan p;
 IOLog("X9 CSF: stock bytes=%lu sha256=%s host-only preparation, GPU execution disabled\n",
  (unsigned long)sizeof(csf_firmware_bytes),csf_firmware_sha256);
 const char *error=parse(p);
 if(!error)error=populate(p);
 if(error) {
  IOLog("X9 CSF: preparation stopped: %s; allocations released without device ownership\n",error);return;
 }
 IOLog("X9 CSF: image prepared entries=%u regions=%u protected-skipped=%u bytes=%llu GPU-pages=%u tables=%u root-CPU-PA=%llx\n",
  p.entry_count,p.region_count,p.protected_count,p.mapped_bytes,p.mapped_pages,p.table_count,p.tables[0].pa);
 IOLog("X9 CSF: candidate tables not installed; CPU PA is not confirmed GPU DMA address; MCU/AS/SMMU untouched\n");
 /* Destructor releases everything; future submission must retain buffers until
  * both GPU-MMU and outer SMMU references have been quiesced. */
}

extern "C" int sweet_x9_csf_boot(vm_offset_t gpu) {
 int enabled=0;
 if(!sweet_early_is_x9()||!PE_parse_boot_argn("x9_csf_boot",&enabled,sizeof(enabled))||enabled!=1)return 0;
 /* Caller has validated live FDT contract, power counts, MFG1/MFG37 ACK
  * and a stable ID; it retains its active/power references until we return. */
 vm_offset_t smmu=ml_io_map(0x48600000,0x1000),doorbell=ml_io_map(0x48080000,0x1000);
 if(!smmu||!doorbell)return 0;
 Plan p;const char *error=parse(p);
 if(!error)error=populate(p);
 if(error){IOLog("X9 CSF BOOT: image preparation failed: %s\n",error);return 0;}
 IOLog("X9 CSF BOOT: pan168 stock-firmware=%s prepared bytes=%llu tables=%u\n",
  csf_firmware_sha256,p.mapped_bytes,p.table_count);
 return boot(p,gpu,smmu,doorbell);
}

extern "C" int sweet_x9_csf_user(vm_offset_t gpu,void *job) {
 if(!metal_startup_verified||active_metal_job||active_desktop_job||!job)return 0;
 active_metal_job=(X9MetalJob *)job;
 int result=sweet_x9_csf_boot(gpu);
 bool passed=active_metal_job->computed&&active_metal_job->cleaned;
 active_metal_job=NULL;
 return result<0?-1:(result==1&&passed?1:0);
}
extern "C" int sweet_x9_csf_desktop(vm_offset_t gpu,void *job) {
 if(!metal_startup_verified||active_metal_job||active_desktop_job||!job)return 0;
 active_desktop_job=(X9DesktopJob *)job;
 int result=sweet_x9_csf_boot(gpu);
 bool passed=active_desktop_job->computed&&active_desktop_job->cleaned;
 active_desktop_job=NULL;
 return result<0?-1:(result==1&&passed?1:0);
}
#include "SweetX9ComputeService.inc"

extern "C" int sweet_x9_csf_resident_begin(vm_offset_t gpu) {return resident_begin(gpu);}
extern "C" int sweet_x9_csf_resident_end(void) {return resident_end();}
extern "C" bool sweet_x9_csf_resident_caps(X9RenderCaps *caps) {
 if(!caps||!resident||!resident->ready||resident->uncertain)return false;
 *caps=resident->caps;return true;
}
extern "C" X9CSFResidentSlot *sweet_x9_csf_resident_slot(unsigned index) {
 if(!resident||!resident->ready||resident->uncertain||index>=resident->slot_count||resident->slots[index].borrowed)return nullptr;
 resident->slots[index].borrowed=true;return &resident->slots[index];
}

extern "C" bool sweet_x9_csf_resident_release_slot(unsigned index,uint64_t epoch) {
 if(!resident||!resident->ready||resident->uncertain||index>=resident->slot_count||resident->epoch!=epoch||
    !resident->slots[index].borrowed||gr64(resident->gpu,0x2440))return false;
 const auto &f=resident->slots[index].access.interface;
 if((input_word(resident->image,f.group_input)|output_word(resident->image,f.group_output))&7u)return false;
 for(unsigned i=0;i<3;i++)if((input_word(resident->image,f.streams[i].input)|output_word(resident->image,f.streams[i].output))&7u)return false;
 if(!no_faults(resident->gpu)||!flush_gpu(resident->gpu)){resident->uncertain=true;return false;}
 unsigned char *io=cpu_address(resident->image,resident->slots[index].user_io,0x10000);
 if(!io){resident->uncertain=true;return false;}
 bzero(io,0x10000);
 for(unsigned off=0;off<0x10000;off+=gpu_page)
  if(!clean_page(resident->image,resident->slots[index].user_io+off)){resident->uncertain=true;return false;}
 resident->slots[index].borrowed=false;return true;
}

extern "C" bool sweet_x9_render_owner_enter(void) {
 if(!metal_service_lock)return false;IOLockLock(metal_service_lock);return true;
}
extern "C" void sweet_x9_render_owner_leave(void) {IOLockUnlock(metal_service_lock);}
// Nonblocking display-side ownership. Never wait for GPU work on the timer
// workloop. Positive means the caller must leave(); zero is no native engine;
// negative means keep the pending frame, including on uncertain DMA ownership.
extern "C" int sweet_x9_render_display_try_enter(void) {
 if(!metal_service_lock)return 0;
 if(!IOLockTryLock(metal_service_lock))return -1;
 if((resident&&resident->uncertain)||
    (render_engine_installed&&(!render_ready()||gr64(resident->gpu,0x2440)))) {
  IOLockUnlock(metal_service_lock);return -2;
 }
 return 1;
}
extern "C" void sweet_x9_render_bootstrap(void) {
 int opt=0;
 if(!PE_parse_boot_argn("x9_metal_render",&opt,sizeof(opt))||opt!=1||render_engine_installed)return;
 /* The render flag replaces the legacy compute flag. Bootstrap cannot
  * depend on compute publication to allocate the shared owner lock. This is
  * the sole startup worker; no service is published before allocation. */
 if(!metal_service_lock)metal_service_lock=IOLockAlloc();
 if(!sweet_x9_render_owner_enter())return;
 int result=sweet_x9_gpu_render_begin();
 if(result==2)render_engine_installed=true;
 sweet_x9_render_owner_leave();
 if(result==2) {
  sweet_x9_render_install(&resident_engine);sweet_x9_render_publish();
  X9RenderCaps caps={};
  if(render_caps(&caps))IOLog("X9 RENDER CAPS: abi=%u capabilities=0x%x streams=%u cache-atom=%u;no legacy compute publication\n",
   caps.abi,caps.capabilities,caps.max_streams,X9_RENDER_CACHE_ATOM);
  IOLog("X9 RENDER: resident multi-stream engine callbacks installed;experimental opt-in,no hardware rendering claim\n");
 }else IOLog("X9 RENDER: resident engine unavailable result=%d;no render service published\n",result);
}
