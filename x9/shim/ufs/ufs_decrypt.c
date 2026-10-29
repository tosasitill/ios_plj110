#include "ufs_decrypt.h"
static void wipe(void *data,size_t size)
{volatile uint8_t *p=data;while(size--)*p++=0;}
int x9d_xts_init(struct x9d_xts *ctx,const uint8_t *key,size_t bytes)
{
 if(!ctx)return X9D_ARGUMENT;
 wipe(ctx,sizeof(*ctx));
 if(!key)return X9D_KEY_UNAVAILABLE;
 if(bytes!=64)return X9D_ARGUMENT;
 unsigned diff=0;for(unsigned i=0;i<32;i++)diff|=key[i]^key[i+32];
 if(!diff)return X9D_ARGUMENT;
 AES_init_ctx(&ctx->data,key);AES_init_ctx(&ctx->tweak,key+32);ctx->ready=1;return 0;
}
int x9d_xts_decrypt(struct x9d_xts *ctx,uint64_t dun,void *data,size_t bytes)
{
 if(!ctx||!data||bytes!=4096)return X9D_ARGUMENT;
 if(!ctx->ready)return X9D_KEY_UNAVAILABLE;
 uint8_t tweak[16]={0};for(unsigned i=0;i<8;i++)tweak[i]=(uint8_t)(dun>>(8*i));
 AES_ECB_encrypt(&ctx->tweak,tweak);
 uint8_t *p=data;
 for(size_t offset=0;offset<bytes;offset+=16) {
  for(unsigned i=0;i<16;i++)p[offset+i]^=tweak[i];
  AES_ECB_decrypt(&ctx->data,p+offset);
  for(unsigned i=0;i<16;i++)p[offset+i]^=tweak[i];
  unsigned carry=0;
  for(unsigned i=0;i<16;i++) {unsigned v=tweak[i];tweak[i]=(uint8_t)((v<<1)|carry);carry=v>>7;}
  if(carry)tweak[0]^=0x87;
 }
 wipe(tweak,sizeof(tweak));return 0;
}
int x9d_init(struct x9d_context *ctx,const uint8_t *metadata_key,const uint8_t *file_key,uint64_t iv_sector)
{
 if(!ctx)return X9D_ARGUMENT;
 wipe(ctx,sizeof(*ctx));
 if(iv_sector&7)return X9D_ARGUMENT;
 ctx->metadata_iv_sector=iv_sector;
 int a=x9d_xts_init(&ctx->metadata,metadata_key,64),b=x9d_xts_init(&ctx->file,file_key,64);
 return a?a:b;
}
int x9d_metadata(struct x9d_context *ctx,uint64_t block,void *data)
{
 if(!ctx||block>UINT64_MAX-ctx->metadata_iv_sector/8)return X9D_ARGUMENT;
 return x9d_xts_decrypt(&ctx->metadata,block+ctx->metadata_iv_sector/8,data,4096);
}
int x9d_file(struct x9d_context *ctx,uint32_t inode,uint64_t block,void *data)
{
 if(!ctx||!inode||block>UINT32_MAX)return X9D_ARGUMENT;
 return x9d_xts_decrypt(&ctx->file,((uint64_t)inode<<32)|block,data,4096);
}
void x9d_clear(struct x9d_context *ctx){if(ctx)wipe(ctx,sizeof(*ctx));}
