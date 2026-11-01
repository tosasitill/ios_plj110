#include "zlib_resource.h"
static uint32_t le32(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t be32(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void copy(void *d,const void *s,size_t n)
{uint8_t *a=d;const uint8_t *b=s;while(n--)*a++=*b++;}
static int read_at(const struct x9dc_io *io,uint64_t offset,void *out,size_t size)
{
 if(offset>io->resource_size||size>io->resource_size-offset)return X9DC_FORMAT;
 return io->read(io->context,offset,out,size)?X9DC_IO:0;
}
int x9dc_decode(const struct x9dc_io *io,const uint8_t *input,size_t length,uint8_t *out,size_t expected)
{
 if(!io||!io->inflate||!input||!out||!length||length>X9DC_INPUT||!expected||expected>X9DC_BLOCK)return X9DC_FORMAT;
 if((input[0]&15)==15) {
  if(length-1!=expected)return X9DC_FORMAT;
  copy(out,input+1,expected);return 0;
 }
 if(length<2||input[0]!=0x78||(((unsigned)input[0]<<8|input[1])%31)||input[1]&0x20)return X9DC_FORMAT;
 return io->inflate(io->context,input,length,out,expected)?X9DC_INFLATE:0;
}
int x9dc_open(const struct x9dc_io *io,uint64_t bytes,struct x9dc_resource *out)
{
 if(!io||!io->read||!io->inflate||!out||!bytes||bytes>(64ULL<<30))return X9DC_FORMAT;
 uint8_t header[16],prefix[8];int error=read_at(io,0,header,sizeof(header));if(error)return error;
 uint64_t data=be32(header),map=be32(header+4),length=be32(header+8),map_bytes=be32(header+12);
 if(data<16||length<8||data>io->resource_size||length>io->resource_size-data||
    map<data+length||map>io->resource_size||map_bytes>io->resource_size-map)return X9DC_FORMAT;
 error=read_at(io,data,prefix,sizeof(prefix));if(error)return error;
 uint32_t record=be32(prefix),blocks=le32(prefix+4);
 uint64_t expected=(bytes+X9DC_BLOCK-1)/X9DC_BLOCK;
 if(record!=length-4||blocks!=expected||!blocks||4+(uint64_t)blocks*8>record)return X9DC_FORMAT;
 out->base=data+4;out->length=record;out->file_size=bytes;out->blocks=blocks;return 0;
}
static int entry(const struct x9dc_io *io,const struct x9dc_resource *r,uint32_t index,uint32_t *offset,uint32_t *length)
{
 if(index>=r->blocks)return X9DC_FORMAT;
 uint8_t raw[8];int error=read_at(io,r->base+4+(uint64_t)index*8,raw,sizeof(raw));if(error)return error;
 *offset=le32(raw);*length=le32(raw+4);
 uint64_t table=4+(uint64_t)r->blocks*8;
 if(*offset<table||!*length||*length>X9DC_INPUT||*offset>r->length||*length>r->length-*offset)return X9DC_FORMAT;
 return 0;
}
int x9dc_read(const struct x9dc_io *io,const struct x9dc_resource *r,uint64_t offset,void *destination,size_t length,struct x9dc_workspace *w)
{
 if(!io||!r||!w||(!destination&&length)||offset>r->file_size||length>r->file_size-offset)return X9DC_RANGE;
 if(r->base>io->resource_size||r->length>io->resource_size-r->base)return X9DC_FORMAT;
 uint8_t *out=destination;
 while(length) {
  uint32_t index=(uint32_t)(offset/X9DC_BLOCK),start,n;
  int error=entry(io,r,index,&start,&n);if(error)return error;
  /* Check adjacent entries: table aliasing/overlap cannot redirect a block. */
  if(index) {
   uint32_t prior,bytes;error=entry(io,r,index-1,&prior,&bytes);if(error)return error;
   if((uint64_t)prior+bytes>start)return X9DC_FORMAT;
  }
  if(index+1<r->blocks) {
   uint32_t next,bytes;error=entry(io,r,index+1,&next,&bytes);if(error)return error;
   if((uint64_t)start+n>next)return X9DC_FORMAT;
  }
  error=read_at(io,r->base+start,w->input,n);if(error)return error;
  uint64_t base=(uint64_t)index*X9DC_BLOCK,remaining=r->file_size-base;
  size_t expected=remaining<X9DC_BLOCK?(size_t)remaining:X9DC_BLOCK;
  error=x9dc_decode(io,w->input,n,w->output,expected);if(error)return error;
  size_t skip=(size_t)(offset%X9DC_BLOCK),take=expected-skip;if(take>length)take=length;
  copy(out,w->output+skip,take);out+=take;offset+=take;length-=take;
 }
 return 0;
}
