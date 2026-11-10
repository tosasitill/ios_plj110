/* Original v062: rodata13df4 NVEMI568000/c0000, 21 blocks140a4,
 * host hdr init343a8 capacity=block-100; shared take324c0, give326e0,
 * write32ab4/read32ec4/info33224. No old D00000 layout or NVLOCK ACK.
 * Vendor unchecked arithmetic, magic-only acceptance, lazy header repair,
 * sparse stale bytes, serial wrap and post-take early-return lock leaks
 * are NOT ported. Device mapping + DSB orders the two-sided protocol;
 * without the supplier's actual retained authority, no NV load/store.
 * No default caller/supplier, persistence, POST_ON or application ACK. */
static const struct { uint32_t offset,bytes; } gps_nv_blocks[21]={
 {0x00000,51200},{0x0c800,6144},{0x0e000,6144},{0x0f800,6144},
 {0x11000,131072},{0x31000,131072},{0x51000,10240},{0x53800,10240},
 {0x56000,1024},{0x56400,1024},{0x56800,20480},{0x5b800,1024},
 {0x5bc00,1024},{0x5c000,1024},{0x5c400,1024},{0x5c800,1024},
 {0x5cc00,1024},{0x5d000,1024},{0x5d400,32768},{0x65400,81920},
 {0x79400,24576}
};
enum { NV_CAP=0,NV_ID=1,NV_ATTR=2,NV_MAGIC=3,NV_OCCUPIED=4,
 NV_VERSION=5,NV_SIZE=6,NV_READS=7,NV_WRITES=8,NV_FLAGS=9,
 NV_SUB_WORDS=12,NV_DATA_BYTES=96,NV_HEADER_BYTES=100 };
