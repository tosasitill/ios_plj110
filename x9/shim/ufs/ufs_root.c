#include "ufs_root.h"
#include "sha256.h"
static uint32_t be32(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
int x9u_root_read(struct x9u_host *host,unsigned lun,const struct x9u_partition *part,
                  const struct x9u_root_manifest *manifest,void *destination,size_t capacity,
                  void (*progress)(uint64_t))
{
 if(!host||!part||!manifest||!destination||lun>=3||lun!=manifest->lun||
    host->block_size[lun]!=4096||part->first!=manifest->partition_first||
    part->count!=manifest->partition_blocks||!manifest->bytes||
    (manifest->bytes&4095)||manifest->bytes>capacity)return X9U_BOUNDS;
 for(unsigned i=0;i<16;i++)if(part->guid[i]!=manifest->guid[i])return X9U_GEOMETRY;
 int error=x9u_extent_validate(manifest->extents,manifest->extent_count,manifest->bytes/4096,part->count);
 if(error)return error;
 uint8_t *out=destination;struct sha256 digest;sha256_init(&digest);
 for(uint64_t offset=0;offset<manifest->bytes;) {
  size_t take=(size_t)(manifest->bytes-offset);if(take>X9U_DATA_SIZE)take=X9U_DATA_SIZE;
  error=x9u_image_read(host,lun,part,manifest->extents,manifest->extent_count,
                       manifest->bytes/4096,offset,out+offset,take);
  if(error)return error;
  sha256_update(&digest,out+offset,take);offset+=take;
  if(progress)progress(offset);
 }
 uint8_t result[32];sha256_final(&digest,result);unsigned different=0;
 for(unsigned i=0;i<32;i++)different|=result[i]^manifest->sha256[i];
 if(different)return X9U_CRC;
 /* Full raw HFS+/HFSX image, no UDIF trailer or partition container. */
 if(manifest->bytes<4096||out[1024]!='H'||(out[1025]!='+'&&out[1025]!='X'))return X9U_GEOMETRY;
 uint32_t block=be32(out+1064),count=be32(out+1068);
 if(block<512||(block&(block-1))||!count||(uint64_t)block*count!=manifest->bytes)return X9U_GEOMETRY;
 return 0;
}
