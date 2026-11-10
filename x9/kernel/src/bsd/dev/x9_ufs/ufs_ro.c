/* Legacy UFSHCI media-read-only engine plus bounded NOP/fDeviceInit. No link reset, device power
 * command, PHY write, MCQ takeover, SMMU modification or disk write exists.
 * X9 adapter must establish a real DMA aperture before x9u_open(...,1).
 */
#include "ufs_ro.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

static void clear(void *p,size_t n) { memset(p,0,n); }
static void copy(void *d,const void *s,size_t n) { if(d!=s)memcpy(d,s,n); }
static uint32_t le32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p)<<32|be32(p+4); }
static void put32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static void big32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(24-8*i));}
static void big64(uint8_t *p,uint64_t n) {big32(p,(uint32_t)(n>>32));big32(p+4,(uint32_t)n);}
static uint32_t rd(struct x9u_host *h,unsigned o) {return h->io.read32(h->io.context,o);}
static void wr(struct x9u_host *h,unsigned o,uint32_t v) {h->io.write32(h->io.context,o,v);}
static int wait_mask(struct x9u_host *h,unsigned o,uint32_t mask,uint32_t want,uint32_t timeout)
{
 uint64_t start=h->io.now_us(h->io.context);
 for(unsigned i=0;i<=timeout/10;i++) {
  if((rd(h,o)&mask)==want)return 0;
  if(h->io.now_us(h->io.context)-start>=timeout)break;
  h->io.delay_us(h->io.context,10);
 }
 return X9U_TIMEOUT;
}
/* Match UniXNU's short polled fast path without taking over the IRQ yet.
 * Long commands back off to the original 10 us interval. Both the wall-clock
 * deadline and an iteration cap remain bounded; no timeout retry/reuse. */
static int wait_completion(struct x9u_host *h,uint32_t timeout)
{
 uint64_t start=h->io.now_us(h->io.context);
 for(uint32_t i=0;i<=timeout/2;i++) {
  h->io_poll_count++;
  if(!(rd(h,0x58)&1))return 0;
  uint64_t elapsed=h->io.now_us(h->io.context)-start;
  if(elapsed>=timeout)break;
  h->io.delay_us(h->io.context,elapsed<128?2:10);
 }
 return X9U_TIMEOUT;
}
void x9u_snapshot(const struct x9u_io *io,struct x9u_snapshot *s)
{
#define R(field,offset) s->field=io->read32(io->context,offset)
 R(cap,0);R(version,8);R(is,0x20);R(ie,0x24);R(hcs,0x30);R(hce,0x34);
 R(list_lo,0x50);R(list_hi,0x54);R(doorbell,0x58);R(run,0x60);
 R(task_doorbell,0x78);R(auto_h8,0x18);
 /* MEM_CFG MCQ is defined only in UFSHCI4+; don't read vendor offsets on v2. */
 s->memcfg=s->version>=0x400?io->read32(io->context,0x300):0;
#undef R
}
int x9u_open(struct x9u_host *h,const struct x9u_io *io,void *dma,uint64_t pa,int verified)
{
 if(!h||!io||!io->read32||!io->write32||!io->sync||!io->now_us||!io->delay_us||
    !dma||((uintptr_t)dma&1023)||(pa&1023)||pa>UINT64_MAX-X9U_DMA_SIZE)return X9U_ARGUMENT;
 clear(h,sizeof(*h));h->io=*io;h->dma=dma;h->dma_pa=pa;h->data_limit=X9U_DEFAULT_DATA_SIZE;
 x9u_snapshot(io,&h->saved);
 if(!verified)return X9U_DMA_UNVERIFIED;
 if(h->saved.cap==0xffffffffU||!h->saved.version||h->saved.version==0xffffffffU||
    !(h->saved.hce&1)||(h->saved.hcs&3)!=3)return X9U_NOT_READY;
 if(h->saved.memcfg&1)return X9U_UNSUPPORTED;
 if(h->saved.doorbell||h->saved.task_doorbell)return X9U_BUSY;
 if(!(h->saved.cap&(1U<<24))&&(pa+X9U_DMA_SIZE-1)>UINT32_MAX)return X9U_BOUNDS;
 /* Own only an idle legacy transfer list. Save/restore all changed registers. */
 wr(h,0x24,0);wr(h,0x60,0);
 if(wait_mask(h,0x60,1,0,10000)) {wr(h,0x24,h->saved.ie);return X9U_TIMEOUT;}
 if(rd(h,0x58)||rd(h,0x78)) {wr(h,0x60,h->saved.run);wr(h,0x24,h->saved.ie);return X9U_BUSY;}
 clear(dma,X9U_DMA_SIZE);io->sync(io->context,0,X9U_DMA_SIZE,0);
 wr(h,0x50,(uint32_t)pa);wr(h,0x54,(uint32_t)(pa>>32));wr(h,0x60,1);
 h->attached=1;
 if(rd(h,0x50)!=(uint32_t)pa||rd(h,0x54)!=(uint32_t)(pa>>32)||
    !(rd(h,0x60)&1)) {int e=x9u_close(h);return e?e:X9U_NOT_READY;}
 return 0;
}
int x9u_close(struct x9u_host *h)
{
 if(!h||!h->attached)return 0;
 /* Never hand memory to XNU while the controller might still DMA into it. */
 if(rd(h,0x58)) {h->poisoned=1;return X9U_DMA_PENDING;}
 wr(h,0x60,0);
 if(wait_mask(h,0x60,1,0,10000)) {h->poisoned=1;return X9U_DMA_PENDING;}
 wr(h,0x50,h->saved.list_lo);wr(h,0x54,h->saved.list_hi);
 wr(h,0x60,h->saved.run);wr(h,0x24,h->saved.ie);h->attached=0;
 return 0;
}
/* UFS transports CHECK CONDITION sense in the response UPIU itself.
 * A later REQUEST SENSE may legitimately report NO SENSE after autosense. */
