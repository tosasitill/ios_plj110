/* CRC-validated GPT and a read-only pinned-file virtual block layer.
 * These helpers do not parse F2FS, bypass Android encryption, allocate files,
 * expose arbitrary disk writes, or assume an extent manifest is current.
 */
#include "ufs_ro.h"

static uint32_t le32(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const uint8_t *p) {return le32(p)|(uint64_t)le32(p+4)<<32;}
static int equal(const void *a,const void *b,size_t n) {const uint8_t *p=a,*q=b;while(n--)if(*p++!=*q++)return 0;return 1;}
static void copy(void *a,const void *b,size_t n) {uint8_t *p=a;const uint8_t *q=b;while(n--)*p++=*q++;}
uint32_t x9u_crc32(uint32_t crc,const void *data,size_t size)
{
 const uint8_t *p=data;crc=~crc;
 while(size--) {crc^=*p++;for(unsigned i=0;i<8;i++)crc=(crc>>1)^((0U-(crc&1))&0xedb88320U);}
 return ~crc;
}
static int name_equal(const uint8_t *p,const char *name)
{
 for(unsigned i=0;i<36;i++) {
  if(!name[i])return !p[2*i]&&!p[2*i+1];
  if((uint8_t)name[i]!=p[2*i]||p[2*i+1])return 0;
 }
 return name[36]==0;
}
int x9u_gpt_find(struct x9u_host *h,unsigned lun,const char *name,struct x9u_partition *out)
{
 if(!h||lun>=3||!name||!out)return X9U_ARGUMENT;
 uint32_t block=h->block_size[lun];
 if(!block||h->blocks[lun]<3)return X9U_GEOMETRY;
 uint8_t *buf=h->dma+X9U_DATA_OFFSET;
 int e=x9u_read(h,lun,1,1,buf);if(e)return e;
 if(!equal(buf,"EFI PART",8)||le32(buf+8)!=0x10000)return X9U_GEOMETRY;
 uint32_t size=le32(buf+12),saved=le32(buf+16);
 if(size<92||size>block||le32(buf+20)||le64(buf+24)!=1||le64(buf+32)>=h->blocks[lun])return X9U_GEOMETRY;
 uint32_t crc=x9u_crc32(0,buf,16);uint32_t zero=0;
 crc=x9u_crc32(crc,&zero,4);crc=x9u_crc32(crc,buf+20,size-20);
 if(crc!=saved)return X9U_CRC;
 uint64_t first=le64(buf+40),last=le64(buf+48),table=le64(buf+72);
 uint32_t count=le32(buf+80),entry=le32(buf+84),table_crc=le32(buf+88);
 if(first>last||first<2||last>=h->blocks[lun]||table<2||!count||count>4096||
    entry<128||entry>block||(entry&(entry-1))||block%entry)return X9U_GEOMETRY;
 uint64_t bytes=(uint64_t)count*entry,blocks=(bytes+block-1)/block;
 if(table>=first||blocks>first-table||table>=h->blocks[lun]||blocks>h->blocks[lun]-table)return X9U_BOUNDS;
 static struct {uint64_t first,last;} ranges[4096];
 struct x9u_partition found={0};unsigned matches=0,used_ranges=0;uint64_t consumed=0;crc=0;
 while(consumed<bytes) {
  uint64_t remaining=bytes-consumed;
  uint32_t take=remaining>X9U_DATA_SIZE?X9U_DATA_SIZE:(uint32_t)remaining;
  uint32_t nblocks=(take+block-1)/block;
  e=x9u_read(h,lun,table+consumed/block,nblocks,buf);if(e)return e;
  crc=x9u_crc32(crc,buf,take);
  for(uint32_t offset=0;offset<take;offset+=entry) {
   const uint8_t *p=buf+offset;unsigned used=0;for(unsigned i=0;i<16;i++)used|=p[i];
   if(!used)continue;
   uint64_t a=le64(p+32),b=le64(p+40);
   if(a<first||a>b||b>last)return X9U_BOUNDS;
   ranges[used_ranges].first=a;ranges[used_ranges].last=b;used_ranges++;
   if(name_equal(p+56,name)) {
    found.first=a;found.count=b-a+1;copy(found.guid,p+16,16);matches++;
   }
  }
  consumed+=take;
 }
 if(crc!=table_crc)return X9U_CRC;
 for(size_t gap=used_ranges/2;gap;gap/=2)for(size_t i=gap;i<used_ranges;i++) {
  uint64_t a=ranges[i].first,b=ranges[i].last;size_t j=i;
  while(j>=gap&&ranges[j-gap].first>a) {ranges[j]=ranges[j-gap];j-=gap;}
  ranges[j].first=a;ranges[j].last=b;
 }
 for(unsigned i=1;i<used_ranges;i++)if(ranges[i-1].last>=ranges[i].first)return X9U_BOUNDS;
 if(matches!=1)return X9U_NOT_FOUND;
 *out=found;return 0;
}
int x9u_extent_validate(const struct x9u_extent *map,size_t n,uint64_t blocks,uint64_t partition_blocks)
{
 if(!map||!n||n>16384||!blocks||!partition_blocks)return X9U_ARGUMENT;
 /* The controller API is serialized; bounded BSS avoids a 32KiB stack array.
  * Sort an index, retaining logical file order for binary read lookups. */
 static uint16_t physical_order[16384];
 uint64_t pos=0;
 for(size_t i=0;i<n;i++) {
  if(map[i].file!=pos||!map[i].count||pos>blocks||map[i].count>blocks-pos||
     map[i].part>=partition_blocks||map[i].count>partition_blocks-map[i].part)return X9U_BOUNDS;
  physical_order[i]=(uint16_t)i;pos+=map[i].count;
 }
 if(pos!=blocks)return X9U_BOUNDS;
 for(size_t gap=n/2;gap;gap/=2)for(size_t i=gap;i<n;i++) {
  uint16_t value=physical_order[i];size_t j=i;
  while(j>=gap&&map[physical_order[j-gap]].part>map[value].part) {
   physical_order[j]=physical_order[j-gap];j-=gap;
  }
  physical_order[j]=value;
 }
 for(size_t i=1;i<n;i++) {
  const struct x9u_extent *a=&map[physical_order[i-1]],*b=&map[physical_order[i]];
  if(a->part+a->count>b->part)return X9U_BOUNDS;
 }
 return 0;
}
int x9u_image_read_mapped(struct x9u_host *h,unsigned lun,const struct x9u_partition *partition,
                   const struct x9u_extent *map,size_t n,uint64_t blocks,uint64_t offset,void *out,size_t length)
{
 if(!h||lun>=3||!partition||!out||!map||!n||n>16384||!blocks||h->block_size[lun]!=4096||blocks>UINT64_MAX/4096||
    partition->first>=h->blocks[lun]||partition->count>h->blocks[lun]-partition->first||
    offset>blocks*4096||length>blocks*4096-offset)return X9U_BOUNDS;
 int e;
 uint8_t *dst=out;
 while(length) {
  uint64_t logical=offset/4096;size_t lo=0,hi=n;
  while(lo<hi) {size_t mid=lo+(hi-lo)/2;if(map[mid].file<=logical)lo=mid+1;else hi=mid;}
  if(!lo)return X9U_BOUNDS;
  const struct x9u_extent *item=&map[lo-1];uint64_t delta=logical-item->file;
  if(item->file>logical||delta>=item->count||item->file>blocks||
     item->count>blocks-item->file||item->part>=partition->count||
     item->count>partition->count-item->part)return X9U_BOUNDS;
  uint32_t skip=(uint32_t)(offset%4096);uint64_t available=(item->count-delta)*4096-skip;
  size_t take=length;if(take>X9U_DATA_SIZE-skip)take=X9U_DATA_SIZE-skip;if(take>available)take=(size_t)available;
  uint32_t count=(uint32_t)((skip+take+4095)/4096);
  e=x9u_read(h,lun,partition->first+item->part+delta,count,h->dma+X9U_DATA_OFFSET);if(e)return e;
  copy(dst,h->dma+X9U_DATA_OFFSET+skip,take);dst+=take;offset+=take;length-=take;
 }
 return 0;
}

/* General callers still validate the entire map. The native kernel validates
 * its private immutable manifest before setting ready and uses mapped reads. */
int x9u_image_read(struct x9u_host *h,unsigned lun,const struct x9u_partition *partition,
                   const struct x9u_extent *map,size_t n,uint64_t blocks,uint64_t offset,void *out,size_t length)
{
 if(!partition)return X9U_BOUNDS;
 int error=x9u_extent_validate(map,n,blocks,partition->count);
 if(error)return error;
 return x9u_image_read_mapped(h,lun,partition,map,n,blocks,offset,out,length);
}
