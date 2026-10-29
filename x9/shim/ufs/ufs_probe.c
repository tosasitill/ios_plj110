/* X9 direct-LK retained UFS adapter. First hardware qualification entry.
 * Captured FDT: UFS0 0x16810000/0x5000; SOC SMMUv3 0x14900000; SID0x63.
 * Never runs MT6789/Qualcomm power-on, resets UFS or changes any SMMU table.
 */
#include "ufs_ro.h"
#include "ufs_smmu.h"
#include "ufs_link.h"
#include "ufs_hs.h"
#include "f2fs_ro.h"
#include "ufs_decrypt.h"
#if X9_UFS_NATIVE
#include "sha256.h"
#endif
#if __has_include("ufs_keys_private.h")
#include "ufs_keys_private.h"
#define X9_HAS_PRIVATE_KEYS 1
#else
#define X9_HAS_PRIVATE_KEYS 0
#endif
static struct x9d_context decryption;
#include "../hw.h"
#include "../fdt.h"
#include "../plog.h"

#if X9_UFS_ROOT
#include "ufs_root_config.h"
#endif
static struct x9u_host controller;
#if X9_UFS_ROOT
static void *root_destination;
static size_t root_capacity;
static int root_valid,root_status=X9U_NOT_FOUND;
uint64_t x9_ufs_root_bytes(void){return X9_ROOT_BYTES;}
void x9_ufs_root_destination(void *address,size_t capacity)
{root_destination=address;root_capacity=capacity;root_valid=0;root_status=X9U_NOT_FOUND;}
int x9_ufs_root_verified(void){return root_valid;}
#if X9_UFS_NATIVE
static uint32_t be32(const uint8_t *);
static uint8_t native_header[65536];
static int native_root_verify(unsigned lun,const struct x9u_partition *part)
{
 if(lun!=X9_ROOT_LUN||part->first!=X9_ROOT_PART_FIRST||part->count!=X9_ROOT_PART_BLOCKS)return X9U_GEOMETRY;
 for(unsigned i=0;i<16;i++)if(part->guid[i]!=x9_root_guid[i])return X9U_GEOMETRY;
 int error=x9u_image_read(&controller,lun,part,x9_root_extents,sizeof(x9_root_extents)/sizeof(x9_root_extents[0]),X9_ROOT_BYTES/4096,0,native_header,sizeof(native_header));
 if(error)return error;
 struct sha256 hash;uint8_t digest[32];sha256_init(&hash);sha256_update(&hash,native_header,sizeof(native_header));sha256_final(&hash,digest);
 /* Mutable copies legitimately update HFS timestamps, allocation metadata
  * and journal state. Their original full digest was verified at provisioning;
  * keep exact UUID/geometry/map guards, not an immutable-header expectation. */
 if(!X9_ROOT_MUTABLE)for(unsigned i=0;i<32;i++)if(digest[i]!=x9_root_header_sha256[i])return X9U_CRC;
 for(unsigned i=0;i<8;i++)if(native_header[1128+i]!=x9_root_volume_uuid[i])return X9U_GEOMETRY;
 if(native_header[1024]!='H'||(native_header[1025]!='X'&&native_header[1025]!='+'))return X9U_GEOMETRY;
 uint64_t bytes=(uint64_t)be32(native_header+1064)*be32(native_header+1068);
 if(bytes!=X9_ROOT_BYTES)return X9U_GEOMETRY;
 /* Do not mount a previously inconsistent or unclean non-journaled writable
  * root. A valid UUID/extent map does not establish HFS consistency. */
 if(X9_ROOT_MUTABLE){
  uint32_t attrs=be32(native_header+1028);
  if((attrs&0x4000U)||(!(attrs&0x2000U)&&!(attrs&0x100U))){
   plog_hex("UFS NATIVE ROOT HFS unhealthy attrs",attrs);plog_flush();return X9U_GEOMETRY;
  }
 }
 plog_str("UFS NATIVE ROOT HEADER VERIFIED: SHA256/UUID/HFS geometry match; full image stays on disk\n");plog_flush();
 return 0;
}
#endif