static int decode_sense(struct x9u_host *h,const uint8_t *p,size_t n)
{
 if(!n)return 0;
 unsigned format=p[0]&0x7f;
 if((format==0x70||format==0x71)&&n>=14) {
  h->sense_key=p[2]&15;h->sense_asc=p[12];h->sense_ascq=p[13];
 }else if((format==0x72||format==0x73)&&n>=8) {
  h->sense_key=p[1]&15;h->sense_asc=p[2];h->sense_ascq=p[3];
 }else return 0;
 h->sense_valid=1;return 1;
}
#if defined(X9U_EXTENT_WRITES)
static int write_range(struct x9u_host *h,unsigned lun,uint64_t lba,uint32_t count)
{
 if(!h||!h->write_enabled||h->poisoned||lun!=h->write_lun||!count||
    !h->write_map||h->write_count>16384||lba<h->write_first)return X9U_BOUNDS;
 uint64_t relative=lba-h->write_first;
 if(relative>=h->write_partition_blocks||count>h->write_partition_blocks-relative)return X9U_BOUNDS;
 /* The logical write lookup supplies a hint. Recheck the physical extent
  * here (including geometry), never trust the hint as authorization. The
  * immutable map was fully checked at bind; keep a fallback for other callers. */
 for(size_t pass=0;pass<=h->write_count;pass++) {
  size_t i=pass?pass-1:h->write_hint;
  if(i>=h->write_count)continue;
  if(pass && i==h->write_hint)continue;
  h->io_write_range_steps++;
  const struct x9u_extent *e=&h->write_map[i];
  if(e->part>=h->write_partition_blocks||!e->count||e->count>h->write_partition_blocks-e->part)return X9U_BOUNDS;
  if(relative>=e->part&&relative-e->part<e->count&&count<=e->count-(relative-e->part)) {
   h->write_hint=i;return 0;
  }
 }
 return X9U_BOUNDS;
}
#endif
/* Private CDB allowlist; extent writes are kernel-only and range-gated again
 * immediately before handing the command to DMA. */
