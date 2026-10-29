#pragma once
#include <stdint.h>
struct x9u_smmu_state {
 uint32_t idr0,cr0,cr0ack,gbpa,strtab_config;
 uint64_t strtab_base,l1,ste0;
 unsigned sid,identity;
};
/* Verify only existing bypass. Never change global SMMU or a stream entry.
 * read_table64 must reject out-of-RAM, reserved or inaccessible addresses. */
int x9u_smmu_identity(void *,unsigned,struct x9u_smmu_state *,
                       uint32_t (*)(void *,unsigned),
                       int (*)(void *,uint64_t,uint64_t *));
