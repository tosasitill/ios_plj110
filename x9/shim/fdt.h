#pragma once
#include <stdint.h>
#include <stddef.h>

/* Bounded /chosen property lookup. Accept one or two big-endian cells. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

int fdt_ram_available(uintptr_t, uint64_t, uint64_t);
enum { FDT_PROPERTY_NODE_MISSING=-2, FDT_PROPERTY_INVALID=-1,
       FDT_PROPERTY_ABSENT=0, FDT_PROPERTY_PRESENT=1 };
/* ABSENT is returned only for a unique node in a fully valid v17 tree. */
int fdt_property_lookup(uintptr_t,const char *,const char *,const uint8_t **,size_t *);
/* 1 unique property, 0 absent in a valid tree, -1 malformed/duplicate. */
int fdt_property_checked(uintptr_t,const char *,const char *,const uint8_t **,size_t *);
int fdt_property(uintptr_t,const char *,const char *,const uint8_t **,size_t *);

/* Bounded metadata from the last RAM check, for USB/pstore failure logs. */
const char *fdt_ram_failure(uint64_t *,uint64_t *,uint64_t *,uint64_t *);
int fdt_ram_memory_tuple(unsigned,uint64_t *,uint64_t *);

int fdt_ram_build_live(uintptr_t);
uint64_t fdt_ram_live_bytes(void);
int fdt_ram_live_tuple(unsigned,uint64_t *,uint64_t *);
int fdt_ram_patch_adt(void *,size_t);

int fdt_gpu_patch_adt(uintptr_t,void *,size_t);
/* 1 layout qualified, 0 FDT mismatch, -1 missing/invalid ADT placeholder.
 * All are nonfatal to boot. No hardware access or readiness implied. */
int fdt_scp_patch_adt(uintptr_t,void *,size_t);
int fdt_gps_patch_adt(uintptr_t,void *,size_t);
/* DMA topology only, not buffer ownership, AIN0 or complete write drain. */
int fdt_mic_dma_patch_adt(uintptr_t,void *,size_t);
int fdt_adsp_patch_adt(uintptr_t,void *,size_t);

/* Read-only Wi-Fi DT facts into /chosen/x9-lk-wifi-dt: 0 written, 1 no placeholder, -1 FDT unusable. */
int fdt_wifi_dump_adt(uintptr_t,void *,size_t);
