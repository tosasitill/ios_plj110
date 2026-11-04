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
  // COHERENCY_NUM_GROUPS (0x300) and COHERENCY_GROUP_0 (0x308) are the GPU's own
  // report: type 0 = none, 1 = ACE-Lite, 2 = ACE. A non-zero type is a register
  // capability, not proof that this interconnect snoops CPU caches for client
  // addresses; the boot-arg is the operator's explicit claim and both are needed.
  // Skipping CPU CIVAC is only valid once the GPU side issues snooping
  // transactions too: COHERENCY_ENABLE (0x304) written, AS MEMATTR with a
  // coherent index and outer-shareable PTEs. attributes()/install_as() program
  // none of that (SH=0, MEMATTR index0=0x88, 0x304 only read), so the request
  // is logged and refused; per-submit CIVAC stays on.
  static constexpr bool gpu_side_coherency_programmed=false;
  uint32_t groups=gr(gpu,0x300),group_type=gr(gpu,0x308)&3;
  int coherent_arg=0;
  bool want=(coherent_arg=sweet_x9_arg("x9_gpu_coherent",1))==1;
  bool capable=groups&&groups<=64&&(group_type==1||group_type==2);
  s->coherent=want&&capable&&gpu_side_coherency_programmed;
  IOLog("X9 RENDER coherency: groups=%u group0_type=%u requested=%d capable=%u gpu_side=%u active=%u;CIVAC kept unless GPU snoop is programmed\n",
   groups,group_type,coherent_arg,capable?1:0,gpu_side_coherency_programmed?1:0,s->coherent?1:0);
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
static int resident_end() {
 if(!resident||!resident->ready||resident->uncertain)return -1;
 if(!resident_cleanup(*resident)){resident->uncertain=true;return -1;}
 delete resident;resident=nullptr;return 0;
}
