#pragma once
#include <stdint.h>
#ifdef KERNEL
#include <IOKit/IOMemoryDescriptor.h>
/* Only private IOBuffer pages are GPU-mapped. Panel publication is CPU-side
 * after full result validation, cleanup and matching frame generation. */
struct X9DesktopJob {
 IOMemoryDescriptor *source,*destination;
 const uint8_t *source_cpu;uint8_t *destination_cpu;
 uint64_t source_offset,destination_offset;
 uint32_t source_stride,destination_stride,width,height,sequence;
 bool computed,cleaned;
};
extern "C" uint8_t *sweet_x9_desktop_frame_begin(const uint8_t *,uint32_t,uint32_t,uint32_t,uint64_t);
extern "C" bool sweet_x9_desktop_frame_queue(uint8_t *,uint64_t);
extern "C" bool sweet_x9_desktop_frame_poll(uint8_t *,uint32_t,uint32_t,uint32_t,uint64_t,bool);
#endif