#if !X9_UFS_NATIVE
static void root_progress(uint64_t done)
{
 wdt_kick();
 if(!(done&0x3fffffUL)||done==X9_ROOT_BYTES){plog_hex("UFS ROOT bytes read",done);plog_flush();}
}
#endif
#endif
static uint8_t dma_area[X9U_DMA_SIZE] __attribute__((aligned(16384)));

static uintptr_t current_fdt;
static unsigned stuck_dma;
static uint32_t be32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint64_t be64(const uint8_t *p) {return (uint64_t)be32(p)<<32|be32(p+4);}
static uint32_t hci_read(void *ctx,unsigned offset) {(void)ctx;return rd32(0x16810000UL+offset);}
static void hci_write(void *ctx,unsigned offset,uint32_t value)
{(void)ctx;wr32(0x16810000UL+offset,value);__asm__ volatile("dsb sy" ::: "memory");}
static uint64_t now_us(void *ctx) {(void)ctx;return rsr("cntpct_el0")/(counter_frequency()/1000000);}
static void delay_us(void *ctx,unsigned us)
{
 (void)ctx;uint64_t start=rsr("cntpct_el0"),ticks=counter_frequency()*us/1000000;
 while(rsr("cntpct_el0")-start<ticks)wdt_kick();
}
static void sync_dma(void *ctx,unsigned offset,size_t size,int to_cpu)
{
 (void)ctx;(void)offset;(void)size;(void)to_cpu;
 /* start.S enters with EL1 MMU and D-cache off. Runtime rejects other modes. */
 __asm__ volatile("dsb sy" ::: "memory");
}
static uint32_t smmu_read(void *ctx,unsigned offset) {(void)ctx;return rd32(0x14900000UL+offset);}
static int table_read(void *ctx,uint64_t address,uint64_t *value)
{
 (void)ctx;
 if((address&7)||!fdt_ram_available(current_fdt,address,8))return -1;
 *value=*(volatile uint64_t *)(uintptr_t)address;
 __asm__ volatile("dsb sy" ::: "memory");return 0;
}
static int verify_dt(uintptr_t fdt)
{
 const uint8_t *p;size_t n;
 if(!fdt_property(fdt,"/soc/ufshci@16810000","reg",&p,&n)||n!=16||
    be64(p)!=0x16810000UL||be64(p+8)!=0x5000)return 0;
 if(!fdt_property(fdt,"/soc/ufshci@16810000","iommus",&p,&n)||n!=8||be32(p+4)!=0x63)return 0;
 uint32_t phandle=be32(p);
 if(!fdt_property(fdt,"/soc/iommu@14900000","phandle",&p,&n)||n!=4||be32(p)!=phandle)return 0;
 if(!fdt_property(fdt,"/soc/iommu@14900000","reg",&p,&n)||n<16||be64(p)!=0x14900000UL)return 0;
 return 1;
}
static void state_log(const struct x9u_snapshot *s)
{
 plog_hex("UFS CAP",s->cap);plog_hex("UFS VER",s->version);
 plog_hex("UFS HCE",s->hce);plog_hex("UFS HCS",s->hcs);
 plog_hex("UFS IS",s->is);plog_hex("UFS IE",s->ie);
 plog_hex("UFS UTRLBA",(uint64_t)s->list_hi<<32|s->list_lo);
 plog_hex("UFS doorbell",s->doorbell);plog_hex("UFS run",s->run);
 plog_hex("UFS task doorbell",s->task_doorbell);
 plog_hex("UFS AHIT",s->auto_h8);plog_hex("UFS MEM_CFG",s->memcfg);plog_flush();
}
#if !X9_UFS_ROOT
/* Frozen Android qualification file, captured 2026-10-26. No F2FS parser:
 * these values are a snapshot, not proof of current inode/pin ownership. */