static int command(struct x9u_host *h,unsigned lun,const uint8_t cdb[16],unsigned cdb_len,uint32_t bytes)
{
 if(!h||!h->attached||h->poisoned||lun>=3||!h->data_limit||h->data_limit>X9U_DATA_SIZE||bytes>h->data_limit)return X9U_ARGUMENT;
 int writing=0,extra=0;
#if defined(X9U_EXTENT_WRITES)
 if((cdb[0]==0x2a&&cdb_len==10)||(cdb[0]==0x8a&&cdb_len==16)) {
  uint64_t lba=cdb[0]==0x2a?be32(cdb+2):be64(cdb+2);
  uint32_t count=cdb[0]==0x2a?((uint32_t)cdb[7]<<8|cdb[8]):be32(cdb+10);
  if(h->block_size[lun]!=4096||(uint64_t)count*4096!=bytes||write_range(h,lun,lba,count))return X9U_BOUNDS;
  writing=1;extra=1;
 }else if(cdb[0]==0x35&&cdb_len==10&&!bytes&&h->write_enabled&&lun==h->write_lun)extra=1;
#endif
 if(!extra&&!((cdb[0]==0&&cdb_len==6)||(cdb[0]==3&&cdb_len==6)||(cdb[0]==0x12&&cdb_len==6)||(cdb[0]==0x9e&&cdb[1]==0x10&&cdb_len==16)||
      (cdb[0]==0x28&&cdb_len==10)||(cdb[0]==0x88&&cdb_len==16)))return X9U_UNSUPPORTED;
 uint64_t command_start=h->io.now_us(h->io.context);
 h->last_ocs=15;h->last_is=0;clear(h->last_response,sizeof(h->last_response));
 h->failure_phase=0;h->sense_valid=0;h->sense_from_upiu=0;
 h->sense_key=0;h->sense_asc=0;h->sense_ascq=0;
 if(rd(h,0x58)||rd(h,0x78))return X9U_BUSY;
 uint8_t *tr=h->dma,*cmd=h->dma+0x1000,*response=h->dma+0x1200,*prdt=h->dma+0x1400;
 clear(tr,32);clear(cmd,0x400+16*X9U_PRD_COUNT);
 /* Full block READs must return every byte (residual/underflow rejected
  * below), so pre-zeroing their buffer only consumes CPU/DDR bandwidth.
  * Keep zero initialization for management, inquiry and short sense data. */
 if(!writing&&cdb[0]!=0x28&&cdb[0]!=0x88)clear(h->dma+X9U_DATA_OFFSET,bytes);
 if(!++h->tag)++h->tag;
 cmd[0]=1;cmd[1]=bytes?(writing?0x20:0x40):0;cmd[2]=(uint8_t)lun;cmd[3]=h->tag;
 big32(cmd+12,bytes);copy(cmd+16,cdb,cdb_len);
 put32(tr,(1U<<28)|(bytes?((writing?1U:2U)<<25):0)|(1U<<24));put32(tr+8,15);
 uint64_t ucd=h->dma_pa+0x1000,data=h->dma_pa+X9U_DATA_OFFSET;
 put32(tr+16,(uint32_t)ucd);put32(tr+20,(uint32_t)(ucd>>32));
 unsigned prds=(bytes+X9U_PRD_SIZE-1)/X9U_PRD_SIZE;
 put32(tr+24,(128U<<16)|128U);put32(tr+28,(256U<<16)|prds);
 for(unsigned i=0,remaining=bytes;i<prds;i++) {
  unsigned take=remaining>X9U_PRD_SIZE?X9U_PRD_SIZE:remaining;
  put32(prdt+i*16,(uint32_t)data);put32(prdt+i*16+4,(uint32_t)(data>>32));
  put32(prdt+i*16+12,take-1);data+=take;remaining-=take;
 }
 /* Only these CPU-written ranges are handed to this command. */
 h->io.sync(h->io.context,0,32,0);
 h->io.sync(h->io.context,0x1000,0x400+16*X9U_PRD_COUNT,0);
 if(bytes)h->io.sync(h->io.context,X9U_DATA_OFFSET,bytes,0);
 uint64_t device_start=h->io.now_us(h->io.context);
 wr(h,0x58,1);
 if(wait_completion(h,X9U_COMMAND_TIMEOUT_US)) {
  /* UTRLCLR is inverse: zero clears the chosen slot. No new command follows. */
  wr(h,0x5c,~1U);h->poisoned=1;h->failure_phase=1;
  if(wait_mask(h,0x58,1,0,10000))return X9U_DMA_PENDING;
  return X9U_TIMEOUT;
 }
 uint64_t device_us=h->io.now_us(h->io.context)-device_start;
 /* Reclaim OCS, response and the actual transfer; unused DMA tail stays idle. */
 h->io.sync(h->io.context,0,32,1);
 h->io.sync(h->io.context,0x1200,0x200,1);
 if(bytes&&!writing)h->io.sync(h->io.context,X9U_DATA_OFFSET,bytes,1);
 h->last_is=rd(h,0x20);h->last_ocs=le32(tr+8)&255;copy(h->last_response,response,sizeof(h->last_response));
 /* Acknowledge only UTR completion generated by this request, not other errors. */
 if(h->last_is&1)wr(h,0x20,1);
 if(h->last_ocs){h->failure_phase=2;return X9U_RESPONSE;}
 /* Target failure (Response=1) accompanies legitimate CHECK CONDITION.
  * Validate framing separately from SCSI status, as Linux does. Reserved
  * response values and target-failure + GOOD are never successful reads. */
 if(response[0]!=0x21||response[2]!=lun||response[3]!=h->tag||
    response[6]>1||(response[6]&&!response[7])) {
  h->failure_phase=3;return X9U_RESPONSE;
 }
 if(response[7]) {
  h->failure_phase=4;
  if(response[7]==2) {
   unsigned segment=(unsigned)response[10]<<8|response[11];
   unsigned length=(unsigned)response[32]<<8|response[33];
   if(length) {
    if(length>18||segment<length+2||segment>480){h->failure_phase=3;return X9U_RESPONSE;}
    h->sense_from_upiu=decode_sense(h,response+34,length);
    if(!h->sense_from_upiu){h->failure_phase=3;return X9U_RESPONSE;}
   }
  }
  return X9U_RESPONSE;
 }
 uint32_t residual=be32(response+12);
 /* REQUEST SENSE can return an 8-byte descriptor instead of 18 allocated
  * bytes. Never accept short data for capacity, GPT or filesystem reads. */
 if((response[1]&0x40)||((response[1]&0x20)||residual)) {
  if((response[1]&0x40)||cdb[0]!=3||!(response[1]&0x20)||!residual||residual>bytes||bytes-residual<4) {
   h->failure_phase=5;return X9U_RESPONSE;
  }
 }
 h->io_commands++;h->io_device_us+=device_us;
 h->io_total_us+=h->io.now_us(h->io.context)-command_start;
 if(device_us>h->io_max_device_us)h->io_max_device_us=device_us;
 if(writing)h->io_write_bytes+=bytes;else h->io_read_bytes+=bytes;
 return 0;
}
/* Only NOP and fDeviceInit SET/READ_FLAG are supported: no arbitrary query,
 * descriptor write, persistent configuration, crypto keyslot or media WRITE. */
