/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
/* Snapshot of the already cache-invalidated CSF shared firmware interface.
 * Offsets are checked against the mapped 64KiB shared image. This parser
 * performs no MMIO and does not claim ownership of a firmware group. */
#define X9_CSF_SHARED_BASE 0x04000000u
#define X9_CSF_SHARED_BYTES 0x10000u
#define X9_CSF_MAX_STREAMS 3u
struct X9CSFInterfaceRange { uint32_t va,bytes; };
struct X9CSFStreamInterface { uint32_t features,input,output; };
struct X9CSFGroupInterface {
 uint32_t global_version,global_input,global_output,group_index,group_input,group_output;
 uint32_t suspend_bytes,features,stream_count;
 struct X9CSFStreamInterface streams[X9_CSF_MAX_STREAMS];
};
bool x9_csf_parse_group(const void *snapshot,uint32_t bytes,uint32_t group_index,
 uint32_t streams,struct X9CSFGroupInterface *out);
