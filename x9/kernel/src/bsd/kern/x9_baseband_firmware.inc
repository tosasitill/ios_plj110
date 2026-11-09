/* SPDX-License-Identifier: BSD-2-Clause */
/* ABM firmware integration adapted from QcomBaseband dd0199cf platform/coachz/
 * patches/coachz-prelink-baseband.patch and UniXNU ae687009 zzy-native-gpu-addon-mount.
 * X9: original D22AP/20H392 Mav17 assets, native writable HFS, 16 KiB pages.
 * No Apple code replacement, firmware personalization or physical MD download.
 * Keep original/existing files, publish missing files atomically and hardlink
 * the second firmware path. No mount or overlay of the Cryptex/cache parents.
 */
#include <libkern/zlib.h>
#include <pexpert/device_tree.h>
#include "../../../generated/x9_baseband_firmware.inc"

static thread_t x9_bbfw_mount_owner;
int sweet_x9_baseband_boot_mount_allowed(const char *source);
/* bindfs invokes this only for its boot-time entitlement exception. A boolean
 * global alone would authorize unrelated threads; require the actual caller,
 * PID1/native root, opt-in and the one fixed verified source directory. */
int sweet_x9_baseband_boot_mount_allowed(const char *source)
{
 return __atomic_load_n(&x9_bbfw_mount_owner,__ATOMIC_ACQUIRE)==current_thread() &&
  proc_getpid(current_proc())==1 && sweet_x9_ufs_root_active() &&
  sweet_x9_ufs_root_writable() && sweet_x9_arg("x9_bb",0)==1 &&
  !strcmp(source,"/private/var/x9/baseband/Mav17");
}

static int x9_bbfw_directory(proc_t p,user_addr_t scratch,user_addr_t end,const char *path)
{
 char parent[256];size_t n=strlen(path);
 if(n>=sizeof(parent)||path[0]!='/')return EINVAL;
 memcpy(parent,path,n+1);
 for(size_t i=1;i<=n;i++)if(parent[i]=='/'||!parent[i]){
  char saved=parent[i];parent[i]=0;
  int error=x9_update_directory(p,scratch,end,parent);
  parent[i]=saved;if(error)return error;
 }
 return 0;
}

static int x9_bbfw_equal(const char *path,const uint8_t digest[32],uint32_t size)
{
 vnode_t vp=NULLVP;vfs_context_t ctx=vfs_context_kernel();
 int error=vnode_lookup(path,VNODE_LOOKUP_NOFOLLOW,&vp,ctx);if(error)return error;
 if(!vnode_isreg(vp)){vnode_put(vp);return EEXIST;}
 struct vnode_attr attributes;VATTR_INIT(&attributes);
 VATTR_WANTED(&attributes,va_data_size);VATTR_WANTED(&attributes,va_mode);
 VATTR_WANTED(&attributes,va_uid);VATTR_WANTED(&attributes,va_gid);
 error=vnode_getattr(vp,&attributes,ctx);
 if(!error&&(attributes.va_data_size!=size||(attributes.va_mode&07777)!=0644||attributes.va_uid||attributes.va_gid))error=EEXIST;
 SHA256_CTX hash;uint8_t buffer[4096],sum[32];SHA256_Init(&hash);
 for(uint32_t off=0;!error&&off<size;){
  int n=(int)((size-off)<sizeof(buffer)?size-off:sizeof(buffer)),resid=0;
  error=vn_rdwr(UIO_READ,vp,(caddr_t)buffer,n,off,UIO_SYSSPACE,0,vfs_context_ucred(ctx),&resid,current_proc());
  if(!error&&resid)error=EIO;
  if(!error)SHA256_Update(&hash,buffer,n);off+=(uint32_t)n;
 }
 SHA256_Final(sum,&hash);if(!error&&memcmp(sum,digest,32))error=EEXIST;
 vnode_put(vp);return error;
}

/* An existing different file or symlink is a conflict, not permission to
 * truncate it. All sources have already passed their built-in SHA256.
 * A matching leftover temp can be published after an interrupted prior boot. */