static uint8_t pin_readback[X9U_DATA_SIZE];
struct f2fs_route {unsigned lun;const struct x9u_partition *partition;};
static int f2fs_read(void *context,uint64_t block,void *out)
{
 struct f2fs_route *route=context;
 if(block>=route->partition->count)return X9U_BOUNDS;
 int error=x9u_read(&controller,route->lun,route->partition->first+block,1,out);
 if(error)return error;
 if(decryption.metadata.ready)return x9d_metadata(&decryption,block,out);
 return 0;
}
static int f2fs_pin_check(unsigned lun,const struct x9u_partition *partition)
{
 struct f2fs_route route={lun,partition};
 volatile struct x9f_reader setup;
 setup.context=&route;setup.read=f2fs_read;setup.blocks=partition->count;
 struct x9f_reader reader=setup;
 struct x9f_info info;
 int error=x9f_check_pin(&reader,&info,659739,8388608UL,5143552);
 plog_hex("UFS F2FS parse status",(uint64_t)(int64_t)error);
 if(error==X9F_NOT_F2FS) {
  plog_str("UFS F2FS: no plaintext magic in either superblock; encrypted or invalid volume\n");
  plog_str("UFS F2FS: default-key metadata requires the correct effective XTS key\n");
 } else {
  plog_hex("UFS F2FS blocks",info.blocks);
  plog_hex("UFS F2FS features",info.features);
  plog_hex("UFS F2FS checkpoint version",info.checkpoint_version);
  plog_hex("UFS F2FS checkpoint flags",info.checkpoint_flags);
  if(error==X9F_UNCLEAN)plog_str("UFS F2FS: checkpoint not clean; recovery required, no stale pin assertion\n");
  if(!error) {
   plog_hex("UFS F2FS inode",info.inode);plog_hex("UFS F2FS inode block",info.node_block);
   plog_hex("UFS F2FS pinned",info.pinned);plog_hex("UFS F2FS verified data blocks",info.checked_blocks);
   plog_str("UFS F2FS PIN BLOCK MAP VERIFIED: known inode; pathname/lifetime not verified\n");
  }
 }
 if(error)plog_str("UFS F2FS pin/ownership NOT verified; raw snapshot check is separate\n");
 plog_flush();return error;
}
static int pin_file_check(unsigned lun,const struct x9u_partition *partition)
{
 const uint8_t guid[16]={0x63,0,0,0,0xc2,0x39,0x88,0x44,0x9b,0xb0,0,0xcb,0x43,0xc9,0xcc,0xd4};
 plog_str("UFS PIN: checking saved Android encrypted-file snapshot\n");
 if(lun!=2||controller.block_size[lun]!=4096||partition->first!=5783552UL||partition->count!=119101432UL) {
  plog_str("UFS PIN SKIP: LUN/block/partition geometry differs\n");return X9U_GEOMETRY;
 }
 for(unsigned i=0;i<16;i++)if(partition->guid[i]!=guid[i]) {
  plog_str("UFS PIN SKIP: userdata GUID differs\n");return X9U_GEOMETRY;
 }
#if X9_HAS_PRIVATE_KEYS
 int crypto_status=x9d_init(&decryption,x9_private_metadata_xts_key,x9_private_file_xts_key,0);
#else
 int crypto_status=x9d_init(&decryption,0,0,0);
#endif
 plog_hex("UFS DECRYPT key setup status",(uint64_t)(int64_t)crypto_status);
 plog_hex("UFS DECRYPT metadata key present",decryption.metadata.ready);
 plog_hex("UFS DECRYPT file key present",decryption.file.ready);
 if(!decryption.metadata.ready||!decryption.file.ready)
  plog_str("UFS DECRYPT KEY UNAVAILABLE: dmctl hides key; policy/nonce do not supply key bytes\n");
 int metadata_status=f2fs_pin_check(lun,partition);
 plog_hex("UFS PIN F2FS metadata status",(uint64_t)(int64_t)metadata_status);
 if(controller.poisoned)return metadata_status?metadata_status:X9U_RESPONSE;
 const struct x9u_extent extent={0,5143552UL,2048};
 uint32_t crc=0,plain_crc=0;unsigned nonzero=0;
 int plaintext_allowed=!metadata_status&&decryption.file.ready;
 if(!plaintext_allowed)plog_str("UFS DECRYPT: plaintext check unavailable; need verified metadata and file key\n");
 for(uint64_t offset=0;offset<8388608UL;offset+=sizeof(pin_readback)) {
  int error=x9u_image_read(&controller,lun,partition,&extent,1,2048,offset,pin_readback,sizeof(pin_readback));
  if(error) {
   plog_hex("UFS PIN read offset",offset);
   plog_hex("UFS PIN read error",(uint64_t)(int64_t)error);
   plog_flush();return error;
  }
  crc=x9u_crc32(crc,pin_readback,sizeof(pin_readback));
  if(plaintext_allowed) {
   for(unsigned i=0;i<sizeof(pin_readback);i+=4096) {
    error=x9d_file(&decryption,659739,(offset+i)/4096,pin_readback+i);
    if(error)return error;
   }
   plain_crc=x9u_crc32(plain_crc,pin_readback,sizeof(pin_readback));
   for(unsigned i=0;i<sizeof(pin_readback);i++)nonzero|=pin_readback[i];
  }
  wdt_kick();
 }
 plog_hex("UFS PIN bytes read",8388608UL);
 plog_hex("UFS PIN raw ciphertext CRC32",crc);
 plog_hex("UFS PIN expected CRC32",0x97ccc5b7UL);
 if(crc!=0x97ccc5b7U) {
  plog_str("UFS PIN MISMATCH: saved extent content changed; file identity NOT verified\n");
  plog_flush();return X9U_CRC;
 }
 plog_str("UFS PIN SNAPSHOT MATCH: full 8MiB encrypted content matches Android capture\n");
 if(plaintext_allowed) {
  plog_hex("UFS DECRYPT plaintext CRC32",plain_crc);
  if(nonzero) {
   plog_str("UFS DECRYPT MISMATCH: expected zero file; key/policy/content not verified\n");plog_flush();return X9U_CRC;
  }
  plog_str("UFS DECRYPT FILE VERIFIED: all 8388608 plaintext bytes match Android zero file\n");
 }
 if(metadata_status)plog_str("UFS PIN: F2FS ownership/pin unverified; plaintext root NOT verified\n");
 else plog_str("UFS PIN: inode pin/block map verified; pathname/lifetime and plaintext root unverified\n");
 plog_flush();return 0;
}
static void pin_file_manifest_log(void)
{
 plog_str("UFS PIN saved file: /data/local/tmp/x9-pin-test-20261026-194306/test-8m.bin\n");
 plog_hex("UFS PIN saved inode",659739UL);
 plog_hex("UFS PIN saved bytes",8388608UL);
 plog_hex("UFS PIN saved FIEMAP flags",0x1089);
 plog_hex("UFS PIN saved LUN",2);
 plog_hex("UFS PIN userdata relative block",5143552UL);
 plog_hex("UFS PIN UFS absolute LBA",10927104UL);
 plog_hex("UFS PIN saved Android pinned",1);
 plog_str("UFS PIN: values above are Android snapshot, not a live pin check\n");plog_flush();
}
#endif
/* Captured LK c56ec..c5714 and c57e0..c5808: exact RST_n SMC ABI.
 * platform selector at c200b8 returns2, selecting this firmware path. */
