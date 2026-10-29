/* MT6993 shader compute bring-up. Descriptor/CS input comes from frozen
 * Mesa v14 XML and hw_runner; power protocol comes from stock a16w driver.
 * Only the shader writes the output allocation after the initial sentinel. */
constexpr uint32_t shader_code_va=0x10010000,shader_desc_va=0x10014000,shader_data_va=0x10018000;
constexpr uint32_t vector_c=0x10020000,vector_d=0x10024000;
constexpr uint32_t vector_a=0x10028000,vector_b=0x1002c000;
constexpr unsigned vector_records=512,vector_record_bytes=16;
static bool global_healthy(Plan &,QueueProbe &,vm_offset_t);
static void queue_diagnostics(Plan &,QueueProbe &);
static bool queue_sync_ring(QueueProbe &,vm_offset_t,unsigned,uint32_t);
static bool shader_prepare(QueueProbe &q,vm_offset_t gpu) {
 if(!q.compute)return true;
 uint64_t present=gr64(gpu,0xa00),ready=gr64(gpu,0xa08);
 uint64_t stack=gr64(gpu,0xbc8),base=gr64(gpu,0xb88),control=gr64(gpu,0x820);
 uint32_t features=gr(gpu,0x60);
 /* Stock delegate_pm_domains_control_to_fw always hands TILER and SHADER
  * to firmware, even for compute-only jobs. NEURAL depends on GPU_FEATURES4. */
 q.power_domains=(1u<<1)|(1u<<8);
 if(features&(1u<<4))q.power_domains|=1u<<9;
 IOLog("X9 SHADER: present=%llx ready=%llx stack=%llx base=%llx PWR_STATUS=%llx\n",
  present,ready,stack,base,control);
 IOLog("X9 SHADER: GPU_FEATURES=%08x required-domains=%08x tiler-ready=%llx neural=%u\n",
  features,q.power_domains,gr64(gpu,0x948),!!(features&(1u<<4)));
 if(features==~0u||!present||present==~0ULL||ready||stack||base||
    gr64(gpu,0xa10)||gr64(gpu,0xb90)||gr64(gpu,0xbd0)||
    gr64(gpu,0x948)||gr64(gpu,0x950)||
    ((q.power_domains&(1u<<9))&&(gr64(gpu,0xa48)||gr64(gpu,0xa50)))||
    (control&q.power_domains)!=q.power_domains||
    (control&(((uint64_t)q.power_domains<<16)|(1ULL<<41)|(1ULL<<43)))) {
  IOLog("X9 SHADER: exclusive required-domain ownership/all-off gate rejected\n");return false;
 }
 uint64_t freq;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(freq));
 /* Stock convert_dur_to_core_pwroff_count: duration800000ns * CNTFRQ /1e9,
  * SYSTEM_TIMESTAMP sourcebit31=0, NO_MODIFIER1 for counts<S32_MAX.
  * The stock lower bound is1; zero selects host shader power management. */
 if(!freq||freq>0xffffffffULL)return false;
 uint64_t count=800000ULL*freq/1000000000ULL;
 if(!count)count=1;
 if(count>=0x7fffffffULL)return false;
 q.shader_pwroff_count=(uint32_t)count;
 IOLog("X9 SHADER: firmware poweroff800us CNTFRQ=%llu count=%u source=SYSTEM_TIMESTAMP no-modifier=1\n",
  freq,q.shader_pwroff_count);
 q.shader_mask=present&(~present+1); /* One actually present shader core. */
 if(!add_shared(q.memory,shader_code_va,host_page,0xdu)||
    !add_shared(q.memory,shader_desc_va)||!add_shared(q.memory,shader_data_va)||
    !add_shared(q.memory,vector_c)||!add_shared(q.memory,vector_d)||
    !add_shared(q.memory,vector_a)||!add_shared(q.memory,vector_b))return false;
 unsigned char *code=cpu_address(q.memory,shader_code_va,sizeof(x9_compute_shader));
 unsigned char *desc=cpu_address(q.memory,shader_desc_va,host_page);
 unsigned char *data=cpu_address(q.memory,shader_data_va,host_page);
 if(!code||!desc||!data)return false;
 memcpy(code,x9_compute_shader,sizeof(x9_compute_shader));
 unsigned char *vector_code=cpu_address(q.memory,shader_code_va+256,sizeof(x9_vector_shader));
 if(!vector_code)return false;
 memcpy(vector_code,x9_vector_shader,sizeof(x9_vector_shader));
 for(uint32_t va=vector_c;va<=vector_b;va+=host_page) {
  unsigned char *buffer=cpu_address(q.memory,va,host_page);
  if(!buffer)return false;
  memset(buffer,0xa5,host_page);
 }
 memcpy(desc,x9_compute_spd,sizeof(x9_compute_spd));
 memcpy(desc+64,x9_compute_tsd,sizeof(x9_compute_tsd));
 memcpy(desc+128,x9_vector_spd,sizeof(x9_vector_spd));
 memset(data,0xa5,host_page);
 for(unsigned i=0;i<3;i++) {
  uint64_t pointer=shader_data_va+i*128;
  uint64_t operands=(uint64_t)x9_compute_operands[i][0]|((uint64_t)x9_compute_operands[i][1]<<32);
  memcpy(desc+256+i*64,&pointer,8);memcpy(desc+264+i*64,&operands,8);
 }
 IOLog("X9 SHADER: assembled40-byte MOV/MOV/MOV/IADD/STORE.end; RX code=%08x descriptor=%08x output=%08x; mask=%llx\n",
  shader_code_va,shader_desc_va,shader_data_va,q.shader_mask);
 return true;
}
static bool shader_delegate(QueueProbe &q,vm_offset_t gpu) {
 if(!q.compute)return true;
 /* Match stock before-L2 order: TILER1, SHADER8, NEURAL9 if present.
  * Completion is tracked per domain, so a partial failure never loses the
  * outstanding ownership obligation or releases outer power. */
 uint64_t state=gr64(gpu,0x820);
 if((state&q.power_domains)!=q.power_domains||
    (state&(((uint64_t)q.power_domains<<16)|(1ULL<<41)|(1ULL<<43))))return false;
 const unsigned domains[]={1,8,9};
 for(unsigned i=0;i<3;i++) {
  unsigned domain=domains[i];uint32_t bit=1u<<domain;
  if(!(q.power_domains&bit))continue;
  q.power_uncertain=true;gw(gpu,0x828,(domain<<8)|0x20u);
  uint64_t end=deadline_ms(1000);
  do {
   state=gr64(gpu,0x820);
   uint32_t expected=q.delegated_domains|bit;
   if(state!=~0ULL&&!(state&expected)&&
      (state&((uint64_t)expected<<16))==((uint64_t)expected<<16)&&
      !(state&((1ULL<<41)|(1ULL<<43)))) {
    q.delegated_domains=expected;q.power_uncertain=false;break;
   }
   sleep_ms();
  }while(mach_absolute_time()<end);
  if(q.power_uncertain) {
   IOLog("X9 SHADER: delegation not confirmed domain=%u completed=%08x PWR_STATUS=%llx; outer power retained\n",
    domain,q.delegated_domains,state);return false;
  }
  IOLog("X9 SHADER: delegated %s domain=%u completed=%08x PWR_STATUS=%llx\n",
   domain==1?"TILER":domain==8?"SHADER":"NEURAL",domain,q.delegated_domains,state);
 }
 IOLog("X9 SHADER: required power-domain delegation complete required=%08x completed=%08x before L2/MCU\n",
  q.power_domains,q.delegated_domains);
 return q.delegated_domains==q.power_domains;
}
static bool shader_power_off(vm_offset_t gpu,unsigned domain,unsigned ready_off,unsigned trans_off) {
 uint64_t owned=gr64(gpu,ready_off),state=gr64(gpu,0x820);
 if(owned==~0ULL||state==~0ULL)return false;
 if(!owned)return gr64(gpu,trans_off)==0;
 if(!(state&(1ULL<<domain))||(state&(1ULL<<(16+domain)))||
    (state&((1ULL<<41)|(1ULL<<43)))) {
  IOLog("X9 SHADER: power-off ownership rejected domain=%u ready=%llx PWR_STATUS=%llx\n",domain,owned,state);return false;
 }
 gw64(gpu,0x830,owned);gw(gpu,0x828,(domain<<8)|0x11u);
 uint64_t end=deadline_ms(1000);
 do {
  uint64_t ready=gr64(gpu,ready_off),trans=gr64(gpu,trans_off);
  if(ready!=~0ULL&&trans!=~0ULL&&!(ready&owned)&&!(trans&owned))return true;
  sleep_ms();
 }while(mach_absolute_time()<end);
 IOLog("X9 SHADER: power-off not confirmed domain=%u ready=%llx trans=%llx\n",
  domain,gr64(gpu,ready_off),gr64(gpu,trans_off));return false;
}
static bool shader_quiesce(QueueProbe &q,vm_offset_t gpu) {
 if(!q.compute)return true;
 if(q.power_uncertain)return false;
 /* Caller confirmed CSG terminate, MCU stopped and GPU cache flush.
  * Retract only successfully delegated domains, in stock TILER/SHADER/NEURAL
  * order. A failed/uncertain step stops here without retry or GPU reset. */
 const unsigned domains[]={1,8,9};
 for(unsigned i=0;i<3;i++) {
  unsigned domain=domains[i];uint32_t bit=1u<<domain;
  if(!(q.delegated_domains&bit))continue;
  q.power_uncertain=true;gw(gpu,0x828,(domain<<8)|0x21u);
  uint64_t end=deadline_ms(1000),state=0;
  do {
   state=gr64(gpu,0x820);
   if(state!=~0ULL&&!(state&(((uint64_t)bit<<16)|(1ULL<<41)|(1ULL<<43)))&&(state&bit)) {
    q.delegated_domains&=~bit;q.power_uncertain=false;break;
   }
   sleep_ms();
  }while(mach_absolute_time()<end);
  if(q.power_uncertain) {
   IOLog("X9 SHADER: retract not confirmed domain=%u remaining=%08x PWR_STATUS=%llx; retaining DMA and power\n",
    domain,q.delegated_domains,state);return false;
  }
  IOLog("X9 SHADER: retracted domain=%u remaining=%08x PWR_STATUS=%llx\n",domain,q.delegated_domains,state);
 }
 /* Return every domain to its observed initial all-off state. No command
  * is sent for an already-off domain or one not permitted by live host control.
  * TILER must be off before the caller releases L2. */
 if(!shader_power_off(gpu,8,0xa08,0xa10)||
    ((q.power_domains&(1u<<9))&&!shader_power_off(gpu,9,0xa48,0xa50))||
    !shader_power_off(gpu,14,0xb88,0xb90)||!shader_power_off(gpu,15,0xbc8,0xbd0)||
    !shader_power_off(gpu,1,0x948,0x950))return false;
 IOLog("X9 SHADER: all delegated domains retracted; shader/neural/base/stack/tiler returned to initial all-off state\n");return true;
}
static void shader_diagnostics(Plan &p,QueueProbe &q,vm_offset_t gpu) {
 if(!q.compute||q.shader_dumped)return;
 q.shader_dumped=true;
 IOLog("X9 SHADER DIAG: GLB_ACK=%08x fatal=%08x info=%08x:%08x MCUctl=%08x MCUstatus=%08x GPUirq=%08x MMUirq=%08x PWRirq=%08x PWR=%llx\n",
  output_word(p,q.global_out),output_word(p,q.global_out,0x10),output_word(p,q.global_out,0x24),
  output_word(p,q.global_out,0x20),gr(gpu,0x700),gr(gpu,0x704),gr(gpu,0x20),gr(gpu,0x2000),gr(gpu,0x800),gr64(gpu,0x820));
 IOLog("X9 SHADER DIAG: shader ready/trans=%llx/%llx base=%llx/%llx stack=%llx/%llx L2=%llx/%llx\n",
  gr64(gpu,0xa08),gr64(gpu,0xa10),gr64(gpu,0xb88),gr64(gpu,0xb90),
  gr64(gpu,0xbc8),gr64(gpu,0xbd0),gr64(gpu,0x908),gr64(gpu,0x910));
 IOLog("X9 SHADER DIAG: required-domains=%08x delegated=%08x TILERready/trans=%llx/%llx\n",
  q.power_domains,q.delegated_domains,gr64(gpu,0x948),gr64(gpu,0x950));
 const unsigned char *b=csf_firmware_bytes;unsigned at=20,end=get32(b+16);
 while(at<end) {
  unsigned h=get32(b+at),size=(h>>8)&255;
  if(size<4||size>end-at)return;
  if((h&255)==3&&size>=40&&!strncmp((const char *)b+at+32,"fwlog",6)) {
   const unsigned char *e=b+at+4;
   uint32_t bytes=output_word(p,get32(e+4));
   uint32_t insert_va=output_word(p,get32(e+8)),extract_va=output_word(p,get32(e+12));
   uint32_t data_va=output_word(p,get32(e+16));
   uint32_t insert=output_word(p,insert_va),extract=output_word(p,extract_va);
   IOLog("X9 SHADER DIAG: fwlog data=%08x size=%u insert=%u extract=%u\n",data_va,bytes,insert,extract);
   if(bytes!=host_page||insert>=bytes||extract>=bytes||!cpu_address(p,data_va,bytes))return;
   unsigned available=(insert+bytes-extract)%bytes,amount=available<4096?available:4096;
   unsigned first=(insert+bytes-amount)%bytes;
   for(unsigned off=0;off<amount;off+=32) {
    unsigned n=amount-off<32?amount-off:32;char hex[65];
    for(unsigned j=0;j<n;j++) {
     unsigned char *value=cpu_address(p,data_va+(first+off+j)%bytes,1);
     invalidate_cpu(value,1);unsigned v=*value;
     static const char digits[]="0123456789abcdef";
     hex[j*2]=digits[v>>4];hex[j*2+1]=digits[v&15];
    }
    hex[n*2]=0;IOLog("X9 SHADER FWLOG +%u %s\n",off,hex);
   }
   return;
  }
  at+=size;
 }
}
#include "SweetX9CSFVector.inc"
#include "SweetX9CSFDesktop.inc"
#include "SweetX9CSFUser.inc"
static bool shader_submit(Plan &p,QueueProbe &q,vm_offset_t gpu,vm_offset_t user_db,unsigned round) {
 if(round>=3)return vector_submit(p,q,gpu,user_db,round);
 if(round>=3||q.completed!=round||output_qword(p,queue_user+4096)!=(uint64_t)round*queue_batch_bytes)return false;
 uint32_t body=queue_bodies+round*gpu_page;
 unsigned char *where=cpu_address(q.memory,body,x9_compute_cs_bytes);
 if(!where)return false;
 for(unsigned i=0;i<x9_compute_cs_bytes;i++)if(where[i])return false;
 /* Whole compute dispatch is generated only now; shader code/descriptors
  * and each immutable case's FAUs were published before GPU ownership. */
 memcpy(where,x9_compute_cs[round],x9_compute_cs_bytes);
 if(!clean_page(q.memory,body)||!queue_sync_ring(q,gpu,round,body))return false;
 uint32_t producer=(round+1)*queue_batch_bytes;
 if(!put_qword(p,queue_user,producer)||!clean_page(p,queue_user))return false;
 uint32_t a=x9_compute_operands[round][0],b=x9_compute_operands[round][1];
 uint32_t expected=a+b,fence=0x53484400u+round+1;
 IOLog("X9 SHADER: dispatch=%u inputs=%08x+%08x case-output=%08x CS-bytes=%u\n",
  round+1,a,b,shader_data_va+round*128,x9_compute_cs_bytes);
 gw(user_db,0,1);
 uint64_t end=deadline_ms(3000),extract=0;uint32_t actual=0,seen_fence=0;
 do {
  actual=output_word(q.memory,shader_data_va+round*128);
  seen_fence=output_word(q.memory,queue_result);
  extract=output_qword(p,queue_user+4096);
  if(!global_healthy(p,q,gpu)||
     ((output_word(p,q.stream_out)^input_word(p,q.stream_in))&0xc0000000u))break;
  if(actual==expected&&seen_fence==fence&&extract==producer) {
   /* Each result owns a128-byte slot: shader may change only its first u32. */
   unsigned char *slot=cpu_address(q.memory,shader_data_va+round*128,128);
   if(!slot)return false;
   invalidate_cpu(slot,128);
   for(unsigned i=4;i<128;i++)if(slot[i]!=0xa5) {
    IOLog("X9 SHADER: result guard corruption at slot=%u byte=%u\n",round,i);return false;
   }
   if(output_word(q.memory,queue_result,4)||output_word(q.memory,queue_result,64)!=0xa5a5a5a5u)return false;
   q.completed=round+1;
   IOLog("X9 SHADER: compute=%u ADD32 a=%08x b=%08x expected=%08x actual=%08x fence=%08x extract=%llu PASS ready=%llx\n",
    round+1,a,b,expected,actual,seen_fence,extract,gr64(gpu,0xa08));return true;
  }
  sleep_ms();
 }while(mach_absolute_time()<end);
 IOLog("X9 SHADER: compute=%u failed expected=%08x actual=%08x fence=%08x/%08x extract=%llu/%u ready=%llx trans=%llx PWR=%llx\n",
  round+1,expected,actual,seen_fence,fence,extract,producer,gr64(gpu,0xa08),gr64(gpu,0xa10),gr64(gpu,0x820));
 shader_diagnostics(p,q,gpu);
 queue_diagnostics(p,q);return false;
}
static void shader_post_flush(QueueProbe &q) {
 if(!q.compute||!q.as_installed)return;
 if(active_desktop_job) {active_desktop_job->cleaned=desktop_validate(q);return;}
 if(q.user) {
  active_metal_job->cleaned=active_metal_job->computed&&user_validate(q,true);
  return;
 }
 bool passed=q.completed>=3;
 for(unsigned i=0;i<3;i++) {
  uint32_t actual=output_word(q.memory,shader_data_va+i*128);
  uint32_t expected=x9_compute_operands[i][0]+x9_compute_operands[i][1];
  if(actual!=expected)passed=false;
  IOLog("X9 SHADER: final case=%u result=%08x expected=%08x\n",i+1,actual,expected);
 }
 IOLog("X9 SHADER: shader arithmetic/readback %s, cases=%u/3; CSF fence is a separate allocation\n",
  passed?"PASS":"INCOMPLETE",q.completed>=3?3u:q.completed);
 if(q.completed==x9_compute_campaign_count) {
  bool vectors=vector_validate_output(q,73,true)&&vector_validate_output(q,74,true);
  metal_startup_verified=passed&&vectors;
  IOLog("X9 VECTOR: final post-stop/full-flush readback %s; completed=%u/75; GPU dependency C=A+B,D=C+B\n",
   passed&&vectors?"PASS":"INCOMPLETE",q.completed);
 }else IOLog("X9 VECTOR: campaign INCOMPLETE completed=%u/75\n",q.completed);
}