static int x9_bbfw_write(proc_t p,user_addr_t scratch,user_addr_t end,
 const char *path,const uint8_t *data,uint32_t bytes,const uint8_t digest[32],int *created)
{
 *created=0;int error=x9_bbfw_equal(path,digest,bytes);
 if(!error)return 0;if(error!=ENOENT)return error;
 char temporary[288];int n=snprintf(temporary,sizeof(temporary),"%s.x9-bbfw-temp",path);
 if(n<0||(size_t)n>=sizeof(temporary))return E2BIG;
 size_t tn=(size_t)n+1,pn=strlen(path)+1;
 if(scratch>end||tn+pn+4096>end-scratch)return E2BIG;
 user_addr_t target=scratch+tn,buffer=target+pn;
 if((error=copyout(temporary,scratch,tn))||(error=copyout(path,target,pn)))return error;
 int32_t rv=0;int fd=-1;
 struct open_args request={.path=scratch,.flags=O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,.mode=0644};
 error=open(p,&request,&rv);
 if(error==EEXIST){error=x9_bbfw_equal(temporary,digest,bytes);if(error)return error;}
 else if(error)return error;
 else{
  fd=rv;
  struct fchmod_args mode={.fd=fd,.mode=0644};error=fchmod(p,&mode,&rv);
  struct fchown_args owner={.fd=fd,.uid=0,.gid=0};if(!error)error=fchown(p,&owner,&rv);
  for(uint32_t off=0;!error&&off<bytes;){
   uint32_t count=bytes-off<4096?bytes-off:4096;
   error=copyout(data+off,buffer,count);user_ssize_t wrote=0;
   if(!error){struct write_args write_request={.fd=fd,.cbuf=buffer,.nbyte=count};error=write(p,&write_request,&wrote);}
   if(!error&&wrote!=(user_ssize_t)count)error=EIO;off+=count;
  }
  if(!error){struct fsync_args sync={.fd=fd};error=fsync(p,&sync,&rv);}
  struct close_args close_request={.fd=fd};int ce=sys_close(p,&close_request,&rv);if(!error)error=ce;
  if(error){struct unlink_args cleanup={.path=scratch};(void)unlink(p,&cleanup,&rv);return error;}
 }
 struct link_args publish={.path=scratch,.link=target};error=link(p,&publish,&rv);
 if(error==EEXIST)error=x9_bbfw_equal(path,digest,bytes);
 if(error)return error;
 struct unlink_args cleanup={.path=scratch};error=unlink(p,&cleanup,&rv);
 if(!error)*created=1;
 return error;
}

/* Reference logd subsystem options; existing owner settings are not changed.
 * Private logs may include modem/SIM identities and remain local. */
static int x9_bbfw_logging(proc_t p,user_addr_t scratch,user_addr_t end)
{
 if(sweet_x9_arg("x9_bb_logging",1)!=1)return 0;
 static const char directory[]="/private/var/preferences/Logging/Subsystems";
 static const char *const names[]={"com.apple.telephony.abm","com.apple.telephony.bb","com.apple.CommCenter"};
 static const uint8_t options[]="<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict><key>DEFAULT-OPTIONS</key><dict>"
  "<key>Enable-Private-Data</key><true/><key>Privacy-Enable-Level</key><string>Private</string></dict></dict></plist>\n";
 int error=x9_bbfw_directory(p,scratch,end,directory);if(error)return error;
 uint8_t digest[32];SHA256_CTX hash;SHA256_Init(&hash);SHA256_Update(&hash,options,sizeof(options)-1);SHA256_Final(digest,&hash);
 unsigned made=0,kept=0;
 for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++){
  char path[192];snprintf(path,sizeof(path),"%s/%s.plist",directory,names[i]);
  vnode_t vp=NULLVP;error=vnode_lookup(path,VNODE_LOOKUP_NOFOLLOW,&vp,vfs_context_kernel());
  if(!error){bool regular=vnode_isreg(vp);vnode_put(vp);if(!regular)return EEXIST;kept++;continue;}
  if(error!=ENOENT)return error;
  int created=0;error=x9_bbfw_write(p,scratch,end,path,options,sizeof(options)-1,digest,&created);
  if(error)return error;made+=(unsigned)created;
 }
 printf("X9 BBFW: private subsystem logging created %u kept %u (local only)\n",made,kept);
 return 0;
}

#if X9_BBFW_PRESENT
/* External bytes come only from a separately reserved pinned file, not the
 * plugin mailbox. The built-in manifest still controls names, sizes and hashes. */
