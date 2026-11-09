/* SPDX-License-Identifier: MIT */
#pragma once
#include "X9CSFInterfaces.h"
#include "X9RenderKernel.h"
/* Accessors operate on the retained AS0 firmware allocation, including cache
 * maintenance. All calls require the single GPU owner lock. now_ns uses a
 * monotonic clock; doorbell() rings the real kernel DB, not a synthetic ACK. */
struct X9CSFQueueIO {
 void *cookie;
 bool (*read32)(void *,uint32_t,uint32_t *);
 bool (*write32)(void *,uint32_t,uint32_t);
 bool (*clean)(void *,uint32_t,uint32_t);
 bool (*healthy)(void *);
 bool (*doorbell)(void *);
 uint64_t (*now_ns)(void *);
 void (*yield)(void *);
};
struct X9CSFStreamBinding { uint64_t ring,user_input,user_output;uint32_t ring_bytes; };
struct X9CSFRenderGroup {
 X9CSFGroupInterface interface;
 X9CSFStreamBinding streams[3];
 uint64_t firmware_epoch;
 uint32_t streams_touched;
 bool touched,running,suspended,uncertain;
};
/* reuse=true (x9cs slot steal): the slot's previous group was SUSPENDed (CSG
 * REQ=ACK=SUSPEND, its state saved elsewhere); program it like kbase's
 * program_csg_slot does, without TERMINATE (a suspended CSG never acks it). */
IOReturn x9_csf_group_configure(const X9CSFQueueIO *,X9CSFRenderGroup *,const X9CSFGroupInterface *,
 const X9RenderGroup *,const X9CSFStreamBinding [3],uint64_t suspend_va,uint64_t epoch,bool reuse=false);
IOReturn x9_csf_group_start(const X9CSFQueueIO *,X9CSFRenderGroup *,uint64_t epoch,uint64_t timeout_ns);
IOReturn x9_csf_group_halt(const X9CSFQueueIO *,X9CSFRenderGroup *,uint64_t epoch,bool suspend,uint64_t timeout_ns);
/* Checks firmware faults and tiler OOM, not rendering correctness or DMA idle. */
IOReturn x9_csf_group_health(const X9CSFQueueIO *,const X9CSFRenderGroup *,uint64_t epoch);
