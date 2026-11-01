/* Bounded staged presentation. Display workloop never takes the GPU provider
 * lock. Only completed, fully validated, generation-matched frames publish. */
extern "C" int sweet_x9_gpu_desktop(void *);
extern "C" kern_return_t thread_terminate(thread_t);
enum DesktopFrameState { FrameCold,FrameAllocating,FrameIdle,FrameBuilding,FrameRunning,FrameReady,FrameCPUReady,FrameDisabled };
struct DesktopFramePool {
 X9DesktopJob job;
 IOBufferMemoryDescriptor *input,*output;
 uint64_t generation,worker_ns;
};
static DesktopFrameState desktop_frame_state=FrameCold;
static DesktopFramePool *desktop_frame_pool;
static unsigned desktop_frame_attempts,desktop_frame_presented;
static bool desktop_frame_no_more;
static void desktop_frame_retire_clean_pool(void) {
 if(!desktop_frame_pool)return;
 desktop_frame_pool->input->release();desktop_frame_pool->output->release();
 IOFree(desktop_frame_pool,sizeof(*desktop_frame_pool));desktop_frame_pool=NULL;
 IOLog("X9 DESKTOP FRAME: clean private pool released;CPU display continues\n");
}
static void desktop_frame_thread_exit(void) {
 (void)thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);
}
static void desktop_frame_worker(void *argument,wait_result_t) {
 if(!argument) {
  DesktopFramePool *pool=(DesktopFramePool *)IOMallocZero(sizeof(DesktopFramePool));
  if(pool) {
   pool->input=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut,desktop_frame_bytes,host_page);
   pool->output=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut,desktop_frame_bytes,host_page);
  }
  bool valid=pool&&pool->input&&pool->output&&pool->input->getBytesNoCopy()&&pool->output->getBytesNoCopy();
  if(!valid&&pool) {
   if(pool->input)pool->input->release();if(pool->output)pool->output->release();IOFree(pool,sizeof(*pool));pool=NULL;
  }
  IOLockLock(desktop_frame_lock);
  desktop_frame_pool=pool;desktop_frame_state=valid?FrameIdle:FrameDisabled;
  IOLog("X9 DESKTOP FRAME: private pool %s bytes-per-buffer=%u;1256x2760 source-stride5120 output-stride5184; GPU panel mappings=0\n",
   valid?"ready":"allocation failed",desktop_frame_bytes);
  IOLockUnlock(desktop_frame_lock);
  desktop_frame_thread_exit();return;
 }
 DesktopFramePool *pool=(DesktopFramePool *)argument;
 IOLog("X9 DESKTOP FRAME: worker BEGIN sequence=%u generation=%llu;display thread continues\n",pool->job.sequence,pool->generation);
 uint64_t worker_started=mach_absolute_time();
 IOLockLock(metal_service_lock);
 int result=metal_service_disabled?0:sweet_x9_gpu_desktop(&pool->job);
 if(result<0)metal_service_disabled=true;
 IOLockUnlock(metal_service_lock);
 absolutetime_to_nanoseconds(mach_absolute_time()-worker_started,&pool->worker_ns);
 IOLockLock(desktop_frame_lock);
 if(result==1&&pool->job.computed&&pool->job.cleaned)desktop_frame_state=FrameReady;
 else {
  desktop_frame_no_more=true;
  desktop_frame_state=result<0?FrameDisabled:FrameCPUReady;
 }
 IOLog("X9 DESKTOP FRAME: worker sequence=%u result=%d computed=%u cleaned=%u generation=%llu;publication still gated\n",
  pool->job.sequence,result,pool->job.computed,pool->job.cleaned,pool->generation);
 IOLog("X9 DESKTOP FRAME: sequence=%u end-to-end-us=%llu;includes setup,dispatch,full-check,cleanup\n",pool->job.sequence,pool->worker_ns/1000);
 if(result<0)IOLog("X9 DESKTOP FRAME: private DMA buffers/PM refs retained;display CPU path continues;no retry/reset\n");
 IOLockUnlock(desktop_frame_lock);
 desktop_frame_thread_exit();
}
static bool desktop_frame_spawn(void *argument) {
 thread_t worker=THREAD_NULL;
 kern_return_t result=kernel_thread_start(desktop_frame_worker,argument,&worker);
 if(result==KERN_SUCCESS){thread_deallocate(worker);return true;}
 IOLog("X9 DESKTOP FRAME: worker creation failed=%08x\n",result);return false;
}
extern "C" uint8_t *sweet_x9_desktop_frame_begin(const uint8_t *panel,uint32_t stride,uint32_t width,uint32_t height,uint64_t generation) {
 if(!panel||stride!=5120||width!=1256||height!=2760||!desktop_frame_lock)return NULL;
 if(!IOLockTryLock(desktop_frame_lock))return NULL;
 if(desktop_frame_state!=FrameIdle||desktop_frame_no_more||desktop_frame_presented>=3||desktop_frame_attempts>=8) {
  IOLockUnlock(desktop_frame_lock);return NULL;
 }
 DesktopFramePool *pool=desktop_frame_pool;
 unsigned char *input=(unsigned char *)pool->input->getBytesNoCopy(),*output=(unsigned char *)pool->output->getBytesNoCopy();
 /* Display owns FrameBuilding. Preserve pixels not touched by this swap,
  * but keep every guard byte poisoned, including native row padding. */
 desktop_frame_state=FrameBuilding;pool->generation=generation;
 IOLockUnlock(desktop_frame_lock);
 memset(input,0xa5,desktop_frame_bytes);memset(output,0xa5,desktop_frame_bytes);
 for(unsigned y=0;y<height;y++)memcpy(input+desktop_frame_offset+(uint64_t)y*stride,panel+(uint64_t)y*stride,width*4);
 X9DesktopJob &j=pool->job; bzero(&j,sizeof(j));
 j.source=pool->input;j.destination=pool->output;j.source_cpu=input;j.destination_cpu=output;
 j.source_stride=desktop_frame_source_stride;j.destination_stride=desktop_frame_destination_stride;
 j.source_offset=j.destination_offset=desktop_frame_offset;j.width=width;j.height=height;j.sequence=++desktop_frame_attempts;
 IOLog("X9 DESKTOP FRAME: CPU composition redirected sequence=%u generation=%llu;previous panel retained\n",j.sequence,generation);
 return input+desktop_frame_offset;
}
extern "C" bool sweet_x9_desktop_frame_queue(uint8_t *pixels,uint64_t generation) {
 if(!desktop_frame_lock)return false;
 IOLockLock(desktop_frame_lock);
 DesktopFramePool *pool=desktop_frame_pool;
 if(!pool||desktop_frame_state!=FrameBuilding||pool->generation!=generation||pixels!=pool->job.source_cpu+desktop_frame_offset) {
  IOLockUnlock(desktop_frame_lock);return false;
 }
 desktop_frame_state=FrameRunning;
 IOLockUnlock(desktop_frame_lock);
 if(!desktop_frame_spawn(pool)) {
  IOLockLock(desktop_frame_lock);desktop_frame_state=FrameCPUReady;desktop_frame_no_more=true;IOLockUnlock(desktop_frame_lock);
 }
 return true;
}
extern "C" bool sweet_x9_desktop_frame_poll(uint8_t *panel,uint32_t stride,uint32_t width,uint32_t height,uint64_t generation,bool can_publish) {
 int mode=0;
 if(!panel||stride!=5120||width!=1256||height!=2760||!desktop_frame_lock||
    !PE_parse_boot_argn("x9_metal_system",&mode,sizeof(mode))||mode!=2)return false;
 /* Observe permission/validation state only through the provider lock. A busy
  * client skips this tick; it never parks the display on GPU progress. */
 if(!IOLockTryLock(metal_service_lock))return false;
 bool enabled=!metal_service_disabled&&sweet_x9_metal_payload_ready&&metal_root_passes>=4&&metal_mobile_passes>=4;
 IOLockUnlock(metal_service_lock);
 if(!IOLockTryLock(desktop_frame_lock))return false;
 if(desktop_frame_state==FrameCold&&enabled) {
  desktop_frame_state=FrameAllocating;IOLockUnlock(desktop_frame_lock);
  if(!desktop_frame_spawn(NULL)){IOLockLock(desktop_frame_lock);desktop_frame_state=FrameDisabled;IOLockUnlock(desktop_frame_lock);}
  return false;
 }
 if(desktop_frame_state!=FrameReady&&desktop_frame_state!=FrameCPUReady) {IOLockUnlock(desktop_frame_lock);return false;}
 DesktopFramePool *pool=desktop_frame_pool;
 if(pool->generation!=generation) {
  if(pool->worker_ns>100000000ULL) {
   desktop_frame_no_more=true;
   IOLog("X9 DESKTOP FRAME: missed100ms frame budget sequence=%u worker-us=%llu;staged experiment disabled for this boot,CPU display continues\n",pool->job.sequence,pool->worker_ns/1000);
  }
  IOLog("X9 DESKTOP FRAME: stale result sequence=%u generation=%llu current=%llu discarded;newer CPU frame kept\n",pool->job.sequence,pool->generation,generation);
  desktop_frame_state=desktop_frame_no_more||desktop_frame_attempts>=8?FrameDisabled:FrameIdle;
  if(desktop_frame_state==FrameDisabled)desktop_frame_retire_clean_pool();
  IOLockUnlock(desktop_frame_lock);return false;
 }
 if(!can_publish) {IOLockUnlock(desktop_frame_lock);return false;}
 bool GPU=desktop_frame_state==FrameReady;
 const uint8_t *source=GPU?pool->job.destination_cpu:pool->job.source_cpu;
 uint32_t source_stride=GPU?pool->job.destination_stride:pool->job.source_stride;
 for(unsigned y=0;y<height;y++)memcpy(panel+(uint64_t)y*stride,source+desktop_frame_offset+(uint64_t)y*source_stride,width*4);
 for(unsigned y=0;y<height;y++)if(memcmp(panel+(uint64_t)y*stride,source+desktop_frame_offset+(uint64_t)y*source_stride,width*4)) {
  IOLog("X9 DESKTOP FRAME: panel CPU readback mismatch row=%u;no LK submission\n",y);
  desktop_frame_state=FrameDisabled;desktop_frame_no_more=true;desktop_frame_retire_clean_pool();
  IOLockUnlock(desktop_frame_lock);return false;
 }
 IOLog("X9 DESKTOP FRAME: panel CPU readback PASS pixels=%u sequence=%u;GPU caches/AS/power already released\n",width*height,pool->job.sequence);
 if(GPU)desktop_frame_presented++;
 IOLog("X9 DESKTOP FRAME: %s publication sequence=%u generation=%llu pixels=%u gpu-presented=%u/3;ready for LK submit\n",
  GPU?"GPU-result":"CPU recovery",pool->job.sequence,generation,width*height,desktop_frame_presented);
 desktop_frame_state=desktop_frame_no_more||desktop_frame_presented>=3||desktop_frame_attempts>=8?FrameDisabled:FrameIdle;
 if(desktop_frame_state==FrameDisabled)desktop_frame_retire_clean_pool();
 IOLockUnlock(desktop_frame_lock);return true;
}