static int management(struct x9u_host *h,unsigned opcode,unsigned *flag)
{
 if(!h||!h->attached||h->poisoned||(opcode!=0&&opcode!=5&&opcode!=6))return X9U_ARGUMENT;
 if(rd(h,0x58)||rd(h,0x78))return X9U_BUSY;
 uint8_t *tr=h->dma,*cmd=tr+0x1000,*response=tr+0x1200;
 clear(tr,32);clear(cmd,0x400+16*X9U_PRD_COUNT);
 if(!++h->tag)++h->tag;
 cmd[0]=opcode?0x16:0;cmd[3]=h->tag;
 if(opcode){cmd[5]=opcode==6?0x81:1;cmd[12]=(uint8_t)opcode;cmd[13]=1;}
 put32(tr,(1U<<28)|(1U<<24));put32(tr+8,15);
 uint64_t ucd=h->dma_pa+0x1000;
 put32(tr+16,(uint32_t)ucd);put32(tr+20,(uint32_t)(ucd>>32));
 put32(tr+24,(128U<<16)|128U);put32(tr+28,256U<<16);
 h->io.sync(h->io.context,0,32,0);
 h->io.sync(h->io.context,0x1000,0x400+16*X9U_PRD_COUNT,0);wr(h,0x58,1);
 if(wait_completion(h,X9U_COMMAND_TIMEOUT_US)) {
  wr(h,0x5c,~1U);h->poisoned=1;
  if(wait_mask(h,0x58,1,0,10000))return X9U_DMA_PENDING;
  return X9U_TIMEOUT;
 }
 h->io.sync(h->io.context,0,32,1);
 h->io.sync(h->io.context,0x1200,0x200,1);
 h->last_is=rd(h,0x20);h->last_ocs=le32(tr+8)&255;copy(h->last_response,response,sizeof(h->last_response));
 if(h->last_is&1)wr(h,0x20,1);
 if(h->last_ocs||response[0]!=(opcode?0x36:0x20)||response[2]||response[3]!=h->tag||
    response[6]||response[7]||response[10]||response[11])return X9U_RESPONSE;
 if(opcode&&(response[12]!=opcode||response[13]!=1||response[14]||response[15]))return X9U_RESPONSE;
 if(flag)*flag=be32(response+20)&1;
 return 0;
}
int x9u_device_init(struct x9u_host *h,unsigned *stage)
{
 if(!stage)return X9U_ARGUMENT;
 *stage=1;int error=management(h,0,0);if(error)return error;
 *stage=2;error=management(h,6,0);if(error)return error;
 *stage=3;uint64_t start=h->io.now_us(h->io.context);
 for(unsigned tries=0;tries<1000;tries++) {
  unsigned flag=1;error=management(h,5,&flag);if(error)return error;
  if(!flag){*stage=4;return 0;}
  if(h->io.now_us(h->io.context)-start>=1000000)break;
  h->io.delay_us(h->io.context,1000);
 }
 return X9U_TIMEOUT;
}
int x9u_inquiry(struct x9u_host *h,unsigned lun,void *out,size_t size)
{
 if(!out||size<36)return X9U_ARGUMENT;
 uint8_t cdb[16]={0x12,0,0,0,36};int e=command(h,lun,cdb,6,36);
 if(!e)copy(out,h->dma+X9U_DATA_OFFSET,36);return e;
}
int x9u_lun_ready(struct x9u_host *h,unsigned lun)
{
 if(!h||lun>=3)return X9U_ARGUMENT;
 h->ready_attempts=0;h->sense_requests=0;h->sense_request_error=0;
 h->ready_unit_attentions=0;h->ready_last_category=0;h->ready_last_asc=0;h->ready_last_ascq=0;
 clear(h->tur_response,sizeof(h->tur_response));h->tur_failure_phase=0;h->tur_ocs=0;
 for(unsigned attempt=0;attempt<3;attempt++) {
  h->ready_attempts=attempt+1;
  uint8_t cdb[16]={0};int error=command(h,lun,cdb,6,0);if(!error)return 0;
  copy(h->tur_response,h->last_response,sizeof(h->tur_response));
  h->tur_failure_phase=h->failure_phase;h->tur_ocs=h->last_ocs;
  if(error!=X9U_RESPONSE||h->failure_phase!=4)return error;
  unsigned status=h->last_response[7];
  if(status==8||status==0x28){h->io.delay_us(h->io.context,100000);continue;}
  if(status!=2)return error;
  if(!h->sense_valid) {
   cdb[0]=3;cdb[4]=18;h->sense_requests++;
   error=command(h,lun,cdb,6,18);h->sense_request_error=error;if(error)return error;
   uint32_t residual=be32(h->last_response+12);
   if(residual>18||!decode_sense(h,h->dma+X9U_DATA_OFFSET,18-residual))return X9U_RESPONSE;
  }
  h->ready_last_category=h->sense_key;h->ready_last_asc=h->sense_asc;h->ready_last_ascq=h->sense_ascq;
  if(h->sense_key==6)h->ready_unit_attentions++;
  if(h->sense_key!=6&&h->sense_key!=0&&!(h->sense_key==2&&h->sense_asc==4))return X9U_RESPONSE;
  /* NO SENSE is not readiness: require a later TUR with GOOD status. */
  h->io.delay_us(h->io.context,100000);
 }
 return X9U_NOT_READY;
}
int x9u_capacity(struct x9u_host *h,unsigned lun)
{
 if(!h||lun>=3)return X9U_ARGUMENT;
 uint8_t cdb[16]={0x9e,0x10};big32(cdb+10,32);
 int e=command(h,lun,cdb,16,32);if(e)return e;
 const uint8_t *data=h->dma+X9U_DATA_OFFSET;uint64_t last=be64(data);uint32_t block=be32(data+8);
 if(last==UINT64_MAX||!last||block<512||block>4096||(block&(block-1)))return X9U_GEOMETRY;
 h->blocks[lun]=last+1;h->block_size[lun]=block;return 0;
}
int x9u_read(struct x9u_host *h,unsigned lun,uint64_t lba,uint32_t count,void *out)
{
 if(!h||lun>=3||!out||!count||!h->block_size[lun]||!h->blocks[lun]||
    lba>=h->blocks[lun]||count>h->blocks[lun]-lba||
    count>h->data_limit/h->block_size[lun])return X9U_BOUNDS;
 uint8_t cdb[16]={0};unsigned length;
 if(lba<=UINT32_MAX && count<=UINT16_MAX && count-1<=UINT32_MAX-lba) {
  cdb[0]=0x28;big32(cdb+2,(uint32_t)lba);cdb[7]=(uint8_t)(count>>8);cdb[8]=(uint8_t)count;length=10;
 } else {cdb[0]=0x88;big64(cdb+2,lba);big32(cdb+10,count);length=16;}
 uint32_t bytes=count*h->block_size[lun];int e=command(h,lun,cdb,length,bytes);
 if(!e && out!=h->dma+X9U_DATA_OFFSET)copy(out,h->dma+X9U_DATA_OFFSET,bytes);
 return e;
}

