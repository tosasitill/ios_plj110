/* X9 native extent-backed UFS root. Media-read-only first implementation.
 * Direct-LK shim initializes MT6993; kernel only attaches to a ready, idle,
 * coherent host with existing SMMU bypass. No platform reset/power sequences.
 * Source transport is shared with tested X9 shim, isolated from sweet_ufs.c.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <sys/kernel.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <arm64/sweet_early.h>
#include <libkern/libkern.h>
#include "x9_ufs/ufs_ro.h"
#include "x9_ufs/ufs_hs.h"
#include "x9_ufs/sha256.h"
extern ppnum_t pmap_find_phys(pmap_t,addr64_t);
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
#define X9_EXT_MAX 16384U
static struct {
 struct x9u_host host;
 struct x9u_partition partition;
 struct x9u_extent extents[X9_EXT_MAX];
 unsigned extent_count;
 uint64_t bytes,header_bytes;
 uint8_t header_sha[32],volume_uuid[8];
 vm_offset_t hci,smmu,wdt,dma;
 uint64_t dma_pa,frequency;
 uint64_t read_calls,read_bytes,read_us,report_at;
 int bmajor,cmajor;
 unsigned sector;
 dev_t device;
 bool ready,failed,writable,mutable;
 uint64_t write_bytes,write_calls,lock_wait_us;
 unsigned write_trace_count;
 uint64_t class_bytes[4][2], class_report_us;
} xu;
static LCK_GRP_DECLARE(x9ufs_grp,"x9_ufs");
static LCK_MTX_DECLARE(x9ufs_lock,&x9ufs_grp);
static uint8_t header_data[65536];
static void pet(void){if(xu.wdt)*(volatile uint32_t *)(xu.wdt+8)=0x1971;}
static uint32_t read32(void *ctx,unsigned off){(void)ctx;return *(volatile uint32_t *)(xu.hci+off);}
static void write32(void *ctx,unsigned off,uint32_t value)
{(void)ctx;*(volatile uint32_t *)(xu.hci+off)=value;__asm__ volatile("dsb sy":::"memory");}
static uint64_t counter(void){uint64_t ticks;__asm__ volatile("mrs %0,cntpct_el0":"=r"(ticks));return ticks;}
static uint64_t now_us(void *ctx)
{(void)ctx;uint64_t ticks=counter();return ticks/xu.frequency*1000000+(ticks%xu.frequency)*1000000/xu.frequency;}
static void delay_us(void *ctx,unsigned us)
{
 (void)ctx;uint64_t start=counter(),ticks=xu.frequency*us/1000000;
 pet();while(counter()-start<ticks)__asm__ volatile("yield");
}
static void sync_dma(void *ctx,unsigned off,size_t bytes,int cpu)
{
 (void)ctx;
 if(off>X9U_DMA_SIZE||bytes>X9U_DMA_SIZE-off)panic("X9 UFS DMA sync bounds");
 if(cpu)flush_dcache64(xu.dma_pa+off,(unsigned)bytes,TRUE);
 else clean_dcache64(xu.dma_pa+off,(unsigned)bytes,TRUE);
 __asm__ volatile("dsb sy":::"memory");
}
static int prop(DTEntry node,const char *key,void *out,unsigned length)
{
 const void *p=NULL;unsigned n=0;
 if(SecureDTGetProperty(node,key,&p,&n)!=kSuccess||n!=length)return EINVAL;
 memcpy(out,p,n);return 0;
}
static int manifest(void)
{
 DTEntry chosen;uint64_t abi[6],part[2];uint8_t guid[16];const void *runs=NULL;unsigned bytes=0;
 if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess||
    prop(chosen,"x9-ufs-native-ready",abi,sizeof(abi))||abi[0]!=1||abi[1]!=0x16810000||
    abi[2]!=0x5000||abi[3]!=0x14900000||abi[4]!=0x63||abi[5]!=1||
    prop(chosen,"x9-ufs-root-bytes",&xu.bytes,sizeof(xu.bytes))||!xu.bytes||(xu.bytes&4095)||
    xu.bytes>(64ULL<<30)||prop(chosen,"x9-ufs-root-partition",part,sizeof(part))||
    prop(chosen,"x9-ufs-root-guid",guid,sizeof(guid))||
    prop(chosen,"x9-ufs-root-header-sha256",xu.header_sha,32)||
    prop(chosen,"x9-ufs-root-header-bytes",&xu.header_bytes,8)||xu.header_bytes!=sizeof(header_data)||
    prop(chosen,"x9-ufs-root-volume-uuid",xu.volume_uuid,8)||
    SecureDTGetProperty(chosen,"x9-ufs-root-extents",&runs,&bytes)!=kSuccess||!bytes||
    bytes%sizeof(struct x9u_extent)||bytes/sizeof(struct x9u_extent)>X9_EXT_MAX)return EINVAL;
 xu.partition.first=part[0];xu.partition.count=part[1];memcpy(xu.partition.guid,guid,16);
 xu.extent_count=bytes/sizeof(struct x9u_extent);memcpy(xu.extents,runs,bytes);
 if(x9u_extent_validate(xu.extents,xu.extent_count,xu.bytes/4096,xu.partition.count))return EINVAL;
 return 0;
}
static int bopen(dev_t dev,int flags,int type,struct proc *p)
{(void)dev;(void)type;(void)p;return !xu.ready?ENXIO:((flags&FWRITE)&&!xu.writable)?EROFS:0;}
static int bclose(dev_t dev,int flags,int type,struct proc *p)
{(void)dev;(void)flags;(void)type;(void)p;return 0;}
static void strategy(buf_t bp)
{
 int error=0;caddr_t address=NULL;uint32_t count=buf_count(bp);bool trace_write=false;uint64_t traced_bytes=0;buf_setresid(bp,count);
 bool reading=buf_flags(bp)&B_READ;
 /* buf vnode is valid until biodone; VSWAP is authoritative, executor isn't.
  * Categories are exclusive: swap / other page IO / named vnode / no vnode. */
 vnode_t io_vnode=buf_vnode(bp);
 unsigned io_class=io_vnode && vnode_isswap(io_vnode)?0:
     (buf_flags(bp)&B_PAGEIO)?1:io_vnode?2:3;
 if(!reading&&!xu.writable){error=EROFS;goto done;}
 if(!xu.ready||xu.failed){error=ENXIO;goto done;}
 daddr64_t block=buf_blkno(bp);
 if(block<0||(uint64_t)block>UINT64_MAX/xu.sector){error=EINVAL;goto done;}
 uint64_t offset=(uint64_t)block*xu.sector;
 if(offset>xu.bytes||count>xu.bytes-offset){error=EINVAL;goto done;}
 if(!count)goto done;
 if(buf_map(bp,&address)){error=EIO;goto done;}
 uint64_t lock_start=now_us(NULL);
 lck_mtx_lock(&x9ufs_lock);xu.lock_wait_us+=now_us(NULL)-lock_start;
 pet();uint64_t started=now_us(NULL);
 int result=reading?x9u_image_read_mapped(&xu.host,2,&xu.partition,xu.extents,xu.extent_count,xu.bytes/4096,offset,address,count):
                    x9u_image_write(&xu.host,offset,address,count);
 if(!result)xu.class_bytes[io_class][reading?0:1]+=count;
 if(reading){xu.read_us+=now_us(NULL)-started;xu.read_calls++;if(!result)xu.read_bytes+=count;}
 else if(!result){xu.write_calls++;xu.write_bytes+=count;
  if(xu.write_trace_count<32 && xu.write_bytes>=((uint64_t)xu.write_trace_count+1)*(64ULL<<20)){xu.write_trace_count++;trace_write=true;traced_bytes=xu.write_bytes;}
 }
 /* One report after 4MiB, then no more than once per 64MiB. No per-read log. */
 if(xu.read_bytes>=xu.report_at) {
  printf("X9 UFS IO: bytes=%llu calls=%llu io_us=%llu KiB/s=%llu (includes DMA/CPU, excludes console)\n",
      xu.read_bytes,xu.read_calls,xu.read_us,xu.read_us?xu.read_bytes*1000000/xu.read_us/1024:0);
  printf("X9 UFS detail: cmds=%llu rd=%llu wr=%llu device_us=%llu command_us=%llu max_device_us=%llu lock_us=%llu\n",
    xu.host.io_commands,xu.host.io_read_bytes,xu.host.io_write_bytes,xu.host.io_device_us,
    xu.host.io_total_us,xu.host.io_max_device_us,xu.lock_wait_us);
  xu.report_at=xu.read_bytes+(64ULL<<20);
  uint64_t report_us=now_us(NULL);
  if(report_us-xu.class_report_us>=10000000) {
   xu.class_report_us=report_us;
   printf("X9 IO CLASS bytes: swap=%llu/%llu page=%llu/%llu vnode=%llu/%llu unknown=%llu/%llu (read/write, root only)\n",
    xu.class_bytes[0][0],xu.class_bytes[0][1],xu.class_bytes[1][0],xu.class_bytes[1][1],
    xu.class_bytes[2][0],xu.class_bytes[2][1],xu.class_bytes[3][0],xu.class_bytes[3][1]);
  }
 }
 if(result) {
  error=EIO;
  printf("X9 UFS %s failed status=%d offset=0x%llx count=%u OCS=%u phase=%u\n",reading?"read":"write",result,offset,count,xu.host.last_ocs,xu.host.failure_phase);
  if(xu.host.poisoned)xu.failed=true;
 }else buf_setresid(bp,0);
 lck_mtx_unlock(&x9ufs_lock);buf_unmap(bp);
 if(trace_write){
  const char *vname=io_vnode?vnode_getname(io_vnode):NULL;
  printf("X9 WRITE VNODE: class=%u flags=%08x name=%s offset=%llu count=%u\n",
    io_class,buf_flags(bp),vname?vname:"(none)",offset,count);
  if(vname)vnode_putname(vname);
  char exec[32]={0},ownername[32]={0};proc_t owner=buf_proc(bp);int ownerpid=owner?proc_pid(owner):-1;
  proc_selfname(exec,sizeof(exec));if(ownerpid>=0)proc_name(ownerpid,ownername,sizeof(ownername));
  printf("X9 WRITE TRACE root_bytes=%llu offset=%llu count=%u executor=%s executor_pid=%d buf_owner=%s buf_owner_pid=%d (executor may be a flusher)\n",traced_bytes,offset,count,exec,proc_selfpid(),ownerpid>=0?ownername:"unknown",ownerpid);
 }
 done:if(error)buf_seterror(bp,error);buf_biodone(bp);
}
static int ioctl_common(u_long command,caddr_t data,int character)
{
 uint32_t *u=(uint32_t *)data;uint64_t *q=(uint64_t *)data;
 switch(command) {
 case DKIOCGETBLOCKSIZE:*u=xu.sector;return 0;
 case DKIOCSETBLOCKSIZE:
  if(character)return ENODEV;
  if(*u<512||*u>4096||(*u&(*u-1)))return EINVAL;
  xu.sector=*u;return 0;
 case DKIOCGETPHYSICALBLOCKSIZE:*u=4096;return 0;
 case DKIOCGETBLOCKCOUNT:*q=xu.bytes/xu.sector;return 0;
 case DKIOCISWRITABLE:*u=xu.writable;return 0;
 /* MT6993 UFS is solid-state: HFS must not relocate files as for rotating disks. */
 case DKIOCISSOLIDSTATE:*u=1;return 0;
 case DKIOCSYNCHRONIZE:case DKIOCSYNCHRONIZECACHE: {
  if(!xu.writable)return 0;
  lck_mtx_lock(&x9ufs_lock);pet();int error=x9u_sync_image(&xu.host);
  if(xu.host.poisoned)xu.failed=true;
  lck_mtx_unlock(&x9ufs_lock);return error?EIO:0;
 }
 case DKIOCGETMAXBLOCKCOUNTREAD:*q=X9U_DATA_SIZE/xu.sector;return 0;
 case DKIOCGETMAXBLOCKCOUNTWRITE:*q=xu.writable?X9U_DATA_SIZE/xu.sector:0;return 0;
 case DKIOCGETMAXSEGMENTCOUNTREAD:case DKIOCGETMAXSEGMENTCOUNTWRITE:*q=1;return 0;
 default:return ENOTTY;
 }
}
static int bioctl(dev_t d,u_long c,caddr_t data,int f,struct proc *p)
{(void)d;(void)f;(void)p;return ioctl_common(c,data,0);}
static int cioctl(dev_t d,u_long c,caddr_t data,int f,struct proc *p)
{(void)d;(void)f;(void)p;return ioctl_common(c,data,1);}
static int psize(dev_t d){(void)d;return xu.bytes/xu.sector>INT_MAX?0:(int)(xu.bytes/xu.sector);}
static int rawread(dev_t d,struct uio *u,int f)
{(void)f;return physio(strategy,NULL,d,B_READ,minphys,u,(int)xu.sector);}
static int rawwrite(dev_t d,struct uio *u,int f)
{(void)f;if(!xu.writable)return EROFS;return physio(strategy,NULL,d,B_WRITE,minphys,u,(int)xu.sector);}
static const struct bdevsw bdev={.d_open=bopen,.d_close=bclose,.d_strategy=strategy,.d_ioctl=bioctl,.d_dump=eno_dump,.d_psize=psize,.d_type=D_DISK};
static const struct cdevsw cdev={.d_open=bopen,.d_close=bclose,.d_read=rawread,.d_write=rawwrite,.d_ioctl=cioctl,.d_stop=eno_stop,.d_reset=eno_reset,.d_ttys=NULL,.d_select=eno_select,.d_mmap=eno_mmap,.d_strategy=eno_strat,.d_reserved_1=eno_getc,.d_reserved_2=eno_putc,.d_type=D_DISK};
/* Two gates: explicit ufsrw and the new-file ADT contract. Check every
 * reservation before granting any media write. Old manifests have no gate. */
