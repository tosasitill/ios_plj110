#pragma once
#include "ufs_ro.h"
struct x9u_root_manifest {
 uint64_t bytes,partition_first,partition_blocks;
 unsigned lun;
 uint8_t guid[16],sha256[32];
 const struct x9u_extent *extents;
 size_t extent_count;
};
/* Only reads. Caller owns a validated non-overlapping destination RAM range.
 * Full digest + HFS geometry must match before publishing RAMDisk to XNU. */
int x9u_root_read(struct x9u_host *,unsigned,const struct x9u_partition *,
                  const struct x9u_root_manifest *,void *,size_t,
                  void (*progress)(uint64_t));
