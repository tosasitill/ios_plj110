/* Read-only decmpfs zlib format parser. No vnode, allocation or media writes.
 * Format: BE resource envelope, length-prefixed LE block table, 64KiB units.
 * Independent implementation; bounded against supplied resource/file sizes.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define X9DC_BLOCK 65536U
#define X9DC_INPUT (X9DC_BLOCK+1U)
enum {X9DC_FORMAT=-1,X9DC_IO=-2,X9DC_INFLATE=-3,X9DC_RANGE=-4};
struct x9dc_io {
 void *context;
 uint64_t resource_size;
 int (*read)(void *,uint64_t,void *,size_t);
 /* Must reach a valid zlib stream end and produce exactly expected bytes.
  * Actual Apple type4 entries include non-stream tail bytes: table bounds,
  * not zlib total_in, determine the next block. Never decode tails as data. */
 int (*inflate)(void *,const uint8_t *,size_t,uint8_t *,size_t);
};
struct x9dc_resource {uint64_t base,length,file_size;uint32_t blocks;};
struct x9dc_workspace {uint8_t input[X9DC_INPUT],output[X9DC_BLOCK];};
int x9dc_open(const struct x9dc_io *,uint64_t,struct x9dc_resource *);
int x9dc_read(const struct x9dc_io *,const struct x9dc_resource *,uint64_t,void *,size_t,struct x9dc_workspace *);
int x9dc_decode(const struct x9dc_io *,const uint8_t *,size_t,uint8_t *,size_t);
