/* Private CSF queue bring-up, included inside SweetX9CSF's namespace.
 * ABI: OPPO MT6993 a16w csf firmware/scheduler/registers and qinspect decoder.
 * Default probes use MOVE/MOVE32/CALL/SYNC_SET32; opt-in compute also
 * dispatches a v14 integer shader. No tiler, fragment or neural work.
 * AS1 maps only our command ring, task bodies and result; MCU AS0 owns user IO/suspend.
 */
#include "x9_compute_program.inc"
constexpr uint32_t queue_user=0x04060000,queue_suspend=0x04200000;
constexpr uint32_t queue_suspend_bytes=0x100000;
constexpr uint32_t queue_ring=0x10000000,queue_result=0x10004000;
constexpr uint32_t queue_bodies=0x10008000;
constexpr uint32_t queue_values[]={0x58394353u,0x4d543639u,0x43534631u};
/*32-byte immutable dispatch packets divide the existing16KiB ring exactly.
 * Four task pages also divide the512 slots, so the same dispatch bytes remain
 * correct across ring wrap. NOP opcode0 is documented by stock csf_sync.c. */
constexpr unsigned queue_dispatch_bytes=32,queue_dispatch_slots=host_page/queue_dispatch_bytes;
static uint32_t queue_expected(unsigned round) {
 return round<3?queue_values[round]:(0x58490000u|(round+1));
}
static bool queue_verbose(unsigned round) {
 return round<4||round%64==0||(round>=30&&round<=33)||
        (round>=510&&round<=513)||round>=574;
}
constexpr unsigned queue_batch_bytes=3*sizeof(uint64_t);
struct QueueProbe {
 Plan memory;
 bool user,requested,as_installed,touched,dynamic,indirect,reuse,range_uncertain,stop_uncertain,compute,power_uncertain,shader_dumped;
 uint32_t shader_pwroff_count,power_domains,delegated_domains;
 uint64_t shader_mask;
 unsigned completed;
 uint64_t saved_cfg,saved_attr;
 uint32_t global_in,global_out,group_in,group_out,stream_in,stream_out;
 QueueProbe():user(false),requested(false),as_installed(false),touched(false),dynamic(false),indirect(false),reuse(false),range_uncertain(false),stop_uncertain(false),compute(false),power_uncertain(false),shader_dumped(false),shader_pwroff_count(0),power_domains(0),delegated_domains(0),shader_mask(0),completed(0),saved_cfg(0),saved_attr(0),
  global_in(0),global_out(0),group_in(0),group_out(0),stream_in(0),stream_out(0) {}
};
static bool clean_page(Plan &p,uint32_t va) {
 for(unsigned i=0;i<p.region_count;i++) {
  Region &r=p.regions[i];
  if(r.memory&&va>=r.begin&&va<r.end) {
   uint64_t pa;
   if(!physical(r.memory,(va-r.begin)&~4095u,&pa))return false;
   x9_gpu_clean(pa,gpu_page,1);__asm__ volatile("dsb sy" ::: "memory");return true;
  }
 }
 return false;
}
static uint32_t input_word(Plan &p,uint32_t va,unsigned off=0) {
 unsigned char *v=cpu_address(p,va+off,4);
 return v?get32(v):0xffffffffu;
}
static uint32_t output_word(Plan &p,uint32_t va,unsigned off=0) {
 unsigned char *v=cpu_address(p,va+off,4);
 if(!v)return 0xffffffffu;
 invalidate_cpu(v,4);return get32(v);
}
static uint64_t output_qword(Plan &p,uint32_t va) {
 unsigned char *v=cpu_address(p,va,8);
 if(!v)return ~0ULL;
 invalidate_cpu(v,8);uint64_t value;memcpy(&value,v,8);return value;
}
static bool put_qword(Plan &p,uint32_t va,uint64_t value) {
 unsigned char *v=cpu_address(p,va,8);
 if(!v||(va&7))return false;
 memcpy(v,&value,8);return true;
}
static bool interface_address(Plan &p,uint32_t va,unsigned bytes) {
 /* Firmware host-input/output tables live only in the owned shared image. */
 return va>=0x04000000&&!(va&63)&&(uint64_t)va+bytes<=0x04010000&&cpu_address(p,va,bytes);
}
static bool desktop_prepare(QueueProbe &);
static bool desktop_submit(Plan &,QueueProbe &,vm_offset_t,vm_offset_t);
static bool desktop_validate(QueueProbe &);
#include "SweetX9CSFCompute.inc"
static bool queue_prepare(Plan &firmware,QueueProbe &q,vm_offset_t gpu,unsigned pa_bits) {
 int enabled=0;
 q.requested=sweet_x9_arg("x9_csf_queue_probe",1)==1;
 q.user=active_metal_job!=NULL||active_desktop_job!=NULL;
 if(!q.requested)return true;
 enabled=0;
 q.dynamic=PE_parse_boot_argn("x9_csf_dynamic_ring",&enabled,sizeof(enabled))&&enabled==1;
 enabled=0;
 q.indirect=sweet_x9_arg("x9_csf_indirect",1)==1;
 enabled=0;
 q.reuse=PE_parse_boot_argn("x9_csf_reuse",&enabled,sizeof(enabled))&&enabled==1;
 enabled=0;
 q.compute=sweet_x9_arg("x9_cs_compute",1)==1;
 if(q.compute&&(!q.indirect||q.reuse||q.dynamic)) {
  IOLog("X9 SHADER: compute requires immutable CALL dispatch packets\n");return false;
 }
 if(q.reuse&&!q.indirect) {
  IOLog("X9 CSF QUEUE: body reuse requires immutable CALL dispatch\n");return false;
 }
 if(q.indirect&&q.dynamic) {
  IOLog("X9 CSF QUEUE: select either dynamic direct-ring or indirect mode\n");return false;
 }
 q.saved_cfg=gr64(gpu,0x2470);q.saved_attr=gr64(gpu,0x2448);
 if(!(gr(gpu,0x18)&2)||gr64(gpu,0x2440)||
    ((q.saved_cfg&15)!=0&&(q.saved_cfg&15)!=1)||!idle_as(gpu,1000,1)) {
  IOLog("X9 CSF QUEUE: AS1 ownership gate rejected, no queue published\n");return false;
 }
 if(table_alloc(q.memory)!=0||!add_shared(q.memory,queue_ring)||!add_shared(q.memory,queue_result)||(q.indirect&&!add_shared(q.memory,queue_bodies))||
    !add_shared(firmware,queue_user)||!add_shared(firmware,queue_suspend,queue_suspend_bytes)) {
  IOLog("X9 CSF QUEUE: private ring/result/user-IO/suspend allocation failed\n");return false;
 }
 if(!shader_prepare(q,gpu)||!desktop_prepare(q))return false;
 /* Keep the root dispatch immutable after publication. In campaign mode,
  * fill all512 slots so wrapping never re-exposes an uninitialized root slot.
  * Only task bodies and the host-owned producer page change while running. */
 for(unsigned round=0;!q.dynamic&&round<(q.reuse?queue_dispatch_slots:(q.compute?(q.user?1u:x9_compute_campaign_count):3u));round++) {
  uint32_t at=queue_ring+round*(q.reuse?queue_dispatch_bytes:queue_batch_bytes);
  if(q.indirect) {
   /* Immutable dispatch triplet: MOVE r4:r5,body; MOVE32 r6,24;
    * CALL opcode20,src0=r4:r5 bits40,src1=r6 bits32. Task bytes
    * remain zero here and are generated only after queue START. */
   if(!put_qword(q.memory,at,(1ULL<<56)|(4ULL<<48)|(queue_bodies+(round%((q.reuse||q.compute)?4u:3u))*gpu_page))||
      !put_qword(q.memory,at+8,(2ULL<<56)|(6ULL<<48)|(q.compute?x9_compute_cs_bytes:queue_batch_bytes))||
      !put_qword(q.memory,at+16,(0x20ULL<<56)|(4ULL<<40)|(6ULL<<32)))return false;
   if(q.reuse&&!put_qword(q.memory,at+24,0))return false;
   continue;
  }
  if(!put_qword(q.memory,at,(1ULL<<56)|queue_result)||
     !put_qword(q.memory,at+8,(2ULL<<56)|(2ULL<<48)|queue_values[round])||
     !put_qword(q.memory,at+16,(0x26ULL<<56)|(2ULL<<32)))return false;
 }
 if(q.user)IOLog("X9 METAL GPU: private one-dispatch copied user request\n");
 else if(q.compute)IOLog("X9 SHADER:75 immutable CALL packets;3 scalar+6 geometry+64 reuse+2 GPU-dependent vector dispatches\n");
 else if(q.reuse)IOLog("X9 CSF QUEUE: 512 immutable CALL+NOP packets; four private4KiB task pages; reuse/wrap/restart campaign608\n");
 else if(q.indirect)IOLog("X9 CSF QUEUE: immutable CALL dispatch ring; three private4KiB bodies generated after START\n");
 else if(q.dynamic)IOLog("X9 CSF QUEUE: dynamic ring; each append requires CPU clean + GPU PA-range completion before INSERT\n");
 else IOLog("X9 CSF QUEUE: nine instructions/72 bytes prepublished; runtime advances INSERT only\n");
 /* Only the GPU is allowed to replace this initial value after publication. */
 memset(cpu_address(q.memory,queue_result,host_page),0xa5,host_page);
 /* Stock BASEP_EVENT32_ERR_OFFSET=4, VAL_OFFSET=0. An unpoisoned sync object
  * has an initial error word0; the adjacent cache-line guard remains a5. */
 if(!host_set(q.memory,queue_result+4,0))return false;
 if(!sync_to_device(q.memory,pa_bits))return false;
 IOLog("X9 CSF QUEUE: AS1-private ring=%08x result=%08x; MCU userIO=%08x suspend=%08x+%x prepared\n",
  queue_ring,queue_result,queue_user,queue_suspend,queue_suspend_bytes);
 return true;
}
static bool global_healthy(Plan &p,QueueProbe &q,vm_offset_t gpu) {
 if(!no_faults(gpu))return false;
 uint32_t ack=output_word(p,q.global_out),req=input_word(p,q.global_in);
 if((ack^req)&(1u<<27)) {
  IOLog("X9 CSF QUEUE: global fatal ack=%08x req=%08x status=%08x info=%08x:%08x\n",
   ack,req,output_word(p,q.global_out,0x10),output_word(p,q.global_out,0x24),output_word(p,q.global_out,0x20));
  shader_diagnostics(p,q,gpu);return false;
 }
 return true;
}
static bool global_configure(Plan &p,QueueProbe &q,vm_offset_t gpu,vm_offset_t db) {
 /* Command-only mode keeps allocation0. Compute mode permits one present
  * shader core and sets the stock nonzero800us firmware poweroff timer.
  * Zero selects host-controlled shader power and contradicts delegation. */
 const uint32_t mask=q.compute?14u:6u,idle_mask=1u<<10;
 uint64_t alloc=q.compute?q.shader_mask:0;
 uint32_t req=input_word(p,q.global_in),ack=output_word(p,q.global_out);
 uint32_t wanted=(req&~(mask|idle_mask))|((ack^mask)&mask);
 if(!host_set(p,q.global_in+0x18,(uint32_t)alloc)||!host_set(p,q.global_in+0x1c,(uint32_t)(alloc>>32))||
    (q.compute&&(!host_set(p,q.global_in+0x14,q.shader_pwroff_count)||
      !host_set(p,q.global_in+0x88,input_word(p,q.global_in,0x88)|1u)))||
    !host_set(p,q.global_in+0x10,0xffffffffu)||
    !host_set(p,q.global_in+4,input_word(p,q.global_in,4)|mask|(1u<<27))||
    !host_set(p,q.global_in,wanted)||!clean_page(p,q.global_in))return false;
 gw(db,0,1);
 uint64_t end=deadline_ms(1000);
 do {
  ack=output_word(p,q.global_out);
  if(!global_healthy(p,q,gpu))return false;
  if(!((ack^wanted)&(mask|idle_mask))) {
   IOLog("X9 CSF QUEUE: global configuration acknowledged shader-mask=%llx firmware-power=%u\n",alloc,q.compute);return true;
  }
  sleep_ms();
 }while(mach_absolute_time()<end);
 IOLog("X9 CSF QUEUE: global configuration timeout req=%08x ack=%08x\n",wanted,ack);return false;
}
static bool ring_group(Plan &p,QueueProbe &q,vm_offset_t db,uint32_t *wanted) {
 uint32_t ack=output_word(p,q.global_out,8),old=input_word(p,q.global_in,8);
 *wanted=(old&~1u)|((ack^1u)&1u);
 if(!host_set(p,q.global_in+8,*wanted)||!clean_page(p,q.global_in))return false;
 gw(db,0,1);return true;
}
static bool ring_stream(Plan &p,QueueProbe &q,vm_offset_t db,uint32_t *global,uint32_t *group) {
 uint32_t ack=output_word(p,q.group_out,8),old=input_word(p,q.group_in,8);
 *group=(old&~1u)|((ack^1u)&1u);
 if(!host_set(p,q.group_in+8,*group)||!clean_page(p,q.group_in))return false;
 return ring_group(p,q,db,global);
}
static bool wait_group(Plan &p,QueueProbe &q,vm_offset_t gpu,uint32_t global,
                       uint32_t group,unsigned state,bool stream,unsigned ms) {
 uint64_t end=deadline_ms(ms);
 do {
  uint32_t gack=output_word(p,q.global_out,8),cack=output_word(p,q.group_out,8);
  uint32_t ack=output_word(p,stream?q.stream_out:q.group_out);
  if(!((gack^global)&1)&&(!stream||!((cack^group)&1))&&(ack&7)==state)return true;
  if(!global_healthy(p,q,gpu))return false;
  sleep_ms();
 }while(mach_absolute_time()<end);
 IOLog("X9 CSF QUEUE: %s state=%u wait failed GLB_DB=%08x CSG_DB=%08x CSG_ACK=%08x CS_ACK=%08x\n",
  stream?"CS":"CSG",state,output_word(p,q.global_out,8),output_word(p,q.group_out,8),
  output_word(p,q.group_out),output_word(p,q.stream_out));return false;
}
static bool queue_interfaces(Plan &p,QueueProbe &q,uint32_t in,uint32_t out) {
 q.global_in=in;q.global_out=out;
 unsigned char *group=cpu_address(p,0x04001000,0x4c);
 if(!group)return false;
 invalidate_cpu(group,0x4c);
 q.group_in=get32(group+4);q.group_out=get32(group+8);
 q.stream_in=get32(group+0x44);q.stream_out=get32(group+0x48);
 uint32_t suspend=get32(group+12),features=get32(group+0x40);
 if(!interface_address(p,in,4096)||!interface_address(p,out,4096)||in==out||
    !interface_address(p,q.group_in,0x70)||!interface_address(p,q.group_out,0x20)||
    !interface_address(p,q.stream_in,0x60)||!interface_address(p,q.stream_out,0xe0)||
    q.group_in==q.group_out||q.stream_in==q.stream_out||
    !suspend||suspend>queue_suspend_bytes||(features&0xffffffffu)==0xffffffffu)return false;
 if((output_word(p,q.group_out)&7)||(output_word(p,q.stream_out)&7)||
    ((output_word(p,q.stream_out)^input_word(p,q.stream_in))&0xc0000000u))return false;
 IOLog("X9 CSF QUEUE: CSG0/CS0 features=%08x suspend=%u inputs=%08x/%08x outputs=%08x/%08x\n",
  features,suspend,q.group_in,q.stream_in,q.group_out,q.stream_out);return true;
}
static void queue_diagnostics(Plan &p,QueueProbe &q) {
 IOLog("X9 CSF QUEUE: CSG_REQ=%08x CSG_ACK=%08x CS_REQ=%08x CS_ACK=%08x fault=%08x fatal=%08x PC=%08x:%08x wait=%08x scoreboards=%08x blocked=%08x extract=%llu\n",
  input_word(p,q.group_in),output_word(p,q.group_out),input_word(p,q.stream_in),
  output_word(p,q.stream_out),output_word(p,q.stream_out,0x80),
  output_word(p,q.stream_out,0x84),output_word(p,q.stream_out,0x44),output_word(p,q.stream_out,0x40),
  output_word(p,q.stream_out,0x48),output_word(p,q.stream_out,0x5c),output_word(p,q.stream_out,0x60),output_qword(p,queue_user+4096));
}
static bool queue_start(Plan &p,QueueProbe &q,vm_offset_t gpu,vm_offset_t db) {
 /* Start empty, then submit through the assigned user doorbell, as kbase does. */
 if(q.reuse||q.compute) {
  unsigned stride=q.reuse?queue_dispatch_bytes:queue_batch_bytes;
  uint64_t previous=output_qword(p,queue_user+4096),insert=output_qword(p,queue_user);
  if(previous!=(uint64_t)q.completed*stride||insert!=previous) {
   IOLog("X9 CSF QUEUE: restart offsets rejected completed=%u insert=%llu extract=%llu\n",
    q.completed,insert,previous);return false;
  }
  if(!put_qword(p,queue_user+8,previous)||!clean_page(p,queue_user))return false;
 }
 q.touched=true; /* Conservatively own the queue before input publication. */
 if(!host_set(p,q.stream_in+0x10,queue_ring)||!host_set(p,q.stream_in+0x14,0)||
    !host_set(p,q.stream_in+0x18,host_page)||
    !host_set(p,q.stream_in+0x30,queue_user)||!host_set(p,q.stream_in+0x34,0)||
    !host_set(p,q.stream_in+0x38,queue_user+4096)||!host_set(p,q.stream_in+0x3c,0)||
    !host_set(p,q.stream_in+4,1u<<8)||!host_set(p,q.stream_in+0xc,0xffffffffu))return false;
 uint32_t csreq=(input_word(p,q.stream_in)&~7u)|1u|(1u<<8)|(1u<<10)|(1u<<12);
 if(!host_set(p,q.stream_in,csreq))return false;
 /* Default probes request no endpoints; compute enables just one compute
  * endpoint from the shader mask whose power control we delegated. */
 for(unsigned off=0x18;off<=0x3c;off+=4)if(!host_set(p,q.group_in+off,0))return false;
 if(q.compute&&(!host_set(p,q.group_in+0x20,(uint32_t)q.shader_mask)||
    !host_set(p,q.group_in+0x24,(uint32_t)(q.shader_mask>>32))||
    !host_set(p,q.group_in+0x38,1)))return false;
 if(!host_set(p,q.group_in+0x40,queue_suspend)||!host_set(p,q.group_in+0x44,0)||
    !host_set(p,q.group_in+0x48,0)||!host_set(p,q.group_in+0x4c,0)||
    !host_set(p,q.group_in+0x50,1)||!host_set(p,q.group_in+4,0xffffffffu)||
    !host_set(p,q.group_in,(input_word(p,q.group_in)&~7u)|1)||
    !clean_page(p,q.stream_in)||!clean_page(p,q.group_in))return false;
 uint32_t global,group;
 if(!ring_stream(p,q,db,&global,&group)||!wait_group(p,q,gpu,global,group,1,false,1000)||
    !wait_group(p,q,gpu,global,group,1,true,1000))return false;
 IOLog("X9 CSF QUEUE: CSG0 and CS0 START acknowledged; AS1, user-doorbell1, producer caught up\n");return true;
}
/* Stock kbase_gpu_cache_flush_pa_range_and_busy_wait: ARG0=start PA,
 * ARG1=inclusive end PA, command08 with FULL clean/invalidate payload73.
 * Our supported e880 device uses this v14.8 register map. Never flush any
 * guessed PA: derive the target4-KiB command page from its held descriptor. */
