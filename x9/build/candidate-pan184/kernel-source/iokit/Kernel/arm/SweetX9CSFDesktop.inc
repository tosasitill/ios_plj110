#include "x9_desktop_program.inc"
/* Native IOSurface rectangle copy. Uses the verified CSF/PM lifecycle;
 * 2-D global IDs and multiply instructions are new hardware coverage. */
constexpr unsigned desktop_frame_width=1256,desktop_frame_height=2760;
constexpr unsigned desktop_frame_source_stride=5120,desktop_frame_destination_stride=5184;
constexpr unsigned desktop_frame_offset=64;
constexpr unsigned desktop_frame_bytes=((desktop_frame_offset+desktop_frame_destination_stride*desktop_frame_height+host_page-1)&~(host_page-1));
static_assert(desktop_frame_bytes<=16u*1024u*1024u,"bounded desktop buffer");
constexpr uint32_t desktop_source_va=0x20000000,desktop_destination_va=0x24000000;
static bool desktop_borrow(QueueProbe &q,IOMemoryDescriptor *m,uint32_t gpu_va,IODirection direction) {
 IOBufferMemoryDescriptor *buffer=OSDynamicCast(IOBufferMemoryDescriptor,m);
 if(!buffer||buffer->getLength()!=desktop_frame_bytes||q.memory.borrowed_count>=2)return false;
 if(buffer->prepare(direction)!=kIOReturnSuccess)return false;
 buffer->retain();q.memory.borrowed[q.memory.borrowed_count++]={buffer,direction};
 for(unsigned off=0;off<desktop_frame_bytes;off+=gpu_page) {
  uint64_t pa;
  if(!physical(buffer,off,&pa)||pa>=(1ULL<<40)||
     (pa<0xfc910000ULL+5120u*2760u&&pa+gpu_page>0xfc910000ULL))return false;
  if(!map_page(q.memory,gpu_va+off,pa,direction==kIODirectionOut?0xc0000001u:0xc0000003u))return false;
  clean_dcache64(pa,gpu_page,1);
 }
 __asm__ volatile("dsb sy" ::: "memory");return true;
}
static bool desktop_prepare(QueueProbe &q) {
 if(!active_desktop_job)return true;
 X9DesktopJob &j=*active_desktop_job;
 if(!j.source||!j.destination||!j.source_cpu||!j.destination_cpu||
    j.width!=desktop_frame_width||j.height!=desktop_frame_height||
    j.source_stride!=desktop_frame_source_stride||j.destination_stride!=desktop_frame_destination_stride||
    j.source_offset!=desktop_frame_offset||j.destination_offset!=desktop_frame_offset)return false;
 uint64_t source_end=j.source_offset+(uint64_t)(j.height-1)*j.source_stride+j.width*4;
 uint64_t destination_end=j.destination_offset+(uint64_t)(j.height-1)*j.destination_stride+j.width*4;
 if(source_end>j.source->getLength()||destination_end>j.destination->getLength()||destination_end>desktop_frame_bytes)return false;
 if(!desktop_borrow(q,j.source,desktop_source_va,kIODirectionOut)||
    !desktop_borrow(q,j.destination,desktop_destination_va,kIODirectionInOut))return false;
 unsigned char *code=cpu_address(q.memory,shader_code_va+512,sizeof(x9_desktop_shader));
 unsigned char *spd=cpu_address(q.memory,shader_desc_va+192,sizeof(x9_desktop_spd));
 if(!code||!spd)return false;
 memcpy(code,x9_desktop_shader,sizeof(x9_desktop_shader));memcpy(spd,x9_desktop_spd,sizeof(x9_desktop_spd));
 return true;
}
static bool desktop_validate(QueueProbe &q) {
 X9DesktopJob *j=active_desktop_job;
 if(!j||!j->computed||q.completed!=1)return false;
 invalidate_cpu(j->destination_cpu,desktop_frame_bytes);
 unsigned matched=0,changed=0;uint32_t digest=2166136261u;
 for(unsigned y=0;y<j->height;y++) {
  const unsigned char *source=j->source_cpu+j->source_offset+(uint64_t)y*j->source_stride;
  const unsigned char *output=j->destination_cpu+j->destination_offset+(uint64_t)y*j->destination_stride;
  if(memcmp(source,output,j->width*4)) {
   IOLog("X9 DESKTOP FRAME: sequence=%u row=%u pixel mismatch; no GPU result publication\n",j->sequence,y);return false;
  }
  for(unsigned x=0;x<j->width;x++) {
   uint32_t actual=get32(output+x*4);changed+=actual!=0xa5a5a5a5u;
   digest=(digest^actual)*16777619u;
  }
  matched+=j->width;
 }
 const uint8_t *buffers[]={j->source_cpu,j->destination_cpu};
 const uint32_t strides[]={j->source_stride,j->destination_stride};
 for(unsigned b=0;b<2;b++) {
  for(unsigned i=0;i<desktop_frame_offset;i++)if(buffers[b][i]!=0xa5)return false;
  for(unsigned y=0;y<j->height;y++) {
   uint64_t at=desktop_frame_offset+(uint64_t)y*strides[b]+j->width*4;
   for(unsigned x=j->width*4;x<strides[b];x++,at++)if(buffers[b][at]!=0xa5)return false;
  }
  for(uint64_t at=desktop_frame_offset+(uint64_t)j->height*strides[b];at<desktop_frame_bytes;at++)
   if(buffers[b][at]!=0xa5)return false;
 }
 if(!changed)return false;
 IOLog("X9 DESKTOP FRAME: sequence=%u post-stop/full-flush PASS pixels=%u changed-from-sentinel=%u guards=all digest=%08x; private output, panel unmapped\n",
  j->sequence,matched,changed,digest);
 return true;
}
static bool desktop_submit(Plan &p,QueueProbe &q,vm_offset_t gpu,vm_offset_t user_db) {
 X9DesktopJob *j=active_desktop_job;
 if(!j||q.completed||output_qword(p,queue_user)||output_qword(p,queue_user+4096))return false;
 unsigned char *fau=cpu_address(q.memory,shader_desc_va+512,32);
 unsigned char *body=cpu_address(q.memory,queue_bodies,x9_compute_cs_bytes);
 if(!fau||!body)return false;
 uint64_t params[]={desktop_source_va+j->source_offset,desktop_destination_va+j->destination_offset,
  (uint64_t)j->source_stride|((uint64_t)j->destination_stride<<32),0};
 memcpy(fau,params,sizeof(params));
 if(!vector_sync(q,gpu,0,shader_desc_va,gpu_page))return false;
 const unsigned local_x=8,local_y=8;
 if(j->width%local_x||j->height%local_y)return false;
 uint32_t fence=0x44000000u|(j->sequence&0xffffffu);
 uint64_t commands[28];memcpy(commands,x9_campaign_cs[3],sizeof(commands));
 for(unsigned i=0;i<28;i++) {
  unsigned opcode=commands[i]>>56,reg=(commands[i]>>48)&255;
  if(opcode==2) {
   uint32_t value=(uint32_t)commands[i];
   if(reg==8)value=shader_desc_va+512;
   if(reg==16)value=(shader_desc_va+192)|(uint32_t)(commands[i]&15);
   if(reg==33)value=x9_desktop_workgroup;
   if(reg==37)value=j->width/local_x;
   if(reg==38)value=j->height/local_y;
   if(reg==42)value=fence;
   commands[i]=(commands[i]&~0xffffffffULL)|value;
  }else if(opcode==1&&reg==16) {
   /* MOVE48 encodes low48 address plus register-allocation low bits. */
   commands[i]=(commands[i]&0xffff000000000000ULL)|((shader_desc_va+192)|(commands[i]&15));
  }
 }
 memcpy(body,commands,sizeof(commands));
 if(!vector_sync(q,gpu,0,queue_bodies,gpu_page)||!put_qword(p,queue_user,24)||!clean_page(p,queue_user))return false;
 uint64_t submitted=mach_absolute_time();
 gw(user_db,0,1);uint64_t end=deadline_ms(3000);
 do {
  if(!global_healthy(p,q,gpu)||((output_word(p,q.stream_out)^input_word(p,q.stream_in))&0xc0000000u))break;
  if(output_word(q.memory,queue_result)==fence&&output_qword(p,queue_user+4096)==24) {
   q.completed=1;j->computed=true;
   uint64_t dispatch_ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-submitted,&dispatch_ns);
   IOLog("X9 DESKTOP FRAME: sequence=%u shader fence=%08x extract=24 rectangle=%ux%u local=%ux%u groups=%ux%u doorbell-to-fence-us=%llu;private GPU output,panel unmapped\n",
    j->sequence,fence,j->width,j->height,local_x,local_y,j->width/local_x,j->height/local_y,dispatch_ns/1000);
   return true;
  }
  sleep_ms();
 }while(mach_absolute_time()<end);
 IOLog("X9 DESKTOP FRAME: sequence=%u dispatch failed; no reset or retry\n",j->sequence);
 shader_diagnostics(p,q,gpu);queue_diagnostics(p,q);return false;
}
