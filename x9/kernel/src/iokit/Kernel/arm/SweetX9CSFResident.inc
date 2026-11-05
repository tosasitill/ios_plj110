/* SPDX-License-Identifier: MIT */
/* Included inside the frozen SweetX9CSF namespace. Uses the proven parser,
 * firmware image, PM delegation, AS0 setup and fresh PING helpers unchanged.
 * Resident ownership is separate from per-request compute/desktop copies. */
struct ResidentSession {
 Plan image;
 QueueProbe policy;
 X9CSFResidentSlot slots[8];
 X9RenderCaps caps;
 vm_offset_t gpu,smmu,doorbell;
 uint64_t saved_cfg,saved_attr,l2_owned,epoch;
 uint32_t gpu_mask,job_mask;
 unsigned slot_count;
 bool masked,installed,mcu_started,ready,uncertain;
 /* Opt-in only. Native panthor performs no cache maintenance for IO_COHERENT
  * BOs; matching that here skips the per-submit CPU CIVAC fallback. */
 bool coherent;
 ResidentSession():gpu(0),smmu(0),doorbell(0),saved_cfg(0),saved_attr(0),l2_owned(0),epoch(0),
  gpu_mask(0),job_mask(0),slot_count(0),masked(false),installed(false),mcu_started(false),ready(false),uncertain(false),
  coherent(false) {
  bzero(slots,sizeof(slots));bzero(&caps,sizeof(caps));
 }
};
static ResidentSession *resident;
static uint64_t resident_epoch_counter;
static bool resident_cleanup(ResidentSession &s) {
 if(s.uncertain||s.policy.power_uncertain||s.policy.range_uncertain||s.policy.stop_uncertain)return false;
 if(s.installed&&!no_faults(s.gpu))return false;
 /* The client layer must have terminated each group before final teardown. */
 for(unsigned i=0;i<s.slot_count;i++) {
  if(s.slots[i].borrowed)return false;
  auto &f=s.slots[i].access.interface;
  if(s.ready&&((output_word(s.image,f.group_output))&7))return false;
 }
 if(s.ready&&gr64(s.gpu,0x2440))return false; /* AS1 still owned by a renderer */
 if(s.mcu_started&&!stop_mcu(s.gpu))return false;s.mcu_started=false;
 if(s.installed&&!flush_gpu(s.gpu))return false;
 if(!shader_quiesce(s.policy,s.gpu))return false;
 if(s.installed) {
  if(!remove_as(s.gpu,s.saved_attr,s.saved_cfg))return false;
  s.image.device_exposed=false;s.installed=false;
 }
 if(s.masked){gw(s.gpu,0x28,s.gpu_mask);gw(s.gpu,0x1008,s.job_mask);s.masked=false;}
 if(!power_l2(s.gpu,s.l2_owned,false))return false;s.l2_owned=0;
 for(unsigned i=0;i<s.slot_count;i++)x9_csf_queue_access_invalidate(&s.slots[i].access);
 s.ready=false;return true;
}
static bool resident_capabilities(ResidentSession &s,uint32_t input,uint32_t output) {
 unsigned char *shared=cpu_address(s.image,0x04000000,0x10000);if(!shared)return false;
 invalidate_cpu(shared,0x10000);
 uint32_t count=get32(shared+16);if(!count||count>8)return false;
 X9CSFSharedRange ranges[max_regions];unsigned range_count=0;
 for(unsigned i=0;i<s.image.region_count;i++)if(s.image.regions[i].memory) {
  auto &r=s.image.regions[i];
  if(range_count==64)return false;ranges[range_count++]={r.memory,r.begin,r.end-r.begin};
 }
 uint32_t feature=0;
 for(unsigned i=0;i<count;i++) {
  X9CSFGroupInterface f={};
  if(!x9_csf_parse_group(shared,0x10000,i,3,&f)||f.global_input!=input||f.global_output!=output)return false;
  for(unsigned j=0;j<3;j++) {
   uint32_t flags=f.streams[j].features;
   uint32_t req=input_word(s.image,f.streams[j].input),ack=output_word(s.image,f.streams[j].output);
   if((req|ack)&7u||((req^ack)&0xc4000000u))return false;
   if((flags&0x70000u)!=0x70000u||((flags>>8)&255)!=16)return false;
   if(!feature)feature=flags;else if((flags&0xffffu)!=(feature&0xffffu))return false;
  }
  if((input_word(s.image,f.group_input)|output_word(s.image,f.group_output))&7u)return false;
  auto &slot=s.slots[i];slot.user_io=0x04400000u+i*0x200000u;slot.suspend=slot.user_io+0x10000;
  if(!x9_csf_queue_access_init(&slot.access,ranges,range_count,&f,s.gpu,s.doorbell,s.epoch))return false;
  s.slot_count=i+1;
 }
 X9RenderCaps &c=s.caps;c.abi=X9_RENDER_ABI;c.bytes=sizeof(c);c.host_page_bytes=host_page;
 /* Capability bits intentionally remain0 until the actual render engine has
  * passed its installation requirements; a running MCU alone is not ready. */
 c.gpu_id=gr(s.gpu,0);c.gpu_features=gr64(s.gpu,0x60);c.shader_present=s.policy.shader_mask;
 c.core_features=gr(s.gpu,8);c.l2_features=gr(s.gpu,4);c.tiler_features=gr(s.gpu,0xc);
 c.mem_features=gr(s.gpu,0x10);c.mmu_features=gr(s.gpu,0x14);
 c.max_threads=gr(s.gpu,0xa0);c.max_workgroup_threads=gr(s.gpu,0xa4);c.max_barrier_threads=gr(s.gpu,0xa8);
 c.thread_features=gr(s.gpu,0xac);c.tls_alloc=c.max_threads;
 for(unsigned i=0;i<4;i++)c.texture_features[i]=gr(s.gpu,0xb0+4*i);
 uint64_t freq;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(freq));c.timestamp_frequency=freq;
 c.cs_registers=(feature&255)+1;c.cs_unpreserved_registers=4;c.max_streams=3;c.max_mappings=X9_RENDER_MAX_MAPPINGS;
 c.va_start=X9_RENDER_MIN_VA;c.va_end=X9_RENDER_MAX_VA;c.max_buffer_bytes=X9_RENDER_BUFFER_LIMIT;c.max_client_bytes=X9_RENDER_CLIENT_LIMIT;
 return c.cs_registers>=64&&c.cs_registers<=256&&!(c.cs_registers&1)&&c.max_threads&&c.max_workgroup_threads&&c.thread_features&&freq;
}
/* Same firmware trace virtual addresses as trace_setup(), but one contiguous
 * owned allocation instead of16 descriptors. This keeps the frozen Plan bound
 * at32 and avoids increasing kernel stack frames on legacy compute paths. */