static bool queue_sync_ring(QueueProbe &q,vm_offset_t gpu,unsigned round,uint32_t va) {
 uint64_t pa=0;
 for(unsigned i=0;i<q.memory.region_count;i++) {
  Region &r=q.memory.regions[i];
  if(r.memory&&va>=r.begin&&(uint64_t)va+gpu_page<=r.end) {
   if(!physical(r.memory,va-r.begin,&pa))return false;
   break;
  }
 }
 if(!pa||!no_faults(gpu))return false;
 const uint32_t done=1u<<20;
 if(gr(gpu,0x20)&done) {
  IOLog("X9 CSF QUEUE: stale PA-range completion before round=%u; no range command issued\n",round+1);return false;
 }
 uint64_t old_arg0=gr64(gpu,0xd0),old_arg1=gr64(gpu,0xd8);
 /* One issued command, no retry. Keep all DMA references on uncertainty. */
 q.range_uncertain=true;
 gw64(gpu,0xd0,pa);gw64(gpu,0xd8,pa+gpu_page-1);gw(gpu,0x30,0x7308u);
 uint64_t end=deadline_ms(1000);
 do {
  uint32_t irq=gr(gpu,0x20);
  if(!no_faults(gpu))break;
  if(irq&done) {
   gw(gpu,0x24,done);
   gw64(gpu,0xd0,old_arg0);gw64(gpu,0xd8,old_arg1);
   q.range_uncertain=false;
   if((!q.reuse&&!q.compute)||queue_verbose(round))IOLog("X9 CSF QUEUE: round=%u GPU command PA-range clean/invalidate complete va=%08x pa=%llx bytes=%u\n",
    round+1,va,pa,gpu_page);return true;
  }
  sleep_ms();
 }while(mach_absolute_time()<end);
 IOLog("X9 CSF QUEUE: PA-range completion not confirmed round=%u irq=%08x; references retained\n",
  round+1,gr(gpu,0x20));return false;
}
static bool queue_submit(Plan &p,QueueProbe &q,vm_offset_t gpu,vm_offset_t user_db,unsigned round) {
 if(q.compute)return shader_submit(p,q,gpu,user_db,round);
 uint32_t expected=queue_expected(round),stride=q.reuse?queue_dispatch_bytes:queue_batch_bytes;
 uint32_t offset=round*stride;
 if(q.dynamic||q.indirect) {
  uint32_t at=q.indirect?queue_bodies+(round%((q.reuse||q.compute)?4u:3u))*gpu_page:queue_ring+offset;
  if(q.indirect) {
   /* Every prior submission must finish before a page can change owner.
    * Validate old body bytes before reusing them; never modify a live body. */
   if(q.completed!=round||output_qword(p,queue_user+4096)!=(uint64_t)offset) {
    IOLog("X9 CSF QUEUE: body ownership rejected round=%u completed=%u\n",round+1,q.completed);return false;
   }
   unsigned char *prior=cpu_address(q.memory,at,queue_batch_bytes);
   if(!prior)return false;
   if(round<(q.reuse?4u:3u)) {
    for(unsigned i=0;i<queue_batch_bytes;i++)if(prior[i])return false;
   }else {
    uint64_t old_words[3];memcpy(old_words,prior,sizeof(old_words));
    if(old_words[0]!=((1ULL<<56)|queue_result)||
       old_words[1]!=((2ULL<<56)|(2ULL<<48)|queue_expected(round-(q.reuse?4u:3u)))||
       old_words[2]!=((0x26ULL<<56)|(2ULL<<32))) {
     IOLog("X9 CSF QUEUE: old task-body contents rejected before reuse\n");return false;
    }
    if((!q.reuse&&!q.compute)||queue_verbose(round))IOLog("X9 CSF QUEUE: round=%u reclaim body=%08x previous-round=%u after extract=%u\n",
     round+1,at,round+1-(q.reuse?4u:3u),offset);
   }
  }
  if(!put_qword(q.memory,at,(1ULL<<56)|queue_result)||
     !put_qword(q.memory,at+8,(2ULL<<56)|(2ULL<<48)|expected)||
     !put_qword(q.memory,at+16,(0x26ULL<<56)|(2ULL<<32))||
     !clean_page(q.memory,at)||!queue_sync_ring(q,gpu,round,at&~4095u))return false;
  if(q.indirect&&(!q.reuse||queue_verbose(round)))IOLog("X9 CSF QUEUE: round=%u live body=%08x bytes=%u generated and synchronized\n",
   round+1,at,queue_batch_bytes);
 }
 /* Expose only bytes whose CPU publication and optional GPU cache sync
  * finished. Static mode keeps pan151's immutable prepublished stream. */
 if(!put_qword(p,queue_user,offset+stride)||!clean_page(p,queue_user))return false;
 gw(user_db,0,1);
 uint64_t end=deadline_ms(1500),extract=0;uint32_t actual=0;
 do {
  actual=output_word(q.memory,queue_result);
  extract=output_qword(p,queue_user+4096);
  if(!global_healthy(p,q,gpu))break;
  if((output_word(p,q.stream_out)^input_word(p,q.stream_in))&0xc0000000u)break;
  if(actual==expected&&extract==offset+stride) {
   if(output_word(q.memory,queue_result,4)!=0||output_word(q.memory,queue_result,64)!=0xa5a5a5a5u) {
    IOLog("X9 CSF QUEUE: sync error-word/guard rejected\n");return false;
   }
   q.completed=round+1;
   if((!q.reuse&&!q.compute)||queue_verbose(round))IOLog("X9 CSF QUEUE: round=%u SYNC_SET32=%08x extract=%llu verified; no CPU result writes after publication\n",
    round+1,actual,extract);return true;
  }
  sleep_ms();
 }while(mach_absolute_time()<end);
 IOLog("X9 CSF QUEUE: round=%u incomplete expected=%08x actual=%08x insert=%u extract=%llu\n",
  round+1,expected,actual,offset+stride,extract);queue_diagnostics(p,q);return false;
}
static bool queue_stop(Plan &p,QueueProbe &q,vm_offset_t gpu,vm_offset_t db) {
 if(!q.touched)return true;
 uint32_t global,group;
 if(!host_set(p,q.stream_in,input_word(p,q.stream_in)&~7u)||!clean_page(p,q.stream_in)||
    !ring_stream(p,q,db,&global,&group)||!wait_group(p,q,gpu,global,group,0,true,1000)) {
  queue_diagnostics(p,q);return false;
 }
 if(!host_set(p,q.group_in,input_word(p,q.group_in)&~7u)||!clean_page(p,q.group_in)||
    !ring_group(p,q,db,&global)||!wait_group(p,q,gpu,global,0,0,false,1000)) {
  queue_diagnostics(p,q);return false;
 }
 q.touched=false;
 if(gr(gpu,0x1000)&1u)gw(gpu,0x1004,1u);
 IOLog("X9 CSF QUEUE: CS0 STOP + CSG0 TERMINATE acknowledged with fresh doorbell ACKs\n");return true;
}
static int queue_run(Plan &p,QueueProbe &q,vm_offset_t gpu,vm_offset_t db,uint32_t in,uint32_t out) {
 if(!q.requested)return 0;
 if(!queue_interfaces(p,q,in,out)||!global_configure(p,q,gpu,db)) {
  IOLog("X9 CSF QUEUE: interface/global configuration rejected before queue start\n");return 0;
 }
 vm_offset_t user_db=ml_io_map(0x48090000,0x1000);
 if(!user_db)return 0;
 q.memory.device_exposed=true;q.as_installed=true;
 if(!install_as(gpu,q.memory.tables[0].pa,1)) {
  IOLog("X9 CSF QUEUE: AS1 install failed\n");return 0;
 }
 bool passed=true;
 if(q.user) {
  passed=queue_start(p,q,gpu,db)&&(active_desktop_job?desktop_submit(p,q,gpu,user_db):user_submit(p,q,gpu,user_db));
 }else if(q.compute) {
  const unsigned ends[]={9,41,x9_compute_campaign_count};
  uint64_t campaign_end=deadline_ms(90000);
  for(unsigned phase=0;passed&&phase<3;phase++) {
   IOLog("X9 VECTOR: phase=%u BEGIN completed=%u target=%u\n",phase+1,q.completed,ends[phase]);
   passed=queue_start(p,q,gpu,db);
   while(passed&&q.completed<ends[phase]) {
    if(mach_absolute_time()>=campaign_end) {
     IOLog("X9 VECTOR:90s campaign deadline at completed=%u\n",q.completed);passed=false;break;
    }
    passed=queue_submit(p,q,gpu,user_db,q.completed);
   }
   if(!passed)break;
   if(!queue_stop(p,q,gpu,db)){q.stop_uncertain=true;passed=false;break;}
   IOLog("X9 VECTOR: phase=%u PASS completed=%u fence=%08x insert=%llu extract=%llu\n",
    phase+1,q.completed,output_word(q.memory,queue_result),output_qword(p,queue_user),output_qword(p,queue_user+4096));
  }
  if(passed)IOLog("X9 VECTOR: campaign PASS75 dispatches;3 scalar,6 geometry,64 reuse,2 GPU-dependency;two restarts;final readback still required\n");
 }else if(q.reuse) {
  /* One boot covers initial reuse, wrap past16KiB and two full STOP/START
   * cycles. No root-ring rewrites, MCU resets or automatic retries. */
  const unsigned rounds[]={32,544,32};
  uint64_t campaign_end=deadline_ms(30000);
  for(unsigned phase=0;passed&&phase<3;phase++) {
   IOLog("X9 CSF QUEUE: phase=%u BEGIN completed=%u target=%u producer=%llu\n",
    phase+1,q.completed,rounds[phase],(uint64_t)q.completed*queue_dispatch_bytes);
   passed=queue_start(p,q,gpu,db);
   for(unsigned i=0;passed&&i<rounds[phase];i++) {
    if(mach_absolute_time()>=campaign_end) {
     IOLog("X9 CSF QUEUE: campaign30s deadline reached at completed=%u\n",q.completed);
     passed=false;break;
    }
    unsigned round=q.completed;
    passed=queue_submit(p,q,gpu,user_db,round);
    if(passed&&q.completed%queue_dispatch_slots==0)
     IOLog("X9 CSF QUEUE: ring wrap verified completed=%u extract=%llu cycle=%u\n",
      q.completed,output_qword(p,queue_user+4096),q.completed/queue_dispatch_slots);
   }
   if(!passed)break;
   if(!queue_stop(p,q,gpu,db)){q.stop_uncertain=true;passed=false;break;}
   IOLog("X9 CSF QUEUE: phase=%u PASS completed=%u insert=%llu extract=%llu result=%08x\n",
    phase+1,q.completed,output_qword(p,queue_user),output_qword(p,queue_user+4096),
    output_word(q.memory,queue_result));
  }
  if(passed)IOLog("X9 CSF QUEUE: campaign PASS608 writes,604 page reuses,ring wrap,two restarts; extract=19456; no shaders\n");
 }else {
  passed=queue_start(p,q,gpu,db);
  for(unsigned round=0;passed&&round<3;round++)passed=queue_submit(p,q,gpu,user_db,round);
  if(passed&&q.compute)IOLog("X9 SHADER: three compute dispatches and arithmetic readbacks verified\n");
  else if(passed)IOLog("X9 CSF QUEUE: three command-stream writes and ring consumption verified; %s ring, no shaders dispatched\n",
   q.indirect?"indirect live-body":(q.dynamic?"dynamic synchronized":"prepublished"));
 }

 return passed?1:0;
}

/* Observe only after the existing STOP/TERMINATE, MCU stop and full GPU cache
 * flush. No extra hardware cache operation or result write is introduced. */
static void queue_post_flush(Plan &p,QueueProbe &q) {
 if(!q.as_installed)return;
 IOLog("X9 CSF QUEUE: post-stop/full-flush result=%08x error=%08x guard=%08x insert=%llu extract=%llu\n",
  output_word(q.memory,queue_result),output_word(q.memory,queue_result,4),
  output_word(q.memory,queue_result,64),output_qword(p,queue_user),output_qword(p,queue_user+4096));
}