static int enable_writes(void)
{
 uint32_t requested=0;PE_parse_boot_argn("ufsrw",&requested,sizeof(requested));
 if(!requested)return 0;
 DTEntry chosen;uint64_t contract[2];const void *raw=NULL;unsigned bytes=0;
 if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess||
    prop(chosen,"x9-ufs-root-write-contract",contract,sizeof(contract))||
    contract[0]!=1||contract[1]!=xu.bytes||
    SecureDTGetProperty(chosen,"x9-ufs-protected-extents",&raw,&bytes)!=kSuccess||
    !bytes||bytes%16||bytes/16>32768)return EPERM;
 const uint8_t *entries=raw;
 for(unsigned i=0;i<bytes/16;i++) {
  uint64_t p,n;memcpy(&p,entries+i*16,8);memcpy(&n,entries+i*16+8,8);
  if(!n||p>=xu.partition.count||n>xu.partition.count-p)return EPERM;
  for(unsigned j=0;j<xu.extent_count;j++) {
   const struct x9u_extent *e=&xu.extents[j];
   if(e->part<p+n&&p<e->part+e->count)return EPERM;
  }
 }
 int error=x9u_bind_extent_writes(&xu.host,2,&xu.partition,xu.extents,xu.extent_count,xu.bytes/4096);
 if(error)return EPERM;
 xu.writable=true;return 0;
}
/* X9 dedicated disk log: source ring copied by a sleepable thread, never from
 * console/IRQ context. Separate preallocated file, same serialized idle host.
 * Does not reset, reboot, mount filesystems or write outside its bound map. */
