/* F2FS on-disk offsets: Linux v6.12 include/linux/f2fs_fs.h.
 * Static scratch: serialized shim use only. No allocations or packed loads.
 * All reads bounded to supplied partition; no filesystem writes or recovery.
 */
#include "f2fs_ro.h"
#define MAGIC 0xf2f52010U
static uint8_t sb[4096],cp[4096],tmp[4096],inode_page[4096],node[4096],journal[4096];
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint16_t le16(const uint8_t *p){return (uint16_t)p[0]|(uint16_t)p[1]<<8;}
static uint64_t le64(const uint8_t *p){return le32(p)|(uint64_t)le32(p+4)<<32;}
static void copy(uint8_t *d,const uint8_t *s,size_t n){while(n--)*d++=*s++;}
/* F2FS crc32_le starts at MAGIC, with no initial/final inversion. */
static uint32_t crc(uint32_t c,const uint8_t *p,size_t n)
{while(n--){c^=*p++;for(unsigned j=0;j<8;j++)c=(c>>1)^(0xedb88320U&-(c&1));}return c;}
static int read_block(const struct x9f_reader *r,uint64_t n,void *out)
{if(n>=r->blocks)return X9F_CORRUPT;return r->read(r->context,n,out)?X9F_READ:0;}
static int super_copy(const struct x9f_reader *r,unsigned block,struct x9f_info *f)
{
 int e=read_block(r,block,sb);if(e)return e;const uint8_t *s=sb+1024;
 if(le32(s)!=MAGIC)return X9F_NOT_F2FS;
 if(le16(s+4)!=1||le32(s+16)!=12||le32(s+20)!=9||
    le32(s+8)<9||le32(s+8)>12||le32(s+8)+le32(s+12)!=12)return X9F_UNSUPPORTED;
 f->blocks=le64(s+36);f->cp=le32(s+76);f->nat=le32(s+84);
 f->ssa=le32(s+88);f->main=le32(s+92);f->nat_segments=le32(s+60);
 f->cp_payload=le32(s+1664);f->features=le32(s+2180);
 copy(f->uuid,s+108,16);f->super_copy=block;
 if(f->blocks>r->blocks||!f->blocks||f->cp<2||
    (uint64_t)f->cp+1024>le32(s+80)||le32(s+80)>=f->nat||
    f->nat>=f->ssa||f->ssa>=f->main||f->main>=f->blocks||
    !f->nat_segments||(f->nat_segments&1)||
    (uint64_t)f->nat+f->nat_segments*512UL>f->ssa||f->cp_payload>510)return X9F_CORRUPT;
 /* Reject zoned geometry, inode checksums and multiple-device volumes.
  * Files with compression/verity are rejected individually below. */
 if(f->features&(0x2U|0x20U))return X9F_UNSUPPORTED;
 for(unsigned i=1;i<8;i++)for(unsigned j=0;j<64;j++)
  if(s[2201+i*68+j])return X9F_UNSUPPORTED;
 if(f->features&0x800U) {
  if(le32(s+32)!=3068||crc(MAGIC,s,3068)!=le32(s+3068))return X9F_CORRUPT;
 }
 return 0;
}
int x9f_super(const struct x9f_reader *r,struct x9f_info *f)
{
 if(!r||!r->read||!f||r->blocks<2)return X9F_CORRUPT;
 *f=(struct x9f_info){0};
 int first=super_copy(r,0,f);if(!first)return 0;
 int second=super_copy(r,1,f);return second==X9F_NOT_F2FS?first:second;
}
static int valid_cp(const uint8_t *p)
{
 uint32_t off=le32(p+164);
 if(off<192||off>4092)return 0;
 uint32_t c=crc(MAGIC,p,off);
 if(off<4092)c=crc(c,p+off+4,4092-off);
 return c==le32(p+off);
}
static int checkpoint(const struct x9f_reader *r,struct x9f_info *f,uint32_t start,uint64_t *ver)
{
 int e=read_block(r,start,tmp);if(e)return e;
 if(!valid_cp(tmp))return X9F_CORRUPT;
 uint32_t total=le32(tmp+136),sum=le32(tmp+140);
 if(total<2||total>512||sum<1+f->cp_payload||sum>=total-1)return X9F_CORRUPT;
 *ver=le64(tmp);
 e=read_block(r,(uint64_t)start+total-1,sb);if(e)return e;
 if(!valid_cp(sb)||le64(sb)!=*ver||le32(sb+136)!=total)return X9F_CORRUPT;
 return 0;
}
static int select_checkpoint(const struct x9f_reader *r,struct x9f_info *f)
{
 uint64_t va=0,vb=0;int a=checkpoint(r,f,f->cp,&va),b=checkpoint(r,f,f->cp+512,&vb);
 if(a&&b)return X9F_CORRUPT;
 f->selected_cp=(!b&&(a||(int64_t)(vb-va)>0))?f->cp+512:f->cp;
 int e=read_block(r,f->selected_cp,cp);if(e)return e;
 f->checkpoint_version=le64(cp);f->checkpoint_flags=le32(cp+132);
 if(!(f->checkpoint_flags&1)||f->checkpoint_flags&(0x8U|0x10U|0x1000U|0x4000U))return X9F_UNCLEAN;
 uint32_t sum=le32(cp+140),total=le32(cp+136);
 /* A clean unmount has 3 data + 3 node summaries. Compact data summary's
  * NAT journal is at byte zero; normal hot-data journal starts at 3584. */
 if(!(f->checkpoint_flags&4)&&total-sum<7)return X9F_CORRUPT;
 e=read_block(r,(uint64_t)f->selected_cp+sum,journal);if(e)return e;
 unsigned offset=(f->checkpoint_flags&4)?0:3584;
 uint16_t count=le16(journal+offset);if(count>38)return X9F_CORRUPT;
 if(offset)for(unsigned i=0;i<507;i++)journal[i]=journal[offset+i];
 /* Duplicate NAT journal keys cannot choose an authoritative mapping. */
 for(unsigned i=0;i<count;i++)for(unsigned j=0;j<i;j++)
  if(le32(journal+2+i*13)==le32(journal+2+j*13))return X9F_CORRUPT;
 return 0;
}
static int nat_lookup(const struct x9f_reader *r,const struct x9f_info *f,uint32_t nid,uint32_t owner,uint32_t *block)
{
 const uint8_t *entry=0;
 for(unsigned i=0;i<le16(journal);i++)if(le32(journal+2+i*13)==nid)entry=journal+6+i*13;
 if(!entry) {
  uint64_t index=nid/455U,total=f->nat_segments*256UL;
  if(!nid||index>=total)return X9F_CORRUPT;
  uint32_t sitbytes=le32(cp+156),natbytes=le32(cp+160),flags=f->checkpoint_flags;
  if(natbytes!=total/8||!natbytes)return X9F_CORRUPT;
  uint64_t base=192;
  if(flags&0x400)base+=4;
  else if(!f->cp_payload)base+=sitbytes;
  if(base+natbytes>(uint64_t)(1+f->cp_payload)*4096)return X9F_UNSUPPORTED;
  if(!(flags&0x400)&&base+natbytes>le32(cp+164))return X9F_CORRUPT;
  uint64_t byte=base+index/8;
  if(byte>=(uint64_t)(1+f->cp_payload)*4096)return X9F_UNSUPPORTED;
  uint8_t bit;
  if(byte<4096)bit=cp[byte];
  else {int e=read_block(r,f->selected_cp+byte/4096,tmp);if(e)return e;bit=tmp[byte%4096];}
  uint64_t address=f->nat+(index/512)*1024+index%512;
  /* F2FS bitmap uses MSB-first bit numbering. */
  if(bit&(1U<<(7-index%8)))address+=512;
  if(address>=f->ssa)return X9F_CORRUPT;
  int e=read_block(r,address,tmp);if(e)return e;entry=tmp+(nid%455U)*9;
 }
 if(le32(entry+1)!=owner)return X9F_IDENTITY;
 *block=le32(entry+5);
 return *block>=f->main&&*block<f->blocks?0:X9F_CORRUPT;
}
static int footer(const uint8_t *p,uint32_t nid,uint32_t owner,uint32_t offset)
{
 return le32(p+4072)==nid&&le32(p+4076)==owner&&le32(p+4080)>>3==offset;
}
int x9f_check_pin(const struct x9f_reader *r,struct x9f_info *f,uint32_t ino,uint64_t bytes,uint32_t first)
{
 int e=x9f_super(r,f);if(e)return e;
 if(!ino||!bytes||(bytes&4095)||bytes/4096>2959||first<f->main||
    first>=f->blocks||bytes/4096>f->blocks-first)return X9F_EXTENT;
 e=select_checkpoint(r,f);if(e)return e;
 f->inode=ino;e=nat_lookup(r,f,ino,ino,&f->node_block);if(e)return e;
 e=read_block(r,f->node_block,inode_page);if(e)return e;
 if(!footer(inode_page,ino,ino,0)||(le16(inode_page)&0170000)!=0100000||!le32(inode_page+12))return X9F_IDENTITY;
 f->size=le64(inode_page+16);f->inline_flags=inode_page[3];f->pinned=!!(f->inline_flags&0x40);
 if(!f->pinned)return X9F_NOT_PINNED;
 if(f->size!=bytes)return X9F_IDENTITY;
 /* No inline data/dentries, compression, released-compression or verity. */
 if(f->inline_flags&(2|4|0x80)||le32(inode_page+80)&(0x4U|0x100000U))return X9F_UNSUPPORTED;
 unsigned extra=(f->inline_flags&0x20)?le16(inode_page+360):0;
 if(extra>256||(extra&3))return X9F_CORRUPT;
 if((f->inline_flags&0x20)&&(!(f->features&8)||extra<4))return X9F_CORRUPT;
 unsigned xattrs=(f->features&0x40)?le16(inode_page+362):((f->inline_flags&1)?50:0);
 if(xattrs>923-extra/4)return X9F_CORRUPT;
 unsigned slots=923-extra/4-xattrs;
 uint32_t logical=0,total=(uint32_t)(bytes/4096);
 for(unsigned i=0;i<slots&&logical<total;i++,logical++)
  if(le32(inode_page+360+extra+4*i)!=first+logical)return X9F_EXTENT;
 for(unsigned direct=0;direct<2&&logical<total;direct++) {
  uint32_t nid=le32(inode_page+4052+direct*4),address;
  e=nat_lookup(r,f,nid,ino,&address);if(e)return e;
  e=read_block(r,address,node);if(e)return e;
  if(!footer(node,nid,ino,direct+1))return X9F_IDENTITY;
  for(unsigned i=0;i<1018&&logical<total;i++,logical++)
   if(le32(node+i*4)!=first+logical)return X9F_EXTENT;
 }
 if(logical!=total)return X9F_UNSUPPORTED;
 f->checked_blocks=logical;return 0;
}
