/* X9 built-in decmpfs readers, types 3/4 and 7-14.
 * Resource bytes are read through VFS in decmpfs_ctx; no file mutation,
 * compressed-flag clearing, binary edits, resigning or block-device writes.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) && FS_COMPRESSION
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/vnode_internal.h>
#include <sys/uio_internal.h>
#include <sys/xattr.h>
#include <sys/decmpfs.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <libkern/zlib.h>
#include <arm64/sweet_early.h>
#include "x9_decmpfs/all_resource.h"
struct context {vnode_t vnode;size_t resource_size;int error;};
struct allocation {size_t bytes;uint64_t pad;};
static voidpf allocate(voidpf ignored,unsigned items,unsigned bytes)
{
 (void)ignored;
 if(!items||!bytes||(size_t)items>SIZE_MAX/bytes)return Z_NULL;
 size_t n=(size_t)items*bytes;
 if(n>(2U<<20)||n>SIZE_MAX-sizeof(struct allocation))return Z_NULL;
 struct allocation *p=kalloc_data(sizeof(*p)+n,Z_WAITOK|Z_ZERO);
 if(!p)return Z_NULL;p->bytes=n;return p+1;
}
static void release(voidpf ignored,voidpf memory)
{
 (void)ignored;if(!memory)return;
 struct allocation *p=(struct allocation *)memory-1;kfree_data(p,sizeof(*p)+p->bytes);
}
static int inflate_block(void *ignored,const uint8_t *input,size_t n,uint8_t *output,size_t expected)
{
 (void)ignored;
 z_stream stream={0};stream.zalloc=allocate;stream.zfree=release;
 stream.next_in=(Bytef *)(uintptr_t)input;stream.avail_in=(uInt)n;
 stream.next_out=output;stream.avail_out=(uInt)expected;
 int result=inflateInit(&stream);if(result!=Z_OK)return X9DC_INFLATE;
 result=inflate(&stream,Z_FINISH);
 int ok=result==Z_STREAM_END&&stream.total_out==expected;
 int ended=inflateEnd(&stream);
 return ok&&ended==Z_OK?0:X9DC_INFLATE;
}
static int read_resource(void *opaque,uint64_t offset,void *buffer,size_t length)
{
 struct context *c=opaque;
 if(offset>c->resource_size||length>c->resource_size-offset){c->error=EINVAL;return -1;}
 UIO_STACKBUF(storage,1);
 uio_t uio=uio_createwithbuffer(1,(off_t)offset,UIO_SYSSPACE,UIO_READ,storage,sizeof(storage));
 if(!uio){c->error=ENOMEM;return -1;}
 int error=uio_addiov(uio,CAST_USER_ADDR_T(buffer),length);
 size_t reported=0;
 if(!error)error=vn_getxattr(c->vnode,XATTR_RESOURCEFORK_NAME,uio,&reported,XATTR_NOSECURITY|XATTR_SHOWCOMPRESSION,decmpfs_ctx);
 if(!error&&uio_resid(uio))error=EIO;
 uio_free(uio);c->error=error;return error?-1:0;
}
static int setup(vnode_t vnode,decmpfs_header *header,struct context *c,struct x9dc_io *io,struct x9d_resource *r)
{
 if(header->compression_magic!=DECMPFS_MAGIC||header->attr_size<sizeof(decmpfs_disk_header))return EINVAL;
 *c=(struct context){.vnode=vnode};
 *io=(struct x9dc_io){.context=c,.read=read_resource,.inflate=inflate_block};
 if(x9d_inline(header->compression_type)) {
  uint64_t bytes=decmpfs_get_uncompressed_size(header);
  size_t compressed=header->attr_size-sizeof(decmpfs_disk_header);
  return bytes<=X9DC_BLOCK&&compressed&&compressed<=X9DC_INPUT?0:EINVAL;
 }
 if(!x9d_supported(header->compression_type))return ENOTSUP;
 int error=vn_getxattr(vnode,XATTR_RESOURCEFORK_NAME,NULL,&c->resource_size,XATTR_NOSECURITY|XATTR_SHOWCOMPRESSION,decmpfs_ctx);
 if(error)return error;
 io->resource_size=c->resource_size;
 int result=x9d_open(io,header->compression_type,decmpfs_get_uncompressed_size(header),r);
 return result?(c->error?c->error:EINVAL):0;
}
static int validate_file(vnode_t vnode,vfs_context_t ignored,decmpfs_header *header)
{
 (void)ignored;struct context context;struct x9dc_io io;struct x9d_resource resource;
 return setup(vnode,header,&context,&io,&resource);
}
static int fetch_file(vnode_t vnode,vfs_context_t ignored,decmpfs_header *header,off_t offset,
                     user_ssize_t requested,int vectors,decmpfs_vector *vec,uint64_t *bytes_read)
{
 (void)ignored;
 if(bytes_read)*bytes_read=0;
 if(offset<0||requested<0||vectors<0||vectors>1024||(!vec&&vectors))return EINVAL;
 uint64_t size=decmpfs_get_uncompressed_size(header);
 if((uint64_t)offset>size)return EINVAL;
 uint64_t want=(uint64_t)requested;if(want>size-(uint64_t)offset)want=size-(uint64_t)offset;
 uint64_t space=0;
 for(int i=0;i<vectors;i++) {
  if(vec[i].size<0||(!vec[i].buf&&vec[i].size)||(uint64_t)vec[i].size>UINT64_MAX-space)return EINVAL;
  space+=(uint64_t)vec[i].size;
 }
 if(space<want)return EINVAL;
 if(!want)return 0;
 struct context context;struct x9dc_io io;struct x9d_resource resource;
 int error=setup(vnode,header,&context,&io,&resource);if(error)return error;
 struct x9d_workspace *work=kalloc_data(sizeof(*work),Z_WAITOK);
 if(!work)return ENOMEM;
 if(x9d_inline(header->compression_type)) {
  int result=x9d_decode(&io,header->compression_type,header->attr_bytes,header->attr_size-sizeof(decmpfs_disk_header),work->data.output,(size_t)size,work->codec);
  if(result){error=EIO;goto done;}
 }
 uint64_t copied=0;
 for(int i=0;i<vectors&&copied<want;i++) {
  size_t n=(size_t)vec[i].size;if(n>want-copied)n=(size_t)(want-copied);
  if(x9d_inline(header->compression_type))memcpy(vec[i].buf,work->data.output+(uint64_t)offset+copied,n);
  else {
   int result=x9d_read(&io,&resource,(uint64_t)offset+copied,vec[i].buf,n,work);
   if(result){error=context.error?context.error:EIO;goto done;}
  }
  copied+=n;
 }
 if(bytes_read)*bytes_read=copied;
 done:
 if(error)printf("X9 decmpfs decode failed type=%u offset=%lld size=%lld errno=%d\n",header->compression_type,offset,requested,error);
 kfree_data(work,sizeof(*work));return error;
}
static decmpfs_registration codec_registration={
 .decmpfs_registration=DECMPFS_REGISTRATION_VERSION,
 .validate=validate_file,.adjust_fetch=NULL,.fetch=fetch_file,.free_data=NULL,.get_flags=NULL
};
void sweet_x9_decmpfs_init(void)
{
 if(!sweet_early_is_x9())return;
 static const unsigned types[]={3,4,7,8,9,10,11,12,13,14};
 for(unsigned i=0;i<sizeof(types)/sizeof(types[0]);i++) {
  int error=register_decmpfs_decompressor(types[i],&codec_registration);
  if(error)panic("X9 decmpfs registration failed type=%u error=%d",types[i],error);
 }
 printf("X9 decmpfs: types3/4 zlib,7/8 LZVN,9/10 raw,11/12 LZFSE,13/14 LZBITMAP registered (read-only)\n");
}
#endif
