#pragma once
#include <stddef.h>
#include <stdint.h>
#include "crypto/aes.h"
#define X9D_KEY_UNAVAILABLE (-60)
#define X9D_ARGUMENT (-61)
#define X9D_UNSUPPORTED (-62)
/* Only 4096-byte AES-256-XTS data units; no writes, HKDF/master-key handling,
 * hardware-wrapped keys or CTS. Key is effective 64-byte XTS key K1||K2. */
struct x9d_xts {struct AES_ctx data,tweak;unsigned ready;};
struct x9d_context {struct x9d_xts metadata,file;uint64_t metadata_iv_sector;};
int x9d_xts_init(struct x9d_xts *,const uint8_t *key,size_t);
int x9d_xts_decrypt(struct x9d_xts *,uint64_t dun,void *data,size_t);
int x9d_init(struct x9d_context *,const uint8_t *metadata_key,const uint8_t *file_effective_key,uint64_t iv_sector);
int x9d_metadata(struct x9d_context *,uint64_t partition_block,void *);
/* Android v2 AES-256-XTS + IV_INO_LBLK_64: low32 logical block, high32 ino.
 * Contents bio bypasses dm-default-key: never apply metadata key here. */
int x9d_file(struct x9d_context *,uint32_t inode,uint64_t logical_block,void *);
void x9d_clear(struct x9d_context *);