#define GPS_NV_MAGIC 0x12345678u
static struct {
 struct x9_gps_nv_ops ops;
 volatile uint32_t *words;
 volatile uint8_t *p;
 uint64_t physical,checks,reads,writes,invalidations,contentions,remote_events;
 uint32_t started,prepared,busy,claimed,dirty,headers,remote_mask,last_id;
 uint32_t last_version,last_size;
 int error;
} gps_nv;
static void gps_nv_barrier(void){__asm__ volatile("dsb sy" ::: "memory");}
static void gps_nv_journal(int error)
{
 if(error && !gps_nv.error)gps_nv.error=error;
 lck_mtx_lock(&gps_lock);
 st.nv_started=gps_nv.started;st.nv_prepared=gps_nv.prepared;
 st.nv_dirty=gps_nv.dirty;st.nv_headers=gps_nv.headers;
 st.nv_claimed=gps_nv.claimed;st.nv_remote_mask=gps_nv.remote_mask;
 st.nv_last_id=gps_nv.last_id;st.nv_last_version=gps_nv.last_version;
 st.nv_last_size=gps_nv.last_size;st.nv_checks=gps_nv.checks;
 st.nv_reads=gps_nv.reads;st.nv_writes=gps_nv.writes;
 st.nv_invalidations=gps_nv.invalidations;st.nv_contentions=gps_nv.contentions;
 st.nv_remote_events=gps_nv.remote_events;st.nv_errno=gps_nv.error;
 lck_mtx_unlock(&gps_lock);
}
static int gps_nv_owned_check(int cold)
{
 if(gps_nv.error)return gps_nv.error;
 int e=gps_mcu_guard();if(e)return e;
 if(!gps_nv.ops.check_owned || !gps_rt.lease)return ENXIO;
 if(gps_nv.checks==UINT64_MAX)return EOVERFLOW;
 gps_nv.checks++;
 e=gps_nv.ops.check_owned(gps_nv.ops.context,gps_rt.generation,gps_rt.lease,
     gps_nv.physical,X9_GPS_NV_BYTES,cold);
 if(e)return e;
 /* A callback must not change epochs or reset behind the worker. */
 e=gps_mcu_guard();if(e)return e;
 return gps_mcu_expect(F_reset,cold ? 0 : (gps_rt.booted ? 1 : 0));
}
static int gps_nv_owned(int cold)
{
 int e=gps_nv_owned_check(cold);
 if(e) {
  /* Unknown ownership cannot be retried by a give/recheck which happens
   * to work later. Retain any claim and revoke output immediately. */
  gps_nv_journal(e);x9_gps_hardware_fault(gps_rt.generation,e);
 }
 return e;
}
int x9_gps_nv_prepare(uint64_t generation,const struct x9_gps_nv_ops *ops)
{
 if(current_thread()!=gps_rt.worker || !gps_rt.in_provision ||
     generation!=gps_rt.generation)return EPERM;
 if(!ops || !ops->check_owned)return EINVAL;
 if(gps_nv.started)return gps_nv.error ? gps_nv.error : EBUSY;
 if(!gps_rt.held_reset || !gps_rt.remapped || gps_rt.booted ||
     gps_rt.hif.prepared)return ESTALE;
 const struct x9_gps_handoff *h=&gps_rt.layout;
 if(h->ap2mcu_offset!=0x628000 || h->mcu2ap_offset!=0x62c000 ||
     h->region_size!=0x4000 || h->reserve_size<X9_GPS_NV_OFFSET+X9_GPS_NV_BYTES ||
     (uint64_t)h->reserve_base+X9_GPS_NV_OFFSET+X9_GPS_NV_BYTES>0x100000000ULL)
  return ENOTSUP;
 gps_nv.ops=*ops;gps_nv.physical=(uint64_t)h->reserve_base+X9_GPS_NV_OFFSET;
 gps_nv.last_id=UINT32_MAX;gps_nv.started=1;
 int e=gps_nv_owned(1);if(e){gps_nv_journal(e);return e;}
 gps_nv.words=(volatile uint32_t *)ml_io_map(gps_nv.physical,X9_GPS_NV_BYTES);
 if(!gps_nv.words){gps_nv_journal(ENOMEM);return ENOMEM;}
 gps_nv.p=(volatile uint8_t *)gps_nv.words;
 /* Original cold clear28fcc, but never reachable via an RX getter. */
 for(unsigned off=0;off<X9_GPS_NV_BYTES;off+=256) {
  e=gps_nv_owned(1);if(e)break;
  gps_nv.dirty=1;
  for(unsigned i=0;i<256;i+=4)gps_nv.words[(off+i)/4]=0;
  gps_nv_barrier();
  e=gps_nv_owned(1);if(e)break;
  for(unsigned i=0;i<256;i+=4)if(gps_nv.words[(off+i)/4]){e=EIO;break;}
  if(e)break;
 }
 for(unsigned id=0;!e && id<X9_GPS_NV_BLOCKS;id++) {
  e=gps_nv_owned(1);if(e)break;
  volatile uint32_t *host=gps_nv.words+gps_nv_blocks[id].offset/4;
  host[NV_CAP]=gps_nv_blocks[id].bytes-NV_HEADER_BYTES;host[NV_ID]=id;
  gps_nv_barrier();host[NV_MAGIC]=GPS_NV_MAGIC;gps_nv_barrier();
  e=gps_nv_owned(1);if(e)break;
  for(unsigned i=0;i<NV_SUB_WORDS*2;i++) {
   uint32_t expected=i==NV_CAP ? gps_nv_blocks[id].bytes-NV_HEADER_BYTES :
       i==NV_ID ? id : i==NV_MAGIC ? GPS_NV_MAGIC : 0;
   if(host[i]!=expected){e=EIO;break;}
  }
  if(e)break;
  gps_nv.headers|=1u<<id;
 }
 if(!e)e=gps_nv_owned(1);
 if(!e)gps_nv.prepared=1;
 gps_nv_journal(e);return e;
}
static int gps_nv_enter(uint64_t generation,unsigned id)
{
 if(current_thread()!=gps_rt.worker ||
     (!gps_rt.in_callback && !gps_rt.in_provision))return EPERM;
 if(generation!=gps_rt.generation)return ESTALE;
 if(id>=X9_GPS_NV_BLOCKS)return EINVAL;
 if(gps_nv.error)return gps_nv.error;
 if(!gps_nv.prepared)return ENXIO;
 if(gps_nv.busy)return EBUSY;
 gps_nv.busy=1;gps_nv.last_id=id;return 0;
}
static volatile uint32_t *gps_nv_host(unsigned id)
{return gps_nv.words+gps_nv_blocks[id].offset/4;}
/* Called only with owned() already proving this complete interval safe.
 * Validate BOTH headers, including capacity/ID/size, not just magic. */