int x9u_x9_reset_route(uintptr_t);
static int host_reset(void)
{
 if(!x9u_x9_reset_route(current_fdt))return X9U_ARGUMENT;
 /* Exact DT reset bank: unipro/crypto/HCI IDs1/2/3, active-high SET/CLR.
  * Match vendor ufs_mtk_host_reset order. No absent M-PHY reset is invented. */
 wr32(0x16840148UL,4);wr32(0x16840148UL,2);wr32(0x16840148UL,1);
 __asm__ volatile("dsb sy":::"memory");delay_us(0,100);
 wr32(0x1684014cUL,1);wr32(0x1684014cUL,2);wr32(0x1684014cUL,4);
 __asm__ volatile("dsb sy":::"memory");delay_us(0,600);
 plog_str("UFS MT6993 host reset: DT-verified unipro/crypto/HCI reset bank\n");plog_flush();return 0;
}
static int device_reset(void *context,unsigned high)
{
 (void)context;
 if(high==2)return host_reset();
 plog_str(high?"UFS RST_n: invoking captured LK release SMC\n":"UFS RST_n: invoking captured LK assert SMC\n");plog_flush();
 uint64_t fid=0xc200040dUL;
 for(unsigned attempt=0;attempt<2;attempt++) {
 register uint64_t x0 __asm__("x0")=fid;
 register uint64_t x1 __asm__("x1")=2;
 register uint64_t x2 __asm__("x2")=high?1:0;
 register uint64_t x3 __asm__("x3")=0;
 register uint64_t x4 __asm__("x4")=0;
 register uint64_t x5 __asm__("x5")=0;
 register uint64_t x6 __asm__("x6")=0;
 register uint64_t x7 __asm__("x7")=0;
 __asm__ volatile("smc #0":"+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7)::
 "x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory","cc");
 plog_hex(high?"UFS device RST_n release status":"UFS device RST_n assert status",x0);plog_flush();
 if((x0!=UINT64_MAX&&x0!=UINT32_MAX)||attempt)return x0?X9U_RESPONSE:0;
 /* SMCCC_NOT_SUPPORTED only: the kernel's documented UFS SiP ABI uses
  * the same selector/argument order. Never retry an operational error. */
 fid=0xc2000276UL;
 plog_str("UFS RST_n: LK service unsupported; using documented kernel UFS SiP ABI\n");plog_flush();
 }
 return X9U_RESPONSE;
}
#if X9_UFS_NATIVE
/* OPPO ufs_mtk_ref_clk_notify: SiP bit3, on, PRE_CHANGE0/POST_CHANGE1,host0. */
static int refclk_notify(void *context,unsigned on,unsigned stage)
{
 (void)context;
 register uint64_t x0 __asm__("x0")=0xc2000276UL;
 register uint64_t x1 __asm__("x1")=8;
 register uint64_t x2 __asm__("x2")=on;
 register uint64_t x3 __asm__("x3")=stage;
 register uint64_t x4 __asm__("x4")=0;
 register uint64_t x5 __asm__("x5")=0;
 register uint64_t x6 __asm__("x6")=0;
 register uint64_t x7 __asm__("x7")=0;
 __asm__ volatile("smc #0":"+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7)::
 "x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory","cc");
 plog_hex("UFS HS refclk notify stage",stage);plog_hex("UFS HS refclk notify status",x0);plog_flush();
 return x0?X9U_RESPONSE:0;
}
static void hs_log(const struct x9u_hs_state *h,int error)
{
 plog_hex("UFS HS status",(uint64_t)(int64_t)error);plog_hex("UFS HS stage",h->stage);
 plog_hex("UFS HS before mode",h->before[0]);plog_hex("UFS HS before TX gear",h->before[1]);plog_hex("UFS HS before RX gear",h->before[2]);
 plog_hex("UFS HS after mode",h->after[0]);plog_hex("UFS HS after TX gear",h->after[1]);plog_hex("UFS HS after RX gear",h->after[2]);
 plog_hex("UFS HS after TX lanes",h->after[3]);plog_hex("UFS HS after RX lanes",h->after[4]);plog_hex("UFS HS after series",h->after[5]);
 plog_hex("UFS HS refclk before",h->refclk_before);plog_hex("UFS HS refclk after",h->refclk_after);plog_hex("UFS HS UPMCRS",h->upmcrs);
 plog_hex("UFS HS last UIC command",h->last_command);plog_hex("UFS HS last UIC attribute",h->last_attribute);plog_hex("UFS HS UIC result",h->uic_result);
 if(!error)plog_str("UFS HS VERIFIED: mode/gear/lane readback accepted; keep link for XNU\n");
 plog_flush();
}
#endif
static void link_log(const struct x9u_link *link,int status)
{
 plog_hex("UFS wake status",(uint64_t)(int64_t)status);
 plog_hex("UFS wake stage",link->stage);
 plog_hex("UFS wake HCE",link->hce);plog_hex("UFS wake HCS",link->hcs);
 plog_hex("UFS wake IS",link->is);plog_hex("UFS wake UIC result",link->uic_result);
 plog_hex("UFS wake UIC arg3",link->uic_arg3);
 plog_hex("UFS wake last UIC command",link->last_uic_command);
 plog_hex("UFS wake last UIC attribute",link->last_uic_attribute);
 plog_hex("UFS wake prelink completed",link->prelink_completed);
 plog_hex("UFS wake link attempts",link->attempts);
 plog_hex("UFS ready HCS before postlink",link->ready_hcs);
 plog_hex("UFS repeat startup skipped",link->repeat_startup_skipped);
 if(!status&&link->repeat_startup_skipped)plog_str("UFS active link retained; next gate is NOP/fDeviceInit\n");
 plog_hex("UFS wake coherence",link->coherence);
 plog_hex("UFS wake reset low",link->reset_low_status);plog_hex("UFS wake reset high",link->reset_high_status);
 plog_hex("UFS wake SAVEPOWER before",link->savepower_before);
 plog_hex("UFS wake SAVEPOWER after",link->savepower_after);
 plog_hex("UFS wake PHY error",link->phy_error);plog_hex("UFS wake DL error",link->dl_error);
 plog_hex("UFS wake NL error",link->nl_error);plog_hex("UFS wake TL error",link->tl_error);
 plog_hex("UFS wake DME error",link->dme_error);plog_flush();
}
int x9_ufs_probe(uintptr_t fdt)
{
 current_fdt=fdt;stuck_dma=0;
#if !X9_UFS_ROOT
 pin_file_manifest_log();
#else
 plog_str("UFS ROOT: data-a-live plaintext extent image; no Android key required\n");
 plog_hex("UFS ROOT expected bytes",X9_ROOT_BYTES);
 plog_hex("UFS ROOT extent count",sizeof(x9_root_extents)/sizeof(x9_root_extents[0]));
#endif
 plog_str("X9 UFS: read-only qualification with bounded HCI wake; no media writes\n");plog_flush();
 if(!verify_dt(fdt)) {plog_str("UFS stop: current DT UFS/SMMU route differs\n");plog_flush();return X9U_ARGUMENT;}
 /* Volatile field stores prevent clang from emitting an absolute function-
  * pointer literal table; LK relocates this image without a relocation pass. */
 volatile struct x9u_io setup;
 setup.context=0;setup.read32=hci_read;setup.write32=hci_write;
 setup.sync=sync_dma;setup.now_us=now_us;setup.delay_us=delay_us;
 struct x9u_io io=setup;
 struct x9u_snapshot snapshot;x9u_snapshot(&io,&snapshot);state_log(&snapshot);
 if(rsr("sctlr_el1")&5) {plog_str("UFS stop: adapter requires MMU/cache-off entry\n");plog_flush();return X9U_UNSUPPORTED;}
 struct x9u_smmu_state smmu;
 int error=x9u_smmu_identity(0,0x63,&smmu,smmu_read,table_read);
 plog_hex("UFS SMMU IDR0",smmu.idr0);plog_hex("UFS SMMU CR0",smmu.cr0);
 plog_hex("UFS SMMU CR0ACK",smmu.cr0ack);plog_hex("UFS SMMU GBPA",smmu.gbpa);
 plog_hex("UFS SMMU STRTAB",smmu.strtab_base);plog_hex("UFS SMMU CFG",smmu.strtab_config);
 plog_hex("UFS SMMU STE0",smmu.ste0);plog_hex("UFS DMA check",(uint64_t)(int64_t)error);plog_flush();
 if(error||!smmu.identity) {
  plog_str("UFS stop: DMA identity unavailable; no queue programmed, no global SMMU changes\n");plog_flush();return X9U_DMA_UNVERIFIED;
 }
 if(!fdt_ram_available(fdt,(uintptr_t)dma_area,sizeof(dma_area))) {
  plog_str("UFS stop: DMA buffer outside unreserved current RAM\n");plog_flush();return X9U_BOUNDS;
 }
 plog_hex("UFS private DMA PA",(uintptr_t)dma_area);
 struct x9u_link link;
 plog_hex("UFS MTK IP version",hci_read(0,0x2240));
 plog_str("UFS wake: MT6993 host config + device RST_n + pre/link/post-link; media read-only\n");plog_flush();
 error=x9u_link_start_full(&io,&link,1,device_reset);link_log(&link,error);
 if(error) {
  int cleanup=x9u_link_stop(&io,&link);
  plog_hex("UFS wake cleanup",(uint64_t)(int64_t)cleanup);plog_flush();
  if(cleanup==X9U_DMA_PENDING)stuck_dma=1;
  return error;
 }
 error=x9u_open(&controller,&io,dma_area,(uintptr_t)dma_area,1);
 plog_hex("UFS retained open",(uint64_t)(int64_t)error);plog_flush();
 if(error) {
  if(controller.attached && x9u_close(&controller)==X9U_DMA_PENDING)stuck_dma=1;
  if(!stuck_dma) {
   int cleanup=x9u_link_stop(&io,&link);
   plog_hex("UFS wake cleanup",(uint64_t)(int64_t)cleanup);plog_flush();
   if(cleanup==X9U_DMA_PENDING)stuck_dma=1;
  }
  return error;
 }
 int found=0;unsigned device_stage=0;
 error=x9u_device_init(&controller,&device_stage);
 plog_hex("UFS device init stage",device_stage);plog_hex("UFS device init status",(uint64_t)(int64_t)error);
 if(error) {
  plog_hex("UFS device init OCS",controller.last_ocs);
  plog_hex("UFS device init response",be64(controller.last_response));
#if X9_UFS_ROOT
  root_status=error;
#endif
  goto finish;
 }
 plog_str("UFS NOP AND fDeviceInit VERIFIED\n");plog_flush();
#if X9_UFS_NATIVE
 struct x9u_hs_state hs;
 error=x9u_hs_enable(&io,&hs,refclk_notify);hs_log(&hs,error);
 if(error){root_status=error;goto finish;}
#endif
 /* The three user LUNs are captured; no boot WLUN, RPMB, reset or power CDB. */
 for(unsigned lun=0;lun<3&&!controller.poisoned;lun++) {
  uint8_t inquiry[37]={0};plog_hex("UFS probe LUN",lun);
  error=x9u_inquiry(&controller,lun,inquiry,sizeof(inquiry));
  plog_hex("UFS inquiry status",(uint64_t)(int64_t)error);
  if(error){plog_hex("UFS last OCS",controller.last_ocs);continue;}
  char identity[29];for(unsigned i=0;i<28;i++)identity[i]=(inquiry[8+i]>=32&&inquiry[8+i]<127)?inquiry[8+i]:'?';identity[28]=0;
  plog_str("UFS identity: ");plog_str(identity);plog_str("\n");
  error=x9u_lun_ready(&controller,lun);plog_hex("UFS LUN ready status",(uint64_t)(int64_t)error);
  plog_hex("UFS readiness attempts",controller.ready_attempts);
  plog_hex("UFS readiness Unit Attention count",controller.ready_unit_attentions);
  if(controller.ready_unit_attentions) {
   plog_hex("UFS readiness last sense category",controller.ready_last_category);
   plog_hex("UFS readiness last ASC",controller.ready_last_asc);plog_hex("UFS readiness last ASCQ",controller.ready_last_ascq);
  }
  if(error) {
   plog_hex("UFS TUR failure phase",controller.tur_failure_phase);
   plog_hex("UFS TUR OCS",controller.tur_ocs);
   plog_hex("UFS TUR header",be64(controller.tur_response));
   plog_hex("UFS TUR segment/residual",be64(controller.tur_response+8));
   plog_hex("UFS TUR sense length",(uint32_t)controller.tur_response[32]<<8|controller.tur_response[33]);
   plog_hex("UFS TUR sense prefix",be64(controller.tur_response+34));
   plog_hex("UFS SCSI sense valid",controller.sense_valid);
   plog_hex("UFS SCSI sense from UPIU",controller.sense_from_upiu);
   plog_hex("UFS SCSI sense category",controller.sense_key);
   plog_hex("UFS SCSI ASC",controller.sense_asc);plog_hex("UFS SCSI ASCQ",controller.sense_ascq);
   plog_hex("UFS REQUEST SENSE count",controller.sense_requests);
   plog_hex("UFS REQUEST SENSE status",(uint64_t)(int64_t)controller.sense_request_error);
#if X9_UFS_ROOT
   if(lun==X9_ROOT_LUN)root_status=error;
#endif
   plog_flush();continue;
  }
  error=x9u_capacity(&controller,lun);plog_hex("UFS capacity status",(uint64_t)(int64_t)error);
  if(error) {
   plog_hex("UFS capacity OCS",controller.last_ocs);
   plog_hex("UFS capacity response header",be64(controller.last_response));
   continue;
  }
  plog_hex("UFS logical block bytes",controller.block_size[lun]);plog_hex("UFS logical blocks",controller.blocks[lun]);
  struct x9u_partition partition;
  error=x9u_gpt_find(&controller,lun,"userdata",&partition);
  plog_hex("UFS userdata GPT status",(uint64_t)(int64_t)error);
  if(!error) {
   plog_hex("UFS userdata first LBA",partition.first);plog_hex("UFS userdata blocks",partition.count);
   found=1;plog_str("UFS READ-ONLY GPT VERIFIED\n");
#if X9_UFS_ROOT
#if X9_UFS_NATIVE
   if(lun==X9_ROOT_LUN) {
    root_status=native_root_verify(lun,&partition);root_valid=!root_status;
    plog_hex("UFS native header status",(uint64_t)(int64_t)root_status);
   }
#else
   if(lun==X9_ROOT_LUN&&root_destination) {
    volatile struct x9u_root_manifest setup_root;
    setup_root.bytes=X9_ROOT_BYTES;setup_root.partition_first=X9_ROOT_PART_FIRST;
    setup_root.partition_blocks=X9_ROOT_PART_BLOCKS;setup_root.lun=X9_ROOT_LUN;
    for(unsigned i=0;i<16;i++)setup_root.guid[i]=x9_root_guid[i];
    for(unsigned i=0;i<32;i++)setup_root.sha256[i]=x9_root_sha256[i];
    setup_root.extents=x9_root_extents;setup_root.extent_count=sizeof(x9_root_extents)/sizeof(x9_root_extents[0]);
    struct x9u_root_manifest manifest=setup_root;
    root_status=x9u_root_read(&controller,lun,&partition,&manifest,root_destination,root_capacity,root_progress);
    plog_hex("UFS ROOT verify status",(uint64_t)(int64_t)root_status);
    root_valid=!root_status;
    if(root_valid)plog_str("UFS ROOT SHA256 AND HFS VERIFIED: plaintext root loaded from pinned-file blocks\n");
    else plog_str("UFS ROOT REJECTED: image identity/read failure; no root handoff\n");
   }
#endif
#else
   int pin_result=pin_file_check(lun,&partition);
   x9d_clear(&decryption);
   volatile uint8_t *erase=pin_readback;
   for(unsigned i=0;i<sizeof(pin_readback);i++)erase[i]=0;
   plog_hex("UFS PIN snapshot check status",(uint64_t)(int64_t)pin_result);
#endif
  }
  plog_flush();
 }
finish:
 x9d_clear(&decryption);
 int closed=x9u_close(&controller);plog_hex("UFS restore original queue",(uint64_t)(int64_t)closed);
 if(closed==X9U_DMA_PENDING)stuck_dma=1;
 if(!closed) {
#if X9_UFS_NATIVE
  int cleanup=root_valid?x9u_link_handoff(&io,&link):x9u_link_stop(&io,&link);
  plog_hex("UFS native idle link handoff",(uint64_t)(int64_t)cleanup);
#else
  int cleanup=x9u_link_stop(&io,&link);
#endif
  plog_hex("UFS restore original HCE",(uint64_t)(int64_t)cleanup);
  if(cleanup)closed=cleanup;
  if(cleanup==X9U_DMA_PENDING)stuck_dma=1;
 }
 plog_str("UFS qualification finished; continuing only if DMA quiescent\n");plog_flush();
#if X9_UFS_ROOT
 (void)found;
 return closed?closed:root_status;
#else
 return closed?closed:found?0:X9U_NOT_FOUND;
#endif
}
int x9_ufs_dma_pending(void) {return stuck_dma;}