static bool resident_trace_setup(Plan &p) {
 if(!add_shared(p,0x04010000,8u*2u*host_page))return false;
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
   if(!host_set(p,pointers[0],host_page)||!host_set(p,pointers[1],index)||
      !host_set(p,pointers[2],index+64)||!host_set(p,pointers[3],data))return false;
   for(unsigned j=0;j<(bits+31)/32;j++)if(!host_set(p,pointers[4]+j*4,0))return false;
   count++;
  }
  at+=size;
 }
 return true;
}
static int resident_begin(vm_offset_t gpu) {
 int enabled=0;
 if(!metal_startup_verified||resident||!gpu||active_metal_job||active_desktop_job||resident_epoch_counter==UINT64_MAX||
    (enabled=sweet_x9_arg("x9_metal_render",1))!=1)return 0;
 const char *stage="initial-state";
 auto *s=new ResidentSession;if(!s)return 0;resident=s;s->gpu=gpu;s->epoch=++resident_epoch_counter;
 s->smmu=ml_io_map(0x48600000,0x1000);s->doorbell=ml_io_map(0x48080000,0x1000);
 s->saved_cfg=gr64(gpu,0x2430);s->saved_attr=gr64(gpu,0x2408);s->gpu_mask=gr(gpu,0x28);s->job_mask=gr(gpu,0x1008);
 unsigned pa_bits=(gr(gpu,0x14)>>8)&255;
 uint64_t present=gr64(gpu,0x900),initial=gr64(gpu,0x908);
 if(!s->smmu||!s->doorbell||gr(gpu,0)!=0xe8800010u||gr(gpu,0x14)!=0x2830u||
    (gr(gpu,0x18)&3)!=3||!bypass(s->smmu)||gr(gpu,0x700)||gr(gpu,0x704)||gr64(gpu,0x2400)||
    gr64(gpu,0x2440)||((s->saved_cfg&15)!=0&&(s->saved_cfg&15)!=1)||!present||present==~0ULL||initial==~0ULL||
    gr(gpu,0x1000)&0x80000001u||gr(gpu,0x20)&(1u<<17)||!no_faults(gpu))goto clean_failure;
 {
  stage="firmware-and-queue-backing";
  const char *error=parse(s->image);if(!error)error=populate(s->image);
  if(error||!resident_trace_setup(s->image)||!queue_prepare(s->image,s->policy,gpu,pa_bits)||!s->policy.requested||!s->policy.compute)goto clean_failure;
  // queue_prepare retains the one-core bring-up policy for its scalar probes.
  // The resident renderer uses the actual SHADER_PRESENT mask. Stock a16w
  // delegates the entire SHADER domain8 (not one power domain per core);
  // its scheduler intersects this mask with each group's compute/fragment
  // masks. Use the same value for global allocation, group admission and
  // public caps. Firmware delegation and global ACK remain mandatory below.
  uint64_t shaders=gr64(gpu,0xa00);
  if(!shaders||shaders==~0ULL||(s->policy.shader_mask&~shaders))goto clean_failure;
  s->policy.shader_mask=shaders;
  IOLog("X9 RENDER shader-policy: present=%llx requested=%llx cores=%u;pending firmware delegation/config ACK\n",
   shaders,s->policy.shader_mask,(unsigned)__builtin_popcountll(shaders));
  // GPU-side IO coherency, as stock a16w kbase does on MT6993 (dts mali
  // system-coherency = <0> = COHERENCY_ACE_LITE): for arch >= 12.0.1
  // kbase_cache_set_coherency_mode writes AMBA_ENABLE (0x304) bits4:0 = 0
  // (ACE-Lite) after reset, before L2 power-up (panthor_gpu_coherency_set does
  // the same with GPU_COHERENCY_PROTOCOL). On this arch 0x300 is AMBA_FEATURES
  // (b0 ACE-Lite, b1 ACE, b5 shareable cache), not COHERENCY_NUM_GROUPS, and
  // 0x308 is not a group type: the old groups/group0_type gate read the wrong
  // registers (measured AMBA_FEATURES=00000001, AMBA_ENABLE=0000001f from LK).
  // Opt-in x9_gpu_acelite=1. With it: AMBA protocol ACE-Lite, TRANSCFG PTW_SH
  // outer (install_as), render-VM leaves outer shareable (X9RenderPageTable),
  // no CPU clean of render page tables, and (unless x9_gpu_coherent=0) no
  // per-submit CIVAC of CPU-exposed client bytes. CSF-private queue/firmware
  // memory keeps its explicit maintenance (its PTEs stay non-shareable).
  {
   uint32_t amba_features=gr(gpu,0x300),amba_before=gr(gpu,0x304),amba_after=amba_before;
   uint64_t l2_ready=gr64(gpu,0x908)|gr64(gpu,0x910);
   int acelite_arg=sweet_x9_arg("x9_gpu_acelite",0),coherent_arg=sweet_x9_arg("x9_gpu_coherent",1);
   bool capable=(amba_features&1u)&&amba_features!=~0u;
   const char *why="not requested";
   if(acelite_arg==1&&!x9_gpu_acelite_active) {
    if(!capable)why="AMBA_FEATURES lacks ACE-Lite";
    else if(l2_ready)why="L2 already powered/transitioning";
    else {
     /* 1) GPU SMMU bypass must pass the GPU's shareability through (see
      * bypass() in SweetX9CSF.cpp). SMMUv3 GBPA update protocol: write with
      * UPDATE (b31), poll UPDATE clear. Only while SMMUEN=0. */
     vm_offset_t sm=s->smmu;
     uint32_t gbpa_before=gr(sm,0x44),gbpa_after=gbpa_before;
     bool smmu_ok=false;
     if(!(gr(sm,0x20)&1)&&!(gr(sm,0x24)&1)&&!(gbpa_before&(1u<<20))) {
      if(((gbpa_before>>12)&3)!=1) {
       gw(sm,0x44,(gbpa_before&~(3u<<12))|(1u<<12)|(1u<<31));
       uint64_t until=deadline_ms(5);
       while((gr(sm,0x44)&(1u<<31))&&mach_absolute_time()<until){}
      }
      gbpa_after=gr(sm,0x44);
      smmu_ok=!(gbpa_after&(1u<<31))&&((gbpa_after>>12)&3)==1&&(gbpa_after&~(3u<<12))==(gbpa_before&~(3u<<12)&~(1u<<31));
      if(smmu_ok)x9_gpu_smmu_incoming=true;
      else if(gbpa_after!=gbpa_before){gw(sm,0x44,(gbpa_before&~(1u<<31))|(1u<<31));}
     }
     IOLog("X9 RENDER coherency: GPU SMMU CR0=%08x GBPA %08x -> %08x (SHCFG %u -> %u;0=force non-shareable,1=use incoming) %s\n",
      gr(sm,0x20),gbpa_before,gbpa_after,(gbpa_before>>12)&3,(gbpa_after>>12)&3,smmu_ok?"ok":"REFUSED");
     /* Read-only view of the MFG->EMI ACP path (gpufreq_reg_mt6993.h). */
     {
      static vm_offset_t mfg_top,acp_gals,emi_bcrm,emi_pctrl;
      if(!mfg_top)mfg_top=ml_io_map(0x48500000,0x1000);
      if(!acp_gals)acp_gals=ml_io_map(0x4b420000,0x1000);
      if(!emi_bcrm)emi_bcrm=ml_io_map(0x1102b000,0x1000);
      if(!emi_pctrl)emi_pctrl=ml_io_map(0x1102c000,0x1000);
      if(mfg_top&&acp_gals&&emi_bcrm&&emi_pctrl)
       IOLog("X9 RENDER ACP: mfg_acp_slpprot=%08x (b2 MFG1 idle,b4 MFG0 idle) gals_rx=%08x gals_tx=%08x emi_acp chi0-3=%08x/%08x/%08x/%08x mfg_dvm_prot=%08x tcu_efp=%08x dvm_si=%08x pctrl con0=%08x con2=%08x\n",
        gr(mfg_top,0x834),gr(acp_gals,0x320),gr(acp_gals,0x364),gr(emi_bcrm,0x174),gr(emi_bcrm,0x180),gr(emi_bcrm,0x18c),
        gr(emi_bcrm,0x198),gr(emi_bcrm,0x160),gr(emi_bcrm,0x1a8),gr(emi_bcrm,0x1b0),gr(emi_pctrl,0),gr(emi_pctrl,8));
     }
     /* 2) GPU-side protocol, as stock kbase_cache_set_coherency_mode. */
     if(!smmu_ok)why="GPU SMMU bypass still forces non-shareable";
     else {
      gw(gpu,0x304,amba_before&~0x1fu);__asm__ volatile("dsb sy":::"memory");
      amba_after=gr(gpu,0x304);
      if((amba_after&0x1fu)==0){x9_gpu_acelite_active=true;why="enabled";}
      else {gw(gpu,0x304,amba_before);why="AMBA_ENABLE readback mismatch, restored";}
     }
    }
   }else if(x9_gpu_acelite_active)why="already enabled";
   s->coherent=x9_gpu_acelite_active&&coherent_arg==1;
   IOLog("X9 RENDER coherency: AMBA_FEATURES=%08x AMBA_ENABLE %08x -> %08x L2=%llx x9_gpu_acelite=%d x9_gpu_coherent=%d acelite=%u (%s) civac_skip=%u\n",
    amba_features,amba_before,amba_after,l2_ready,acelite_arg,coherent_arg,x9_gpu_acelite_active?1:0,why,s->coherent?1:0);
  }
  for(unsigned i=0;i<8;i++) {
   uint32_t va=0x04400000u+i*0x200000u;
   if(!add_shared(s->image,va,0x10000)||!add_shared(s->image,va+0x10000,0x100000))goto clean_failure;
  }
  if(!sync_to_device(s->image,pa_bits)||!bypass(s->smmu)||!idle_as(gpu))goto clean_failure;
 }
 stage="power-delegation-and-AS0";
 if(!shader_delegate(s->policy,gpu))goto cleanup_failure;
 s->l2_owned=present&~initial;
 if(!power_l2(gpu,s->l2_owned,true)){s->uncertain=true;goto cleanup_failure;}
 gw(gpu,0x28,s->gpu_mask&~((1u<<17)|(1u<<19)|(1u<<20)));
 gw(gpu,0x1008,s->job_mask&~0x800000ffu);s->masked=true;
 s->image.device_exposed=true;s->installed=true;
 if(!install_as(gpu,s->image.tables[0].pa))goto cleanup_failure;
 gw(gpu,0x700,2);s->mcu_started=true;
 {
  stage="fresh-MCU-IRQ";
  bool fresh=false;uint64_t end=deadline_ms(3000);
  do {
   if(!no_faults(gpu))break;
   if(gr(gpu,0x1000)&0x80000000u){fresh=true;break;}sleep_ms();
  }while(mach_absolute_time()<end);
  if(!fresh)goto cleanup_failure;gw(gpu,0x1004,0x80000000u);
  uint32_t input=0,output=0;
  stage="firmware-capabilities-and-PING";
  if(!capabilities(s->image,&input,&output)||!ping(s->image,gpu,s->doorbell,input,output,1)||
     !ping(s->image,gpu,s->doorbell,input,output,2))goto cleanup_failure;
  stage="three-stream-capabilities";
  if(!resident_capabilities(*s,input,output))goto cleanup_failure;
  stage="global-configuration";
  if(!queue_interfaces(s->image,s->policy,input,output)||!global_configure(s->image,s->policy,gpu,s->doorbell))goto cleanup_failure;
 }
 s->ready=true;
 IOLog("X9 RENDER SESSION: resident MCU with%u three-stream slots;AS0 live;GPUEB refs retained;render service not published\n",s->slot_count);
 return 2;
cleanup_failure:
 IOLog("X9 RENDER SESSION: startup failed stage=%s; checking cleanup\n",stage);
 if(!resident_cleanup(*s)){s->uncertain=true;IOLog("X9 RENDER SESSION: cleanup uncertain;all DMA/power refs retained,no reset\n");return -1;}
clean_failure:
 IOLog("X9 RENDER SESSION: startup unavailable stage=%s; no reset\n",stage);
 delete s;resident=nullptr;return 0;
}
static bool render_retire_parked(const char *why); /* SweetX9RenderEngine.inc */
static int resident_end() {
 if(!resident||!resident->ready||resident->uncertain)return -1;
 if(!render_retire_parked("resident end"))return -1;
 if(!resident_cleanup(*resident)){resident->uncertain=true;return -1;}
 delete resident;resident=nullptr;return 0;
}
