/* Included by SweetX9CSFCompute.inc. All dispatches are serialized by verified
 * CSF fence + EXTRACT. No output is CPU-filled with expected arithmetic. */
static uint32_t vector_input(unsigned seed,unsigned index,bool second) {
 /* Wrapping unsigned patterns exercise zero, signs, carries and all bit lanes. */
 return second?(0xfffffff0u+seed*0x10203041u-index*0x01010101u):
                (0x12345678u+seed*0x9e3779b9u+index*0x07060503u);
}
static bool vector_sync(QueueProbe &q,vm_offset_t gpu,unsigned round,uint32_t va,unsigned bytes) {
 if(!bytes||(va&4095)||(bytes&4095))return false;
 for(unsigned off=0;off<bytes;off+=gpu_page)
  if(!clean_page(q.memory,va+off)||!queue_sync_ring(q,gpu,round,va+off))return false;
 return true;
}
static bool vector_validate_inputs(QueueProbe &q,unsigned seed) {
 for(unsigned input=0;input<2;input++) {
  unsigned char *p=cpu_address(q.memory,input?vector_b:vector_a,host_page);
  if(!p)return false;
  invalidate_cpu(p,host_page);
  for(unsigned i=0;i<vector_records;i++) {
   uint32_t expected[]={vector_input(seed,i,input!=0),i,~i,seed};
   if(memcmp(p+i*16,expected,16)) {
    IOLog("X9 VECTOR: input corruption buffer=%u record=%u seed=%u\n",input,i,seed);return false;
   }
  }
  for(unsigned i=vector_records*16;i<host_page;i++)if(p[i]!=0xa5) {
   IOLog("X9 VECTOR: input tail corruption buffer=%u byte=%u\n",input,i);return false;
  }
 }
 return true;
}
static bool vector_validate_output(QueueProbe &q,unsigned round,bool announce) {
 if(round<3||round>=x9_compute_campaign_count)return false;
 const X9VectorCase &c=x9_vector_cases[round-3];
 unsigned begin=c.offset_x*c.local_x,end=begin+c.local_x*c.groups_x;
 if(end>vector_records||!c.local_x||!c.groups_x)return false;
 unsigned char *p=cpu_address(q.memory,c.output_va,host_page);
 if(!p)return false;
 invalidate_cpu(p,host_page);
 uint32_t digest=0;
 for(unsigned i=0;i<vector_records;i++) {
  if(i>=begin&&i<end) {
   uint32_t expected[]={vector_input(c.seed,i,false)+vector_input(c.seed,i,true)*(c.chain?2u:1u),
                        i,i%c.local_x,i/c.local_x};
   for(unsigned word=0;word<4;word++) {
    uint32_t actual=get32(p+i*16+word*4);
    if(actual!=expected[word]) {
     IOLog("X9 VECTOR: mismatch dispatch=%u gid=%u word=%u expected=%08x actual=%08x local=%u groups=%u offset=%u chain=%u\n",
      round+1,i,word,expected[word],actual,c.local_x,c.groups_x,c.offset_x,c.chain);return false;
    }
    digest=(digest<<5)|(digest>>27);digest^=actual;
   }
  }else for(unsigned byte=0;byte<16;byte++)if(p[i*16+byte]!=0xa5) {
   IOLog("X9 VECTOR: out-of-dispatch write dispatch=%u record=%u byte=%u\n",round+1,i,byte);return false;
  }
 }
 for(unsigned byte=vector_records*16;byte<host_page;byte++)if(p[byte]!=0xa5) {
  IOLog("X9 VECTOR: output tail guard failed dispatch=%u byte=%u\n",round+1,byte);return false;
 }
 if(!vector_validate_inputs(q,c.seed))return false;
 if(announce)IOLog("X9 VECTOR: dispatch=%u local=%u groups=%u offset=%u records=%u all-values+global/local/group-IDs+guards PASS digest=%08x chain=%u\n",
  round+1,c.local_x,c.groups_x,c.offset_x,end-begin,digest,c.chain);
 return true;
}
static bool vector_submit(Plan &p,QueueProbe &q,vm_offset_t gpu,vm_offset_t user_db,unsigned round) {
 if(round<3||round>=x9_compute_campaign_count||q.completed!=round||
    output_qword(p,queue_user+4096)!=(uint64_t)round*queue_batch_bytes||
    output_qword(p,queue_user)!=(uint64_t)round*queue_batch_bytes)return false;
 const X9VectorCase &c=x9_vector_cases[round-3];
 unsigned slot=round%4;uint32_t body=queue_bodies+slot*gpu_page;
 unsigned char *where=cpu_address(q.memory,body,x9_compute_cs_bytes);
 unsigned char *dest=cpu_address(q.memory,c.output_va,host_page);
 unsigned char *fau=cpu_address(q.memory,c.fau_va,32);
 if(!where||!dest||!fau)return false;
 /* Each root CALL slot is immutable. Reclaim only after all prior work is
  * verified; compare the exact old packet before modifying a reused body. */
 if(round<4) {
  for(unsigned i=0;i<x9_compute_cs_bytes;i++)if(where[i])return false;
 }else if(memcmp(where,x9_campaign_cs[round-4],x9_compute_cs_bytes)) {
  IOLog("X9 VECTOR: old body bytes rejected dispatch=%u slot=%u\n",round+1,slot);return false;
 }
 if(c.chain) {
  /* C was written and verified by dispatch74; read it directly on GPU.
   * There is intentionally no CPU copy/write/clean of C between dispatches. */
  if(round!=74||!vector_validate_output(q,73,false))return false;
 }else {
  for(unsigned input=0;input<2;input++) {
   uint32_t va=input?vector_b:vector_a;
   unsigned char *data=cpu_address(q.memory,va,host_page);
   if(!data)return false;
   memset(data,0xa5,host_page);
   for(unsigned i=0;i<vector_records;i++) {
    uint32_t record[]={vector_input(c.seed,i,input!=0),i,~i,c.seed};
    memcpy(data+i*16,record,16);
   }
   if(!vector_sync(q,gpu,round,va,host_page))return false;
  }
 }
 /* Reset only the completed job's destination, to a non-result sentinel. */
 memset(dest,0xa5,host_page);
 if(!vector_sync(q,gpu,round,c.output_va,host_page))return false;
 /* Mesa csf/panvk_vX_cmd_dispatch.c keeps hardware JOB_OFFSET0 because
  * it does not apply to WG_ID; add both bases in shader through FAU instead. */
 uint64_t pointers[]={c.a_va,c.b_va,c.output_va,
  (uint64_t)(c.offset_x*c.local_x)|((uint64_t)c.offset_x<<32)};memcpy(fau,pointers,sizeof(pointers));
 if(!vector_sync(q,gpu,round,shader_desc_va,gpu_page))return false;
 memcpy(where,x9_campaign_cs[round],x9_compute_cs_bytes);
 if(!vector_sync(q,gpu,round,body,gpu_page))return false;
 unsigned producer=(round+1)*queue_batch_bytes;uint32_t fence=0x53484400u+round+1;
 if(!put_qword(p,queue_user,producer)||!clean_page(p,queue_user))return false;
 gw(user_db,0,1);
 uint64_t end=deadline_ms(3000),extract=0;uint32_t seen=0;
 do {
  seen=output_word(q.memory,queue_result);extract=output_qword(p,queue_user+4096);
  if(!global_healthy(p,q,gpu)||
     ((output_word(p,q.stream_out)^input_word(p,q.stream_in))&0xc0000000u))break;
  if(seen==fence&&extract==producer) {
   if(output_word(q.memory,queue_result,4)||output_word(q.memory,queue_result,64)!=0xa5a5a5a5u||
      !vector_validate_output(q,round,true)||
      (c.chain&&!vector_validate_output(q,73,false)))break;
   q.completed=round+1;
   IOLog("X9 VECTOR: completion=%u fence=%08x extract=%llu verified; body-slot=%u reused=%u\n",
    q.completed,seen,extract,slot,round>=4);return true;
  }
  sleep_ms();
 }while(mach_absolute_time()<end);
 IOLog("X9 VECTOR: dispatch=%u FAILED fence=%08x/%08x extract=%llu/%u; campaign stops without retry\n",
  round+1,seen,fence,extract,producer);
 shader_diagnostics(p,q,gpu);queue_diagnostics(p,q);return false;
}