#define X9_LOG_EXT_MAX 16U
static struct {
 struct x9u_host host;
 struct x9u_extent extents[X9_LOG_EXT_MAX];
 unsigned extent_count;
 uint64_t bytes,first,total,seq;
 bool ready,started;
 int channel;
 uint8_t data[65536],header[4096];
} xl;
extern uint32_t sweet_log_copy(uint64_t *,char *,uint32_t);
static void log_put64(uint8_t *p,uint64_t x)
{for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(x>>(i*8));}
static void log_put32(uint8_t *p,uint32_t x)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(i*8));}
static int log_flush_locked(void)
{
 if(!xl.ready||!xu.ready||xu.failed)return 0;
 uint64_t capacity=xl.bytes-4096;bool changed=false;int error=0;
 for(unsigned round=0;round<64;round++) {
  uint64_t next=xl.total;
  uint32_t n=sweet_log_copy(&next,(char *)xl.data,sizeof(xl.data));
  if(!n)break;
  uint64_t start=next-n;
  if(start!=xl.total)xl.first=start;
  size_t first=(size_t)(capacity-start%capacity);if(first>n)first=n;
  error=x9u_image_write(&xl.host,4096+start%capacity,xl.data,first);
  if(!error&&first<n)error=x9u_image_write(&xl.host,4096,xl.data+first,n-first);
  if(error)break;
  xl.total=next;if(xl.total-xl.first>capacity)xl.first=xl.total-capacity;
  changed=true;
 }
 if(!error&&changed) {
  /* Publish a new cursor only after payload reaches the device. */
  error=x9u_sync_image(&xl.host);
  if(!error) {
   bzero(xl.header,sizeof(xl.header));memcpy(xl.header,"X9UFSLOG",8);
   log_put32(xl.header+8,1);log_put32(xl.header+12,4096);
   log_put64(xl.header+16,capacity);log_put64(xl.header+24,xl.first);
   log_put64(xl.header+32,xl.total);log_put64(xl.header+40,++xl.seq);
   log_put32(xl.header+48,x9u_crc32(0,xl.header,48));
   error=x9u_image_write(&xl.host,0,xl.header,sizeof(xl.header));
   if(!error)error=x9u_sync_image(&xl.host);
  }
 }
 if(error) {
  xl.ready=false;
  if(xl.host.poisoned){xu.host.poisoned=1;xu.failed=true;}
 }
 return error;
}
void sweet_x9_ufs_log_flush(void)
{
 if(!xl.ready)return;
 lck_mtx_lock(&x9ufs_lock);pet();int error=log_flush_locked();lck_mtx_unlock(&x9ufs_lock);
 if(error)printf("X9 disk log: stopped errno=%d OCS=%u phase=%u\n",error,xl.host.last_ocs,xl.host.failure_phase);
}
static void log_thread(void *argument,wait_result_t result)
{
 (void)argument;(void)result;
 while(xl.ready) {
  sweet_x9_ufs_log_flush();
  (void)tsleep(&xl.channel,PZERO,"x9disklog",hz*2);
 }
}
static int log_prepare(void)
{
 DTEntry chosen;uint64_t bytes=0;const void *raw=NULL;unsigned n=0;
 if(!xu.writable||SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess||
    prop(chosen,"x9-ufs-log-bytes",&bytes,8))return 0; /* Sidecar is opt-in. */
 if(bytes<(128U<<10)||bytes>(16U<<20)||(bytes&4095)||
    SecureDTGetProperty(chosen,"x9-ufs-log-extents",&raw,&n)!=kSuccess||
    !n||n%sizeof(struct x9u_extent)||n/sizeof(struct x9u_extent)>X9_LOG_EXT_MAX)return EINVAL;
 xl.bytes=bytes;xl.extent_count=n/sizeof(struct x9u_extent);memcpy(xl.extents,raw,n);
 if(x9u_extent_validate(xl.extents,xl.extent_count,bytes/4096,xu.partition.count))return EINVAL;
 for(unsigned i=0;i<xl.extent_count;i++) {
  const struct x9u_extent *a=&xl.extents[i];
  for(unsigned j=0;j<xu.extent_count;j++) {
   const struct x9u_extent *b=&xu.extents[j];
   if(a->part<b->part+b->count&&b->part<a->part+a->count)return EPERM;
  }
 }
 const void *protected=NULL;unsigned protected_bytes=0;
 if(SecureDTGetProperty(chosen,"x9-ufs-protected-extents",&protected,&protected_bytes)!=kSuccess||
    !protected_bytes||protected_bytes%16||protected_bytes/16>32768)return EINVAL;
 for(unsigned j=0;j<protected_bytes/16;j++) {
  uint64_t p,count;memcpy(&p,(const uint8_t *)protected+j*16,8);memcpy(&count,(const uint8_t *)protected+j*16+8,8);
  if(!count||p>=xu.partition.count||count>xu.partition.count-p)return EPERM;
  for(unsigned i=0;i<xl.extent_count;i++) {
   const struct x9u_extent *a=&xl.extents[i];
   /* Root's exclusion list contains the exact sidecar extents as well. */
   if(p==a->part&&count==a->count)continue;
   if(a->part<p+count&&p<a->part+a->count)return EPERM;
  }
 }
 /* Copy transport bookkeeping, not controller ownership. Both views use the
  * same DMA aperture and mutex. No x9u_open/close/list programming is done. */
 xl.host=xu.host;xl.host.write_enabled=0;
 int error=x9u_bind_extent_writes(&xl.host,2,&xu.partition,xl.extents,xl.extent_count,bytes/4096);
 if(error)return EINVAL;
 xl.ready=true;return 0;
}
static void log_start(void)
{
 if(!xl.ready||xl.started)return;
 xl.started=true;
 printf("X9 disk log: dedicated %llu KiB, %u extents, flush every2s, no automatic reset\n",xl.bytes/1024,xl.extent_count);
 /* Capture the early boot ring before root mounting can fail. */
 sweet_x9_ufs_log_flush();
 thread_t thread;
 if(kernel_thread_start(log_thread,NULL,&thread)==KERN_SUCCESS)thread_deallocate(thread);
 else printf("X9 disk log: periodic thread unavailable; explicit flush remains enabled\n");
}

