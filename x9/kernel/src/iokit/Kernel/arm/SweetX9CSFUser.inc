/* Synchronous built-in vector pipeline, entered only with a private copied
 * user request and the provider serialization lock. Never GPU-map user pages. */
static bool user_validate(QueueProbe &q,bool final) {
 if(!active_metal_job)return false;
 const X9MetalRequest &r=active_metal_job->request;
 unsigned char *out=cpu_address(q.memory,vector_c,host_page);
 unsigned char *a=cpu_address(q.memory,vector_a,host_page);
 unsigned char *b=cpu_address(q.memory,vector_b,host_page);
 if(!out||!a||!b)return false;
 invalidate_cpu(out,host_page);invalidate_cpu(a,host_page);invalidate_cpu(b,host_page);
 for(unsigned i=0;i<r.count;i++) {
  uint32_t expected[]={r.a[i]+r.b[i],i,i%r.local_x,i/r.local_x};
  if(memcmp(out+i*16,expected,16)||get32(a+i*16)!=r.a[i]||get32(b+i*16)!=r.b[i])return false;
  for(unsigned byte=4;byte<16;byte++)if(a[i*16+byte]!=0xa5||b[i*16+byte]!=0xa5)return false;
 }
 for(unsigned i=r.count*16;i<host_page;i++)if(out[i]!=0xa5||a[i]!=0xa5||b[i]!=0xa5)return false;
 if(output_word(q.memory,queue_result,4)||output_word(q.memory,queue_result,64)!=0xa5a5a5a5u)return false;
 /* Copy values read from GPU, never the expected arithmetic. */
 for(unsigned i=0;i<r.count;i++)active_metal_job->response.c[i]=get32(out+i*16);
 if(final)IOLog("X9 METAL GPU: final GPU readback count=%u local=%u PASS after MCU-stop/full-flush\n",r.count,r.local_x);
 return true;
}
static bool user_submit(Plan &p,QueueProbe &q,vm_offset_t gpu,vm_offset_t user_db) {
 if(!active_metal_job||q.completed||output_qword(p,queue_user)||output_qword(p,queue_user+4096))return false;
 const X9MetalRequest &r=active_metal_job->request;
 for(unsigned input=0;input<2;input++) {
  uint32_t va=input?vector_b:vector_a;unsigned char *data=cpu_address(q.memory,va,host_page);
  if(!data)return false;
  memset(data,0xa5,host_page);
  for(unsigned i=0;i<r.count;i++)memcpy(data+i*16,input?&r.b[i]:&r.a[i],4);
  if(!vector_sync(q,gpu,0,va,host_page))return false;
 }
 unsigned char *out=cpu_address(q.memory,vector_c,host_page);
 unsigned char *fau=cpu_address(q.memory,shader_desc_va+512,32);
 unsigned char *body=cpu_address(q.memory,queue_bodies,x9_compute_cs_bytes);
 if(!out||!fau||!body)return false;
 for(unsigned i=0;i<x9_compute_cs_bytes;i++)if(body[i])return false;
 memset(out,0xa5,host_page);
 if(!vector_sync(q,gpu,0,vector_c,host_page))return false;
 uint64_t pointers[]={vector_a,vector_b,vector_c,0};memcpy(fau,pointers,sizeof(pointers));
 if(!vector_sync(q,gpu,0,shader_desc_va,gpu_page))return false;
 uint64_t commands[28];static_assert(sizeof(commands)==x9_compute_cs_bytes,"user CALL body ABI");memcpy(commands,x9_campaign_cs[3],sizeof(commands));
 /* Frozen v14 MOVE32 bit56opcode2, bit48destination. Override only geometry
  * and FAU pointer from the compiled one-element vector command template. */
 for(unsigned i=0;i<28;i++) {
  if((commands[i]>>56)==2) {
   unsigned reg=(commands[i]>>48)&255;
   if(reg==33)commands[i]=(commands[i]&~0xffffffffULL)|((1u<<31)|(r.local_x-1));
   if(reg==37)commands[i]=(commands[i]&~0xffffffffULL)|(r.count/r.local_x);
  }
  /* Template dispatch4's FAU points at descriptor+512+3*64. */
  if((commands[i]>>56)==2&&((commands[i]>>48)&255)==8)
   commands[i]=(commands[i]&~0xffffffffULL)|(shader_desc_va+512);
 }
 memcpy(body,commands,sizeof(commands));
 if(!vector_sync(q,gpu,0,queue_bodies,gpu_page)||!put_qword(p,queue_user,24)||!clean_page(p,queue_user))return false;
 gw(user_db,0,1);uint64_t end=deadline_ms(3000);
 do {
  if(!global_healthy(p,q,gpu)||((output_word(p,q.stream_out)^input_word(p,q.stream_in))&0xc0000000u))break;
  if(output_word(q.memory,queue_result)==0x53484404u&&output_qword(p,queue_user+4096)==24) {
   if(!user_validate(q,false))break;
   q.completed=1;active_metal_job->computed=true;
   IOLog("X9 METAL GPU: user dispatch count=%u local=%u actual-values+IDs+guards PASS fence=53484404 extract=24\n",r.count,r.local_x);
   return true;
  }
  sleep_ms();
 }while(mach_absolute_time()<end);
 IOLog("X9 METAL GPU: user dispatch failed; no retry or CPU arithmetic fallback\n");
 shader_diagnostics(p,q,gpu);queue_diagnostics(p,q);return false;
}
