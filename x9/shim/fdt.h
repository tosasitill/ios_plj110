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
/* Strict node schema, using the SAME complete-tree parser. Reject unknown
 * or duplicate properties; count direct children. Keys are inline arrays
 * (no relocatable pointer table). Publish outputs only after a valid END.
 * Returns PRESENT for a unique node, NODE_MISSING/INVALID otherwise. */
struct fdt_node_property { const uint8_t *value; size_t bytes; };
int fdt_node_schema(uintptr_t,const char *,const char (*)[32],unsigned,
                    struct fdt_node_property *,uint32_t *,unsigned *);
/* Exactly one phandle/linux,phandle occurrence of this value, at this path.
 * Conservative: duplicate aliases also refuse a new hardware grant. */
int fdt_phandle_unique(uintptr_t,const char *,uint32_t);
/* Capture required nodes in ONE complete-tree pass. No persistent cache or
 * early-success shortcut: publish only after valid END/trailing padding.
 * Paths are unique inline strings (safe for relocated shim); all required
 * nodes must exist uniquely, every captured property key must be unique.
 * Records point into this caller-owned FDT and are consumed immediately,
 * never retained across mutations/another boot. Unknown properties allowed;
 * strict pinctrl schemas must still use fdt_node_schema, not this API. */
#define FDT_CAPTURE_PATH_BYTES 128u
#define FDT_CAPTURE_MAX_NODES 16u
#define FDT_CAPTURE_MAX_PROPERTIES 128u
#define FDT_CAPTURE_KEY_BYTES 128u /* includes NUL; fail, never truncate */
struct fdt_captured_property {
    uint32_t node,reserved;
    const char *key;
    const uint8_t *value;
    size_t bytes;
};
int fdt_capture_nodes(uintptr_t,const char (*)[FDT_CAPTURE_PATH_BYTES],unsigned,
                     struct fdt_captured_property *,unsigned,unsigned *);

/* Bounded multi-node collection (fdt_collect.c): the same complete-tree
 * validation as fdt_property_lookup, run ONCE to record every property and
 * the direct child count of up to 32 wanted nodes, plus ONCE more to check a
 * batch of phandles. For boot-critical consumers that need many fields
 * (audio); the per-field lookups above remain for small single-use callers.
 * The struct is ~6 KiB: callers keep it in static storage, not the stack.
 * Records are offsets into the caller's FDT, valid only for this call chain. */
#define FDT_COLLECT_MAX_NODES 32u
#define FDT_COLLECT_MAX_PROPERTIES 384u
#define FDT_COLLECT_MAX_HANDLES 24u
struct fdt_collected { uint16_t node,reserved; uint32_t key,value,bytes; };
struct fdt_collect {
    uintptr_t fdt;
    const char (*paths)[FDT_CAPTURE_PATH_BYTES];
    unsigned path_count,count,kicks,passes;
    uint32_t required,present,strings;
    uint64_t ticks;   /* CNT ticks spent in completed passes */
    uint16_t children[FDT_COLLECT_MAX_NODES];
    struct fdt_collected prop[FDT_COLLECT_MAX_PROPERTIES];
};
/* PRESENT: valid tree, every required node unique; c->present has the found
 * (optional) nodes. NODE_MISSING/INVALID publish nothing. */
int fdt_collect(struct fdt_collect *,uintptr_t,const char (*)[FDT_CAPTURE_PATH_BYTES],unsigned,uint32_t);
/* Second pass: *ok bit i set iff handles[i] occurs exactly once as a
 * phandle/linux,phandle, at collected node owners[i]. */
int fdt_collect_phandles(struct fdt_collect *,const uint32_t *,const uint8_t *,unsigned,uint32_t *);
/* 1 found, 0 absent from a collected node, -1 node not collected/present. */
int fdt_collect_get(const struct fdt_collect *,unsigned,const char *,const uint8_t **,size_t *);
/* fdt_node_schema semantics on a collected node (unknown key -> INVALID). */
int fdt_collect_schema(const struct fdt_collect *,unsigned,const char (*)[32],unsigned,
                       struct fdt_node_property *,uint32_t *,unsigned *);
/* All pre-XNU audio handoffs (ADSP layout/IRQ/I2S4/PA, microphone DMA) from
 * one collection. -1 ADT placeholders invalid, 0 nothing qualified, 1 some. */
int fdt_audio_patch_adt(uintptr_t,void *,size_t);

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

/* Read-only Wi-Fi DT facts into /chosen/x9-lk-wifi-dt: 0 written, 1 no placeholder, -1 FDT unusable. */
int fdt_wifi_dump_adt(uintptr_t,void *,size_t);
