/* SPDX-License-Identifier: MIT */
#include "X9CSFInterfaces.h"
#include <libkern/libkern.h>
/* Offsets match the CSF 0.3 interface parsed in pan168 capabilities() and
 * queue_interfaces(), originally sourced from MT6993 a16w firmware headers. */
static uint32_t word(const uint8_t *p) {
 return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static bool region(uint32_t va,uint32_t bytes,uint32_t alignment) {
 return !(va&(alignment-1))&&va>=X9_CSF_SHARED_BASE&&
  va-X9_CSF_SHARED_BASE<X9_CSF_SHARED_BYTES&&bytes<=X9_CSF_SHARED_BYTES-(va-X9_CSF_SHARED_BASE);
}
static bool overlaps(const X9CSFInterfaceRange &a,const X9CSFInterfaceRange &b) {
 return a.va<(uint64_t)b.va+b.bytes&&b.va<(uint64_t)a.va+a.bytes;
}
bool x9_csf_parse_group(const void *snapshot,uint32_t bytes,uint32_t index,uint32_t count,X9CSFGroupInterface *out) {
 if(!snapshot||!out||bytes<X9_CSF_SHARED_BYTES||!count||count>X9_CSF_MAX_STREAMS)return false;
 const uint8_t *image=(const uint8_t *)snapshot;
 uint32_t version=word(image),groups=word(image+16),stride=word(image+20);
 if(!version||version==0xffffffffu||!groups||groups>16||index>=groups||stride<64||
    0x1000ULL+(uint64_t)groups*stride>X9_CSF_SHARED_BYTES)return false;
 const uint8_t *g=image+0x1000+(uint64_t)index*stride;
 uint32_t num=word(g+20),stream_stride=word(g+24),suspend=word(g+12);
 if(num<count||num>32||stream_stride<12||64ULL+(uint64_t)num*stream_stride>stride||
    !suspend||suspend>0x100000u)return false;
 X9CSFGroupInterface parsed={};
 parsed.global_version=version;parsed.global_input=word(image+8);parsed.global_output=word(image+12);
 parsed.group_index=index;parsed.group_input=word(g+4);parsed.group_output=word(g+8);
 parsed.suspend_bytes=suspend;parsed.features=word(g);parsed.stream_count=count;
 X9CSFInterfaceRange ranges[4+2*X9_CSF_MAX_STREAMS]={
  {parsed.global_input,4096},{parsed.global_output,4096},
  {parsed.group_input,0x70},{parsed.group_output,0x20}};
 if(!region(parsed.global_input,4096,4096)||!region(parsed.global_output,4096,4096)||
    !region(parsed.group_input,0x70,64)||!region(parsed.group_output,0x20,64))return false;
 for(unsigned i=0;i<count;i++) {
  const uint8_t *entry=g+64+(uint64_t)i*stream_stride;
  parsed.streams[i]={word(entry),word(entry+4),word(entry+8)};
  if(parsed.streams[i].features==0xffffffffu||!region(parsed.streams[i].input,0x60,64)||
     !region(parsed.streams[i].output,0xe0,64))return false;
  ranges[4+2*i]={parsed.streams[i].input,0x60};ranges[5+2*i]={parsed.streams[i].output,0xe0};
 }
 /* Separate control regions prevent a firmware-supplied alias from causing a
  * stream write to corrupt another stream, group or global interface. */
 for(unsigned i=0;i<4+2*count;i++)for(unsigned j=0;j<i;j++)if(overlaps(ranges[i],ranges[j]))return false;
 *out=parsed;return true;
}