#if X9_BBFW_EXTERNAL
extern int sweet_x9_bbfw_read(uint64_t,void *,size_t);
#endif
static int x9_bbfw_alias(proc_t p,user_addr_t scratch,user_addr_t end,
 const char *source,const char *path,const struct x9_bbfw_file *file,int *created)
{
 *created=0;int error=x9_bbfw_equal(path,file->sha256,file->bytes);
 if(!error)return 0;if(error!=ENOENT)return error;
 error=x9_bbfw_equal(source,file->sha256,file->bytes);if(error)return error;
 size_t sn=strlen(source)+1,pn=strlen(path)+1;
 if(scratch>end||sn+pn>end-scratch)return E2BIG;
 if((error=copyout(source,scratch,sn))||(error=copyout(path,scratch+sn,pn)))return error;
 int32_t rv=0;struct link_args request={.path=scratch,.link=scratch+sn};error=link(p,&request,&rv);
 if(!error)*created=1;
 else if(error!=EEXIST)return error;
 /* No duplicate-copy fallback on EXDEV or other errors: qdsp is ~57 MiB. */
 return x9_bbfw_equal(path,file->sha256,file->bytes);
}

static int x9_bbfw_load(const uint8_t **pack)
{
 *pack=NULL;const uint8_t *bytes=NULL;
#if X9_BBFW_EXTERNAL
 uint8_t *data=kalloc_data(X9_BBFW_PACK_BYTES,Z_WAITOK);if(!data)return ENOMEM;
 for(uint32_t off=0;off<X9_BBFW_PACK_BYTES;){
  size_t n=X9_BBFW_PACK_BYTES-off;if(n>256u*1024u)n=256u*1024u;
  int error=sweet_x9_bbfw_read(off,data+off,n);
  if(error){kfree_data(data,X9_BBFW_PACK_BYTES);return error;}off+=(uint32_t)n;
 }
 bytes=data;
#else
 if((uint64_t)(x9_bbfw_pack_end-x9_bbfw_pack)!=X9_BBFW_PACK_BYTES)return EINVAL;
 bytes=x9_bbfw_pack;
#endif
 SHA256_CTX hash;uint8_t digest[32];SHA256_Init(&hash);SHA256_Update(&hash,bytes,X9_BBFW_PACK_BYTES);SHA256_Final(digest,&hash);
 if(memcmp(bytes,"X9BBF001",8)||memcmp(digest,x9_bbfw_pack_sha256,32)){
#if X9_BBFW_EXTERNAL
  kfree_data(data,X9_BBFW_PACK_BYTES);
#endif
  return EINVAL;
 }
 *pack=bytes;return 0;
}

static voidpf x9_bbfw_zalloc(voidpf opaque __unused,uInt items,uInt size)
{
 if(size&&items>(SIZE_MAX-16)/size)return Z_NULL;
 size_t bytes=(size_t)items*size+16;uint8_t *block=kalloc_data(bytes,Z_WAITOK|Z_ZERO);
 if(!block)return Z_NULL;memcpy(block,&bytes,sizeof(bytes));return block+16;
}
static void x9_bbfw_zfree(voidpf opaque __unused,voidpf address)
{
 if(!address)return;uint8_t *block=(uint8_t *)address-16;size_t bytes=0;
 memcpy(&bytes,block,sizeof(bytes));kfree_data(block,bytes);
}
static int x9_bbfw_decode(const uint8_t *pack,const struct x9_bbfw_file *file,uint8_t **out)
{
 *out=NULL;
 if(file->offset<16||file->offset>X9_BBFW_PACK_BYTES||!file->packed||
  file->packed>X9_BBFW_PACK_BYTES-file->offset||file->bytes>64u*1024u*1024u)return EINVAL;
 size_t allocated=file->bytes?file->bytes:1;
 uint8_t *data=kalloc_data(allocated,Z_WAITOK);if(!data)return ENOMEM;
 z_stream stream={0};stream.zalloc=x9_bbfw_zalloc;stream.zfree=x9_bbfw_zfree;
 stream.next_in=(Bytef *)(uintptr_t)(pack+file->offset);stream.avail_in=file->packed;
 stream.next_out=data;stream.avail_out=(uInt)allocated;
 int error=inflateInit(&stream)==Z_OK?0:ENOMEM;
 if(!error){int result=inflate(&stream,Z_FINISH);
  error=result==Z_STREAM_END&&stream.total_out==file->bytes&&!stream.avail_in?0:EINVAL;
  inflateEnd(&stream);
 }
 if(!error){SHA256_CTX hash;uint8_t digest[32];SHA256_Init(&hash);SHA256_Update(&hash,data,file->bytes);SHA256_Final(digest,&hash);
  if(memcmp(digest,file->sha256,32))error=EINVAL;}
 if(error){kfree_data(data,allocated);}else{*out=data;}
 return error;
}

