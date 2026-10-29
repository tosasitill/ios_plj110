#pragma once
#include <stdint.h>
#define X9_METAL_ABI 1u
#define X9_METAL_MAX 256u
#define X9_METAL_SERVICE "SweetX9Compute"
/* Fixed-size copied IOKit ABI, no user pointers or physical/MMIO addresses.
 * Selector0: capabilities, selector1: execute this sole built-in pipeline.
 * All arrays are copied once on entry. count must be divisible by local_x. */
struct X9MetalCaps { uint32_t abi,max_count,max_local_x,gpu_id; };
struct X9MetalRequest {
 uint32_t abi,count,local_x,reserved;
 uint32_t a[X9_METAL_MAX],b[X9_METAL_MAX];
};
struct X9MetalResponse {
 uint32_t abi,count,sequence,status;
 uint32_t c[X9_METAL_MAX];
};
#ifdef KERNEL
struct X9MetalJob {
 struct X9MetalRequest request;
 struct X9MetalResponse response;
 bool computed,cleaned;
};
#endif