static int initialize(void)
{
 if(!sweet_early_is_x9()||manifest())return ENXIO;
 __asm__ volatile("mrs %0,cntfrq_el0":"=r"(xu.frequency));if(!xu.frequency)return ENXIO;
 xu.hci=ml_io_map(0x16810000,0x5000);xu.smmu=ml_io_map(0x14900000,0x1000);xu.wdt=ml_io_map(0x1c010000,PAGE_SIZE);
 if(!xu.hci||!xu.smmu||!xu.wdt)return ENOMEM;
 uint32_t cr0=*(volatile uint32_t *)(xu.smmu+0x20),ack=*(volatile uint32_t *)(xu.smmu+0x24),gbpa=*(volatile uint32_t *)(xu.smmu+0x44);
 if((cr0&1)||(ack&1)||(gbpa&((1U<<31)|(1U<<20)))){printf("X9 UFS: DMA bypass changed cr0=%x ack=%x gbpa=%x\n",cr0,ack,gbpa);return ENXIO;}
 if((read32(0,0x34)&1)==0||(read32(0,0x30)&3)!=3||read32(0,0x58)||read32(0,0x78)||read32(0,0x60)&1){printf("X9 UFS: no ready idle shim handoff\n");return ENXIO;}
 if((read32(0,0x2308)&3)!=3)return ENXIO;
 if(kmem_alloc_contig(kernel_map,&xu.dma,X9U_DMA_SIZE,PAGE_MASK,0,0,KMA_DATA|KMA_ZERO|KMA_PERMANENT,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return ENOMEM;
 ppnum_t first=pmap_find_phys(kernel_pmap,(addr64_t)xu.dma);if(!first)return ENOMEM;
 xu.dma_pa=ptoa_64(first);
 for(unsigned off=0;off<X9U_DMA_SIZE;off+=PAGE_SIZE)
  if(ptoa_64(pmap_find_phys(kernel_pmap,(addr64_t)(xu.dma+off)))!=xu.dma_pa+off)return ENOMEM;
 struct x9u_io io={NULL,read32,write32,sync_dma,now_us,delay_us};
 int result=x9u_open(&xu.host,&io,(void *)xu.dma,xu.dma_pa,1);
 if(result){printf("X9 UFS: native attach failed %d\n",result);return ENXIO;}
 int require_hs=0;
 if(PE_parse_boot_argn("x9_ufs_hs",&require_hs,sizeof(require_hs))&&require_hs==1){
  struct x9u_hs_state hs={0};uint32_t pa[6]={0};
  int mode_error=x9u_hs_read(&io,&hs,pa);
  printf("X9 UFS LINK: status=%d mode=0x%x TX_G%u RX_G%u TX_lanes=%u RX_lanes=%u series=%u\n",
    mode_error,pa[0],pa[1],pa[2],pa[3],pa[4],pa[5]);
  if(mode_error||!x9u_hs_valid(pa))return ENXIO;
 }
 struct x9u_partition expected=xu.partition,actual;
 if(x9u_capacity(&xu.host,2)||x9u_gpt_find(&xu.host,2,"userdata",&actual)||actual.first!=expected.first||actual.count!=expected.count||memcmp(actual.guid,expected.guid,16))return ENXIO;
 xu.partition=actual;
 if(x9u_image_read(&xu.host,2,&xu.partition,xu.extents,xu.extent_count,xu.bytes/4096,0,header_data,sizeof(header_data)))return EIO;
 struct sha256 hash;uint8_t digest[32];sha256_init(&hash);sha256_update(&hash,header_data,sizeof(header_data));sha256_final(&hash,digest);
 DTEntry chosen;const void *write_contract=NULL;unsigned contract_bytes=0;
 if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess)return EINVAL;
 if(SecureDTGetProperty(chosen,"x9-ufs-root-write-contract",&write_contract,&contract_bytes)==kSuccess) {
  uint64_t contract[2];if(contract_bytes!=sizeof(contract))return EINVAL;
  memcpy(contract,write_contract,sizeof(contract));
  if(contract[0]!=1||contract[1]!=xu.bytes)return EINVAL;
  xu.mutable=true;
 }
 if((!xu.mutable&&memcmp(digest,xu.header_sha,32))||memcmp(header_data+1128,xu.volume_uuid,8))return EINVAL;
 uint32_t block=(uint32_t)header_data[1064]<<24|(uint32_t)header_data[1065]<<16|(uint32_t)header_data[1066]<<8|header_data[1067];
 uint32_t blocks=(uint32_t)header_data[1068]<<24|(uint32_t)header_data[1069]<<16|(uint32_t)header_data[1070]<<8|header_data[1071];
 if(header_data[1024]!='H'||(header_data[1025]!='X'&&header_data[1025]!='+')||(uint64_t)block*blocks!=xu.bytes)return EINVAL;
 int write_error=enable_writes();if(write_error){printf("X9 UFS: write contract rejected errno=%d\n",write_error);return write_error;}
 xu.sector=512;xu.bmajor=bdevsw_add(-1,&bdev);if(xu.bmajor<0)return ENXIO;
 xu.cmajor=cdevsw_add_with_bdev(-1,&cdev,xu.bmajor);if(xu.cmajor<0)return ENXIO;
 xu.device=makedev(xu.bmajor,0);xu.report_at=4ULL<<20;xu.ready=true;
 int log_error=log_prepare();if(log_error){printf("X9 disk log contract rejected errno=%d\n",log_error);xu.ready=false;return log_error;}
 devfs_make_node(xu.device,DEVFS_BLOCK,UID_ROOT,GID_OPERATOR,xu.writable?0640:0440,"ufsroot0");
 devfs_make_node(makedev(xu.cmajor,0),DEVFS_CHAR,UID_ROOT,GID_OPERATOR,xu.writable?0640:0440,"rufsroot0");
 printf("X9 UFS NATIVE ROOT: LUN2, %llu MiB, %u extents, 512-byte sectors, %s; DMA=0x%llx\n",xu.bytes>>20,xu.extent_count,xu.writable?"read-write (dedicated extent copy)":"read-only",xu.dma_pa);
 /* The legacy sweet_ufs_init path exits early on X9. Start input here,
  * once the native root has completed initialization and threads are usable. */
 extern void sweet_x9_touch_init(void);
 sweet_x9_touch_init();
 return 0;
}
/* Read-only query; never initializes hardware from an exec path. */
boolean_t sweet_x9_ufs_root_writable(void)
{
 return xu.ready && xu.writable && !xu.failed;
}
boolean_t sweet_x9_ufs_root_active(void)
{
 return xu.ready && !xu.failed;
}
int sweet_x9_ufs_rootdev(dev_t *device)
{
 if(xu.failed)return ENXIO;
 if(!xu.ready) {
  int error=initialize();
  if(error) {
   xu.failed=true;
   int stopped=x9u_close(&xu.host);
   printf("X9 UFS native root init failed errno=%d OCS=%u phase=%u cleanup=%d; DMA allocation retained\n",error,xu.host.last_ocs,xu.host.failure_phase,stopped);
   return error;
  }
 }
 log_start();
 *device=xu.device;return 0;
}
#endif

/* Dedicated mailbox slices are read-only to XNU. Host writes are restricted to
 * pinned extents and occur only while Android owns the controller. */
#include "x9_update_map.h"
static bool x9_update_map_ready;
int sweet_x9_update_read(uint64_t offset, void *data, size_t length)
{
 if(!xu.ready || xu.failed || !data || offset>X9_UPDATE_BLOCKS*4096 ||
    length>X9_UPDATE_BLOCKS*4096-offset)return EINVAL;
 lck_mtx_lock(&x9ufs_lock);
 int error=0;
 if(!x9_update_map_ready) {
 error=x9u_extent_validate(x9_update_extents,sizeof(x9_update_extents)/sizeof(x9_update_extents[0]),X9_UPDATE_BLOCKS,xu.partition.count);
 DTEntry chosen;const void *raw=NULL;unsigned bytes=0;
 if(!error && (SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess ||
    SecureDTGetProperty(chosen,"x9-ufs-protected-extents",&raw,&bytes)!=kSuccess || bytes%16))error=EINVAL;
 for(unsigned i=0;!error && i<sizeof(x9_update_extents)/sizeof(x9_update_extents[0]);i++) {
  const struct x9u_extent *a=&x9_update_extents[i];bool reserved=false;
  for(unsigned j=0;j<xu.extent_count;j++) {
   const struct x9u_extent *b=&xu.extents[j];
   if(a->part<b->part+b->count && b->part<a->part+a->count){error=EPERM;break;}
  }
  for(unsigned j=0;!error && j<bytes/16;j++) {
   uint64_t p,n;memcpy(&p,(const uint8_t*)raw+j*16,8);memcpy(&n,(const uint8_t*)raw+j*16+8,8);
   if(p==a->part && n==a->count){reserved=true;continue;}
   if(a->part<p+n && p<a->part+a->count){error=EPERM;break;}
  }
  if(!reserved)error=EPERM;
 }
 if(!error)x9_update_map_ready=true;
 }
 if(!error){pet();error=x9u_image_read_mapped(&xu.host,2,&xu.partition,x9_update_extents,sizeof(x9_update_extents)/sizeof(x9_update_extents[0]),X9_UPDATE_BLOCKS,offset,data,length);}
 if(xu.host.poisoned)xu.failed=true;
 lck_mtx_unlock(&x9ufs_lock);
 return error?EIO:0;
}