static int gps_nv_headers(unsigned id)
{
 volatile uint32_t *h=gps_nv_host(id),*t=h+NV_SUB_WORDS;
 uint32_t cap=gps_nv_blocks[id].bytes-NV_HEADER_BYTES;
 gps_nv_barrier();
 if(h[NV_MAGIC]!=GPS_NV_MAGIC || t[NV_MAGIC]!=GPS_NV_MAGIC)return EPROTO;
 if(h[NV_ID]!=id || t[NV_ID]!=id || h[NV_CAP]!=cap || t[NV_CAP]!=cap ||
     h[NV_SIZE]>cap || t[NV_SIZE]>cap || h[NV_OCCUPIED]>1 ||
     t[NV_OCCUPIED]>1)return EPROTO;
 return 0;
}
static int gps_nv_take(unsigned id)
{
 int e=gps_nv_owned(!gps_rt.booted);if(e)return e;
 volatile uint32_t *h=gps_nv_host(id),*t=h+NV_SUB_WORDS;
 uint32_t cap=gps_nv_blocks[id].bytes-NV_HEADER_BYTES;
 gps_nv_barrier();
 if(h[NV_MAGIC]!=GPS_NV_MAGIC || h[NV_ID]!=id || h[NV_CAP]!=cap ||
     h[NV_SIZE]>cap)return EPROTO;
 if(h[NV_OCCUPIED])return ESTALE; /* never steal a prior AP claim */
 /* Until MCU publishes its real magic there is no peer/header lease.
  * Zero is not a made-up target init. Nonzero bad magic is a fault. */
 if(!t[NV_MAGIC])return EAGAIN;
 if(t[NV_MAGIC]!=GPS_NV_MAGIC || t[NV_OCCUPIED]>1)return EPROTO;
 if(t[NV_OCCUPIED])return EAGAIN; /* don't inspect live changing metadata */
 h[NV_OCCUPIED]=1;gps_nv.claimed=1u<<id;gps_nv_barrier();
 /* Authority loss after store retains our claim/mapping, not blind give. */
 e=gps_nv_owned(!gps_rt.booted);if(e)return e;
 if(t[NV_OCCUPIED])return EAGAIN;
 e=gps_nv_headers(id);if(e)return e;
 if(h[NV_OCCUPIED]!=1)return ESTALE;
 return 0;
}
static int gps_nv_leave(unsigned id,int error)
{
 if(gps_nv.claimed) {
  int e=gps_nv_owned(!gps_rt.booted);
  if(!e) {
   volatile uint32_t *h=gps_nv_host(id);
   if(h[NV_OCCUPIED]!=1)e=ESTALE;
   else {
    gps_nv_barrier();h[NV_OCCUPIED]=0;gps_nv_barrier();
    if(h[NV_OCCUPIED])e=EIO;else gps_nv.claimed=0;
   }
  }
  if(e)error=e; /* retained claim on unknown authority */
 }
 gps_nv.busy=0;
 if(error==EAGAIN) {
  if(gps_nv.contentions==UINT64_MAX)error=EOVERFLOW;
  else gps_nv.contentions++;
 }
 /* Bad request/no peer header yet/ordinary contention aren't hardware
  * faults. Bad nonzero metadata is unsafe in ANY phase, never repair. */
 gps_nv_journal(error==EAGAIN || error==EINVAL ? 0 : error);
 if(gps_nv.error)x9_gps_hardware_fault(gps_rt.generation,gps_nv.error);
 return error;
}
static void gps_nv_latest(unsigned id,uint32_t *serial,uint32_t *size)
{
 volatile uint32_t *h=gps_nv_host(id),*t=h+NV_SUB_WORDS;
 if(t[NV_VERSION]>h[NV_VERSION]){*serial=t[NV_VERSION];*size=t[NV_SIZE];}
 else {*serial=h[NV_VERSION];*size=h[NV_SIZE];}
}
static int gps_nv_buffer(const void *p,unsigned bytes)
{
 uintptr_t start=(uintptr_t)p,end;
 if(!p || !bytes || bytes>X9_GPS_NV_IO_MAX || start>UINTPTR_MAX-bytes)return EINVAL;
 end=start+bytes;
 if(start<(uintptr_t)gps_nv.p+X9_GPS_NV_BYTES && end>(uintptr_t)gps_nv.p)return EINVAL;
 return 0;
}
/* Stable caller buffers cannot alias either header or shared payload.
 * Revalidate real authority every256B; counters/headers publish only once
 * the full copy completed under the claim. No cache/ACK-based ownership. */
