/* Bounded UFS plaintext-image -> RAM md0 handoff. No storage writes. */
#include "../loader.h"
#include "../resident.h"
#include "../fdt.h"
#include "../plog.h"
#include "../hw.h"
extern char _head[],_image_end[];
uint64_t x9_ufs_root_bytes(void);
void x9_ufs_root_destination(void *,size_t);
int x9_ufs_probe(uintptr_t);
int x9_ufs_dma_pending(void);
int x9_ufs_root_verified(void);
static int overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{return a>b? a-b<m : b-a<n;}
const char *x9_ufs_root_boot(uintptr_t dtb,const void *payload,size_t size)
{
 uint64_t base,capacity,adt,args;
 const char *error=payload_validate(payload,size,&adt,&args);if(error)return error;
 error=resident_layout_ufs(payload,size,&base,&capacity);if(error)return error;
 uint64_t bytes=x9_ufs_root_bytes();const struct payload *p=payload;
 uint64_t tail=p->adt_size+0x8000UL;
 if(!bytes||(bytes&0x3fff)||bytes>capacity||bytes>UINT64_MAX-tail)return "UFS root does not fit resident RAM";
 uint64_t region=bytes+tail;
 if(!fdt_ram_available(dtb,base,region)||
    overlap(base,region,(uintptr_t)_head,(size_t)(_image_end-_head))||
    overlap(base,region,(uintptr_t)payload,size)||
    overlap(base,region,dtb,0x400000UL))return "UFS root destination overlaps active/reserved memory";
 plog_hex("UFS ROOT RAM destination",base);plog_hex("UFS ROOT bytes",bytes);plog_flush();
 x9_ufs_root_destination((void *)(uintptr_t)base,(size_t)capacity);
 int result=x9_ufs_probe(dtb);
 plog_hex("UFS ROOT load status",(uint64_t)(int64_t)result);plog_flush();
 if(x9_ufs_dma_pending())return "UFS root DMA pending; handoff prohibited";
 if(result||!x9_ufs_root_verified())return "UFS plaintext root not verified; handoff prohibited";
 plog_str("UFS ROOT: verified plaintext image -> RAM md0; no Android decryption\n");plog_flush();
 return loader_boot_resident(dtb,payload,size,base,bytes);
}

#if X9_UFS_NATIVE
const char *x9_ufs_native_boot(uintptr_t dtb,const void *payload,size_t size)
{
 uint64_t adt,args;const char *error=payload_validate(payload,size,&adt,&args);if(error)return error;
 x9_ufs_root_destination(0,0);
 plog_str("UFS NATIVE: no full-image RAM load; initialize host and verify root header\n");plog_flush();
 int result=x9_ufs_probe(dtb);
 plog_hex("UFS NATIVE handoff status",(uint64_t)(int64_t)result);plog_flush();
 if(result||x9_ufs_dma_pending()||!x9_ufs_root_verified())return "native UFS root not verified/quiescent";
 plog_str("UFS NATIVE HANDOFF READY: kernel will mount ufsroot0 on demand\n");plog_flush();
 return loader_boot(dtb,payload,size);
}
#endif