static int x9_bbfw_mount(proc_t p,user_addr_t scratch,user_addr_t end,const char *point)
{
 static const char source[]="/private/var/x9/baseband/Mav17";
 size_t pn=strlen(point)+1;
 if(scratch>end||7+pn+sizeof(source)>end-scratch)return E2BIG;
 int error=x9_bbfw_directory(p,scratch,end,point);if(error)return error;
 struct mount_args request={.type=scratch,.path=scratch+7,.flags=MNT_RDONLY,.data=scratch+7+pn};int32_t rv=0;
 if((error=copyout("bindfs",request.type,7))||(error=copyout(point,request.path,pn))||
  (error=copyout(source,request.data,sizeof(source))))return error;
 __atomic_store_n(&x9_bbfw_mount_owner,current_thread(),__ATOMIC_RELEASE);
 error=mount(p,&request,&rv);
 __atomic_store_n(&x9_bbfw_mount_owner,NULL,__ATOMIC_RELEASE);
 if(error)return error;
 /* Verify the authoritative visible view, not merely mount() returning 0. */
 for(unsigned i=0;i<sizeof(x9_bbfw_files)/sizeof(x9_bbfw_files[0]);i++){
  char path[288];int n=snprintf(path,sizeof(path),"%s/%s",point,x9_bbfw_files[i].name);
  if(n<0||(size_t)n>=sizeof(path))return E2BIG;
  error=x9_bbfw_equal(path,x9_bbfw_files[i].sha256,x9_bbfw_files[i].bytes);if(error)return error;
 }
 return 0;
}
#endif