static int gps_nv_copy(unsigned id,unsigned offset,void *destination,
    const void *source,unsigned bytes)
{
 volatile uint8_t *p=gps_nv.p+gps_nv_blocks[id].offset+NV_DATA_BYTES+offset;
 uint8_t *dst=destination;const uint8_t *src=source;
 for(unsigned done=0;done<bytes;) {
  int e=gps_nv_owned(!gps_rt.booted);if(e)return e;
  unsigned n=bytes-done;if(n>256)n=256;
  for(unsigned i=0;i<n;i++)if(dst)dst[done+i]=p[done+i];
      else p[done+i]=src ? src[done+i] : 0;
  gps_nv_barrier();
  if(!dst) {
   e=gps_nv_owned(!gps_rt.booted);if(e)return e;
   for(unsigned i=0;i<n;i++)if(p[done+i]!=(src ? src[done+i] : 0))return EIO;
  }
  done+=n;
 }
 return gps_nv_owned(!gps_rt.booted);
}
static int gps_nv_sync_host(unsigned id,uint32_t serial,uint32_t size)
{
 volatile uint32_t *h=gps_nv_host(id);
 h[NV_SIZE]=size;gps_nv_barrier();h[NV_VERSION]=serial;gps_nv_barrier();
 int e=gps_nv_owned(!gps_rt.booted);if(e)return e;
 if(h[NV_SIZE]!=size || h[NV_VERSION]!=serial)return EIO;
 gps_nv.last_version=serial;gps_nv.last_size=size;
 return 0;
}
int x9_gps_nv_get_info(uint64_t generation,unsigned id,struct x9_gps_nv_info *out)
{
 if(gps_nv_buffer(out,sizeof(*out)))return EINVAL;
 int e=gps_nv_enter(generation,id);if(e)return e;
 e=gps_nv_take(id);struct x9_gps_nv_info info;
 if(!e) {
  volatile uint32_t *h=gps_nv_host(id),*t=h+NV_SUB_WORDS;
  gps_nv_latest(id,&info.version,&info.size);info.capacity=h[NV_CAP];
  info.host_flags=h[NV_FLAGS];info.target_flags=t[NV_FLAGS];
  e=gps_nv_sync_host(id,info.version,info.size);
 }
 e=gps_nv_leave(id,e);if(!e)*out=info;return e;
}
int x9_gps_nv_read(uint64_t generation,unsigned id,unsigned offset,
    void *buffer,unsigned bytes,unsigned *read_bytes)
{
 if(gps_nv_buffer(read_bytes,sizeof(*read_bytes)) || gps_nv_buffer(buffer,bytes))return EINVAL;
 int e=gps_nv_enter(generation,id);if(e)return e;
 e=gps_nv_take(id);unsigned n=0;uint32_t serial=0,size=0;
 if(!e) {
  gps_nv_latest(id,&serial,&size);n=offset<size ? size-offset : 0;if(n>bytes)n=bytes;
  volatile uint32_t *h=gps_nv_host(id);
  if(n && (h[NV_READS]==UINT32_MAX || gps_nv.reads==UINT64_MAX))e=EOVERFLOW;
  if(!e && n)e=gps_nv_copy(id,offset,buffer,NULL,n);
  if(!e) {
   e=gps_nv_sync_host(id,serial,size);
   if(!e && n){h[NV_READS]++;gps_nv.reads++;}
  }
 }
 e=gps_nv_leave(id,e);if(!e)*read_bytes=n;return e;
}
int x9_gps_nv_write(uint64_t generation,unsigned id,unsigned offset,
    const void *buffer,unsigned bytes)
{
 if(gps_nv_buffer(buffer,bytes))return EINVAL;
 int e=gps_nv_enter(generation,id);if(e)return e;
 e=gps_nv_take(id);
 if(!e) {
  uint32_t serial,size;gps_nv_latest(id,&serial,&size);
  volatile uint32_t *h=gps_nv_host(id);uint32_t cap=h[NV_CAP];
  /* Match original strict end<capacity without offset+bytes overflow.
   * No sparse gap which could expose old target payload as new host data. */
  if(offset>size || offset>=cap || bytes>=cap-offset)e=EINVAL;
  else if(serial==UINT32_MAX || h[NV_WRITES]==UINT32_MAX ||
      gps_nv.writes==UINT64_MAX)e=EOVERFLOW;
  else {
   e=gps_nv_copy(id,offset,NULL,buffer,bytes);
   if(!e) {
    unsigned end=offset+bytes;if(end>size)size=end;
    e=gps_nv_sync_host(id,serial+1,size);
    if(!e){h[NV_WRITES]++;gps_nv.writes++;}
   }
  }
 }
 return gps_nv_leave(id,e);
}
int x9_gps_nv_invalidate(uint64_t generation,unsigned id)
{
 int e=gps_nv_enter(generation,id);if(e)return e;
 e=gps_nv_take(id);
 if(!e) {
  uint32_t serial,size;gps_nv_latest(id,&serial,&size);
  volatile uint32_t *h=gps_nv_host(id);
  if(serial==UINT32_MAX || h[NV_WRITES]==UINT32_MAX ||
      gps_nv.invalidations==UINT64_MAX)e=EOVERFLOW;
  else {
   e=gps_nv_copy(id,0,NULL,NULL,size); /* clear latest size, not stale host size */
   if(!e)e=gps_nv_sync_host(id,serial+1,0);
   if(!e){h[NV_WRITES]++;gps_nv.invalidations++;}
  }
 }
 return gps_nv_leave(id,e);
}
int x9_gps_nv_set_open(uint64_t generation,unsigned id,int open)
{
 if(open!=0 && open!=1)return EINVAL;
 int e=gps_nv_enter(generation,id);if(e)return e;
 e=gps_nv_take(id);
 if(!e) {
  volatile uint32_t *h=gps_nv_host(id);
  uint32_t flags=(h[NV_FLAGS]&~1u)|(unsigned)open;
  h[NV_FLAGS]=flags;gps_nv_barrier();
  e=gps_nv_owned(!gps_rt.booted);
  if(!e && h[NV_FLAGS]!=flags)e=EIO;
 }
 return gps_nv_leave(id,e);
}
static int gps_nv_remote(const struct x9_gps_mcusys_message *message)
{
 if(!gps_nv.started || message->type!=X9_GPS_MCUSYS_NVDATA)return 0;
 if(gps_nv.error)return gps_nv.error;
 if(gps_nv.remote_events==UINT64_MAX){gps_nv_journal(EOVERFLOW);return EOVERFLOW;}
 gps_nv.remote_events++;gps_nv.remote_mask|=1u<<message->id;
 /* Wake/notification fact only: no memory take, serial, ACK or POST_ON.
  * Provider explicitly calls read/info under the shared occupied protocol. */
 gps_nv_journal(0);return 0;
}