#if defined(X9U_EXTENT_WRITES)
int x9u_bind_extent_writes(struct x9u_host *h,unsigned lun,const struct x9u_partition *p,
                          const struct x9u_extent *map,size_t n,uint64_t blocks)
{
 if(!h||!h->attached||h->poisoned||h->write_enabled||lun>=3||!p||h->block_size[lun]!=4096||
    p->first>=h->blocks[lun]||p->count>h->blocks[lun]-p->first)return X9U_BOUNDS;
 int error=x9u_extent_validate(map,n,blocks,p->count);if(error)return error;
 h->write_map=map;h->write_count=n;h->write_hint=0;h->write_first=p->first;h->write_partition_blocks=p->count;
 h->write_blocks=blocks;h->write_lun=lun;h->write_enabled=1;return 0;
}
static int write_blocks(struct x9u_host *h,uint64_t lba,uint32_t count)
{
 if(!count||count>h->data_limit/4096||write_range(h,h->write_lun,lba,count))return X9U_BOUNDS;
 uint8_t cdb[16]={0};unsigned length;
 if(lba<=UINT32_MAX&&count-1<=UINT32_MAX-lba) {
  cdb[0]=0x2a;big32(cdb+2,(uint32_t)lba);cdb[7]=(uint8_t)(count>>8);cdb[8]=(uint8_t)count;length=10;
 }else {cdb[0]=0x8a;big64(cdb+2,lba);big32(cdb+10,count);length=16;}
 return command(h,h->write_lun,cdb,length,count*4096);
}
int x9u_image_write(struct x9u_host *h,uint64_t offset,const void *source,size_t length)
{
 if(!h||!h->write_enabled||!h->attached||h->poisoned||(!source&&length)||
    h->write_blocks>UINT64_MAX/4096||offset>h->write_blocks*4096||
    length>h->write_blocks*4096-offset)return X9U_BOUNDS;
 const uint8_t *src=source;
 while(length) {
  uint64_t logical=offset/4096;size_t lo=0,hi=h->write_count;
  while(lo<hi){size_t mid=lo+(hi-lo)/2;if(h->write_map[mid].file<=logical)lo=mid+1;else hi=mid;}
  if(!lo)return X9U_BOUNDS;
  const struct x9u_extent *item=&h->write_map[lo-1];uint64_t delta=logical-item->file;
  if(item->file>logical||item->file>=h->write_blocks||delta>=item->count||item->count>h->write_blocks-item->file)return X9U_BOUNDS;
  unsigned skip=(unsigned)(offset%4096);uint64_t available=(item->count-delta)*4096-skip;
  size_t take=length;if(take>h->data_limit-skip)take=h->data_limit-skip;if(take>available)take=(size_t)available;
  uint32_t count=(uint32_t)((skip+take+4095)/4096);uint64_t lba=h->write_first+item->part+delta;
  h->write_hint=lo-1;
  if(write_range(h,h->write_lun,lba,count))return X9U_BOUNDS;
  uint8_t *bounce=h->dma+X9U_DATA_OFFSET;
  if(skip||take%4096) {int e=x9u_read(h,h->write_lun,lba,count,bounce);if(e)return e;}
  copy(bounce+skip,src,take);
  int e=write_blocks(h,lba,count);if(e)return e;
  src+=take;offset+=take;length-=take;
 }
 return 0;
}
int x9u_sync_image(struct x9u_host *h)
{
 if(!h||!h->write_enabled||!h->attached||h->poisoned)return X9U_BOUNDS;
 uint8_t cdb[16]={0x35};return command(h,h->write_lun,cdb,10,0);
}
#endif