static int sweet_x9_prepare_baseband_firmware(proc_t p,user_addr_t scratch,user_addr_t end)
{
 if(sweet_x9_arg("x9_bb",0)!=1||!sweet_x9_ufs_root_active())return 0;
 if(!sweet_x9_ufs_root_writable()||!proc_is64bit(p)||proc_getpid(p)!=1)return EROFS;
 /* rev252hk: BBUpdater (as _wireless) extends bbfs/efs1.bin with ftruncate; the root mount's 1% HFS
  * reserve (164 MiB of 16 GiB) left non-root ~3 MiB of the 167 MiB free -> ENOSPC -> "Failed to create
  * scratch ... (11)". Lower it (only lowers); x9_hfs_reserve_mb=<MiB>, default 16. */
 {
  extern int x9_hfs_lower_reserve(mount_t,uint64_t,uint64_t *,uint64_t *,uint64_t *);
  int mb=sweet_x9_arg("x9_hfs_reserve_mb",16);
  uint64_t before=0,after=0,free_bytes=0;int rerr=EINVAL;
  vnode_t root=vfs_rootvnode();
  if(root!=NULLVP){
   if(mb>=0)rerr=x9_hfs_lower_reserve(vnode_mount(root),(uint64_t)mb<<20,&before,&after,&free_bytes);
   vnode_put(root);
  }
  printf("X9 BBFW: root HFS reserve %llu -> %llu KiB, free %llu KiB, non-root usable %llu KiB errno=%d\n",
   before>>10,after>>10,free_bytes>>10,free_bytes>after?(free_bytes-after)>>10:0,rerr);
 }
 int logging=x9_bbfw_logging(p,scratch,end);
 if(logging)printf("X9 BBFW: logging setup errno=%d; existing settings preserved\n",logging);
 if(sweet_x9_arg("x9_bb_firmware",1)!=1)return 0;
#if !X9_BBFW_PRESENT
 /* rev252ff: BootControllerLegacy::prepare is only stat(firmwarePath) (ABM 20H392 0x1b7f332d0).
  * Create the two empty firmware directories so that existence check can pass; no files, no mount. */
 {
  int standalone=x9_bbfw_directory(p,scratch,end,"/usr/standalone/firmware/Baseband/Mav17");
  int preboot=ENOENT;DTEntry chosen=NULL;const void *value=NULL;unsigned value_size=0;
  if(SecureDTLookupEntry(NULL,"/chosen",&chosen)==kSuccess&&
   SecureDTGetProperty(chosen,"boot-manifest-hash",&value,&value_size)==kSuccess&&value_size&&value_size<=48){
   char name[97]={0},point[256];
   for(unsigned i=0;i<value_size;i++)snprintf(name+2*i,3,"%02X",((const uint8_t *)value)[i]);
   snprintf(point,sizeof(point),"/private/preboot/%s/usr/local/standalone/firmware/Baseband/Mav17",name);
   preboot=x9_bbfw_directory(p,scratch,end,point);
  }
  printf("X9 BBFW: Mav17 input absent; empty directories only (existence check) standalone errno=%d personalized errno=%d\n",standalone,preboot);
 }
 return 0;
#else
 const uint8_t *pack=NULL;int error=x9_bbfw_load(&pack);
 if(error){printf("X9 BBFW: pack load/verify external=%u errno=%d; firmware not installed\n",X9_BBFW_EXTERNAL,error);return error;}
 unsigned count=sizeof(x9_bbfw_files)/sizeof(x9_bbfw_files[0]),written=0,same=0,linked=0;int result=0;
 /* Validate every member before any file is installed. */
 for(unsigned i=0;i<count;i++){uint8_t *data=NULL;error=x9_bbfw_decode(pack,&x9_bbfw_files[i],&data);
  if(error){result=error;goto release_pack;}kfree_data(data,x9_bbfw_files[i].bytes?x9_bbfw_files[i].bytes:1);}
 /* rev252gg: write the files straight into the two paths ABM looks at (no bindfs): ABM 20H392
  * BootControllerLegacy::prepare stats capabilities::bb::radio::getPersonalizedFirmwarePath(),
  * BBUpdater then reads info.plist, Options.plist and MBN files from it. */
 char points[2][256];unsigned npoints=0;
 snprintf(points[npoints++],sizeof(points[0]),"/usr/standalone/firmware/Baseband/Mav17");
 {DTEntry chosen=NULL;const void *value=NULL;unsigned value_size=0;
  if(SecureDTLookupEntry(NULL,"/chosen",&chosen)==kSuccess&&
   SecureDTGetProperty(chosen,"boot-manifest-hash",&value,&value_size)==kSuccess&&value_size&&value_size<=48){
   char name[97]={0};
   for(unsigned i=0;i<value_size;i++)snprintf(name+2*i,3,"%02X",((const uint8_t *)value)[i]);
   snprintf(points[npoints++],sizeof(points[0]),"/private/preboot/%s/usr/local/standalone/firmware/Baseband/Mav17",name);
  }}
 int directories[2]={0};
 for(unsigned k=0;k<npoints;k++){
  directories[k]=x9_bbfw_directory(p,scratch,end,points[k]);
  if(directories[k]){printf("X9 BBFW: directory %s errno=%d\n",points[k],directories[k]);if(!result)result=directories[k];}
 }
 /* Prefer either existing verified copy. Never replace it just to deduplicate
  * old installs. Missing files get exactly one data copy, then hardlink aliases. */
 for(unsigned i=0;i<count;i++){
  const struct x9_bbfw_file *file=&x9_bbfw_files[i];char paths[2][320];int state[2],source=-1,missing=-1;
  bool conflict=false;
  for(unsigned k=0;k<npoints;k++){
   int n=snprintf(paths[k],sizeof(paths[k]),"%s/%s",points[k],file->name);
   state[k]=directories[k]?directories[k]:(n<0||(size_t)n>=sizeof(paths[k]))?E2BIG:x9_bbfw_equal(paths[k],file->sha256,file->bytes);
   if(!state[k]){source=(int)k;same++;}
   else if(state[k]==ENOENT){if(missing<0)missing=(int)k;}
   else{printf("X9 BBFW: %s errno=%d (kept)\n",paths[k],state[k]);if(!result)result=state[k];conflict=true;}
  }
  if(conflict||missing<0)continue;
  if(source<0){
   uint8_t *data=NULL;error=x9_bbfw_decode(pack,file,&data);if(error){if(!result)result=error;continue;}
   int made=0;error=x9_bbfw_write(p,scratch,end,paths[missing],data,file->bytes,file->sha256,&made);
   kfree_data(data,file->bytes?file->bytes:1);
   if(!error)error=x9_bbfw_equal(paths[missing],file->sha256,file->bytes);
   if(error){printf("X9 BBFW: write %s errno=%d\n",paths[missing],error);if(!result)result=error;continue;}
   written+=(unsigned)made;source=missing;state[source]=0;
  }
  for(unsigned k=0;k<npoints;k++)if(state[k]==ENOENT){
   int made=0;error=x9_bbfw_alias(p,scratch,end,paths[source],paths[k],file,&made);
   if(error){printf("X9 BBFW: link %s errno=%d\n",paths[k],error);if(!result)result=error;continue;}
   linked+=(unsigned)made;
  }
 }
 printf("X9 BBFW: original 20H392 Mav17 files %u in %u paths written %u linked %u same %u external %u complete %u result errno=%d (direct write, no ticket/MD download)\n",count,npoints,written,linked,same,X9_BBFW_EXTERNAL,X9_BBFW_COMPLETE,result);
release_pack:
#if X9_BBFW_EXTERNAL
 {uint8_t *owned=(uint8_t *)(uintptr_t)pack;kfree_data(owned,X9_BBFW_PACK_BYTES);}
#endif
 return result;
#endif
}
