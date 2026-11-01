#pragma once
#include "zlib_resource.h"
#define X9D_CODEC_SCRATCH 65536U
struct x9d_resource {struct x9dc_resource zlib;uint64_t size,bytes;uint32_t blocks,type;};
struct x9d_workspace {struct x9dc_workspace data;uint64_t aligned;uint8_t codec[X9D_CODEC_SCRATCH];};
int x9d_supported(unsigned);
int x9d_inline(unsigned);
int x9d_codec_decode(unsigned,const uint8_t *,size_t,uint8_t *,size_t,void *);
int x9d_decode(const struct x9dc_io *,unsigned,const uint8_t *,size_t,uint8_t *,size_t,void *);
int x9d_open(const struct x9dc_io *,unsigned,uint64_t,struct x9d_resource *);
int x9d_read(const struct x9dc_io *,const struct x9d_resource *,uint64_t,void *,size_t,struct x9d_workspace *);
