/* Read-only decmpfs types 3/4 and 7-14. Legacy zlib envelope and modern
 * little-endian contiguous offset tables are deliberately parsed separately. */
#include "all_resource.h"
static uint32_t le32(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void copy(void *d,const void *s,size_t n)
{uint8_t *a=d;const uint8_t *b=s;while(n--)*a++=*b++;}
static int read_at(const struct x9dc_io *io,uint64_t offset,void *out,size_t n)
{
 if(!io||!io->read||offset>io->resource_size||n>io->resource_size-offset)return X9DC_FORMAT;
 return io->read(io->context,offset,out,n)?X9DC_IO:0;
}
int x9d_supported(unsigned type)
{return type==3||type==4||(type>=7&&type<=14);}
int x9d_inline(unsigned type)
{return x9d_supported(type)&&(type&1);}
int x9d_decode(const struct x9dc_io *io,unsigned type,const uint8_t *src,size_t n,uint8_t *dst,size_t expected,void *scratch)
{
 if(!x9d_supported(type)||!src||!dst||!n||n>X9DC_INPUT||expected>X9DC_BLOCK)return X9DC_FORMAT;
 if(type==3||type==4)return x9dc_decode(io,src,n,dst,expected);
 int raw=type==9||type==10;
 if(type==9&&src[0]!=0xcc)return X9DC_FORMAT;
 if((type==7||type==8)&&src[0]==6)raw=1;
 if((type==13||type==14)&&src[0]==0xff)raw=1;
 if(raw) {
  if(n-1!=expected)return X9DC_FORMAT;
  copy(dst,src+1,expected);return 0;
 }
 return x9d_codec_decode(type,src,n,dst,expected,scratch);
}
int x9d_open(const struct x9dc_io *io,unsigned type,uint64_t bytes,struct x9d_resource *r)
{
 if(!io||!r||!x9d_supported(type)||x9d_inline(type)||!bytes||bytes>(64ULL<<30))return X9DC_FORMAT;
 *r=(struct x9d_resource){.size=io->resource_size,.bytes=bytes,.type=type};
 if(type==4)return x9dc_open(io,bytes,&r->zlib);
 uint64_t blocks=(bytes+X9DC_BLOCK-1)/X9DC_BLOCK,table=(blocks+1)*4;
 if(table>UINT32_MAX||io->resource_size>UINT32_MAX||table>io->resource_size)return X9DC_FORMAT;
 uint8_t first[4],last[4];int e=read_at(io,0,first,4);if(e)return e;
 e=read_at(io,blocks*4,last,4);if(e)return e;
 if(le32(first)!=table||le32(last)!=io->resource_size)return X9DC_FORMAT;
 r->blocks=(uint32_t)blocks;return 0;
}
int x9d_read(const struct x9dc_io *io,const struct x9d_resource *r,uint64_t offset,void *destination,size_t length,struct x9d_workspace *w)
{
 if(!io||!r||!w||(!destination&&length)||offset>r->bytes||length>r->bytes-offset||r->size!=io->resource_size)return X9DC_RANGE;
 if(r->type==4)return x9dc_read(io,&r->zlib,offset,destination,length,&w->data);
 if(!x9d_supported(r->type)||x9d_inline(r->type)||!r->blocks)return X9DC_FORMAT;
 uint8_t *dst=destination;
 while(length) {
  uint32_t index=(uint32_t)(offset/X9DC_BLOCK);uint8_t pair[8];
  if(index>=r->blocks)return X9DC_RANGE;
  int e=read_at(io,(uint64_t)index*4,pair,sizeof(pair));if(e)return e;
  uint32_t start=le32(pair),end=le32(pair+4);uint64_t table=((uint64_t)r->blocks+1)*4;
  if(start<table||end<=start||end>r->size||end-start>X9DC_INPUT)return X9DC_FORMAT;
  uint64_t base=(uint64_t)index*X9DC_BLOCK;size_t expected=(size_t)(r->bytes-base);
  if(expected>X9DC_BLOCK)expected=X9DC_BLOCK;
  e=read_at(io,start,w->data.input,end-start);if(e)return e;
  e=x9d_decode(io,r->type,w->data.input,end-start,w->data.output,expected,w->codec);if(e)return e;
  size_t skip=(size_t)(offset%X9DC_BLOCK),take=expected-skip;if(take>length)take=length;
  copy(dst,w->data.output+skip,take);dst+=take;offset+=take;length-=take;
 }
 return 0;
}
