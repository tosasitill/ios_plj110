#pragma once
#include <stddef.h>
#include <stdint.h>
/* Read-only, serialized, bounded F2FS v1 subset. Reader addresses are 4KiB
 * partition-relative blocks. No keys, decryption, mounting or disk mutation.
 * Only a clean checkpoint is authoritative; no roll-forward recovery. */
enum x9f_error {
 X9F_NOT_F2FS=-40, X9F_CORRUPT=-41, X9F_UNSUPPORTED=-42,
 X9F_UNCLEAN=-43, X9F_IDENTITY=-44, X9F_NOT_PINNED=-45,
 X9F_EXTENT=-46, X9F_READ=-47
};
struct x9f_reader { void *context; int (*read)(void *,uint64_t,void *); uint64_t blocks; };
struct x9f_info {
 uint64_t blocks,checkpoint_version,size;
 uint32_t cp,nat,main,ssa,nat_segments,cp_payload,features;
 uint32_t selected_cp,checkpoint_flags,inode,node_block,checked_blocks;
 uint8_t uuid[16],inline_flags;
 unsigned super_copy,pinned;
};
int x9f_super(const struct x9f_reader *,struct x9f_info *);
/* Verify known inode and every block of a small contiguous pinned file.
 * Supports inode addresses plus two direct nodes (not indirect/compression).
 * Does not resolve pathname or prove lifetime against inode reuse. */
int x9f_check_pin(const struct x9f_reader *,struct x9f_info *,uint32_t inode,
                  uint64_t expected_bytes,uint32_t expected_first);
