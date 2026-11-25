/* Native Bluetooth root files, installed by PID1 before the original launchd
 * reads its service cache. This replaces the Android-side macFUSE/COW stager:
 * the live iOS HFS does journal, allocation and ownership itself, nothing
 * else on the root is read or written, and no whole-root image is involved.
 *
 *   x9_bt_root=0 (default)  do nothing
 *   x9_bt_root=1            install the root payload embedded in this kernel
 *   x9_bt_root=2            restore the Apple originals, remove our own files
 *
 * Apple binaries are never modified; /usr/sbin/bluetoothd and BlueTool must
 * match the build-time originals before anything is written. The four
 * replaced Apple files (two jobs, launchd cache, PreferencesUI Settings.plist
 * with the Bluetooth row's capability gate removed) must hold their exact
 * original bytes; the original is first kept as a same-directory hard link
 * "<path>.x9-orig" (no data copy), then the verified new file is renamed over
 * it atomically. The cache is last on install and first on restore, so an
 * interrupted boot always leaves launchd with a complete original or new
 * cache. Every state is classified before the first mutation; an unknown file
 * anywhere stops the whole operation. A file holding an earlier build of
 * our own payload (listed per file at build time) is upgraded in place.
 */
struct x9_btr_prev {uint32_t bytes;uint8_t sha[32];};
struct x9_btr_file {
 const char *path;const uint8_t *data;uint32_t bytes;uint16_t mode;uint8_t sha[32];
 uint32_t orig_bytes;uint8_t orig_sha[32]; /* orig_bytes==0: new file, target must be absent */
 const struct x9_btr_prev *prev;uint32_t prev_count; /* earlier builds of our own file */
};
struct x9_btr_require {const char *path;uint32_t bytes;uint8_t sha[32];};
#if __has_include("../../../generated/x9_bt_root.inc")
#include "../../../generated/x9_bt_root.inc"
#endif
#ifndef X9_BT_ROOT_PRESENT
#define X9_BT_ROOT_PRESENT 0
#endif

#if X9_BT_ROOT_PRESENT
#define X9_BTR_READ_LIMIT (16u<<20)
struct x9_btr_user {user_addr_t at,end;};

static int x9_btr_str(struct x9_btr_user *u,const char *s,user_addr_t *out)
{
 size_t n=strlen(s)+1;
 if(u->at>u->end||n+16>u->end-u->at)return E2BIG;
 int e=copyout(s,u->at,n);if(e)return e;
 *out=u->at;u->at+=(n+15)&~(size_t)15;return 0;
}

/* Hash a regular file by reading to EOF (also correct for decmpfs files).
 * 0 = hashed; ENOENT = absent; EEXIST = not a regular file or too large. */
static int x9_btr_stat(const char *path,uint8_t sum[32],uint64_t *size,uint32_t *mode,uint32_t *uid,uint32_t *gid)
{
 vnode_t vp=NULLVP;vfs_context_t ctx=vfs_context_kernel();
 int e=vnode_lookup(path,VNODE_LOOKUP_NOFOLLOW,&vp,ctx);if(e)return e;
 if(!vnode_isreg(vp)){vnode_put(vp);return EEXIST;}
 struct vnode_attr a;VATTR_INIT(&a);
 VATTR_WANTED(&a,va_mode);VATTR_WANTED(&a,va_uid);VATTR_WANTED(&a,va_gid);
 e=vnode_getattr(vp,&a,ctx);
 SHA256_CTX h;SHA256_Init(&h);uint8_t buffer[4096];uint64_t off=0;
 while(!e){
  int resid=0;
  e=vn_rdwr(UIO_READ,vp,(caddr_t)buffer,(int)sizeof(buffer),(off_t)off,UIO_SYSSPACE,0,
            vfs_context_ucred(ctx),&resid,current_proc());
  if(e)break;
  uint32_t got=(uint32_t)(sizeof(buffer)-(size_t)resid);
  SHA256_Update(&h,buffer,got);off+=got;
  if(off>X9_BTR_READ_LIMIT){e=EEXIST;break;}
  if(got<sizeof(buffer))break;
 }
 SHA256_Final(sum,&h);vnode_put(vp);
 if(!e){*size=off;*mode=a.va_mode;*uid=a.va_uid;*gid=a.va_gid;}
 return e;
}

/* 0 = exact match (and mode/0:0 when mode>=0); ENOENT = absent; EEXIST = different. */
static int x9_btr_check(const char *path,uint32_t bytes,const uint8_t sha[32],int mode)
{
 uint8_t sum[32];uint64_t size=0;uint32_t m=0,uid=0,gid=0;
 int e=x9_btr_stat(path,sum,&size,&m,&uid,&gid);if(e)return e;
 if(size!=bytes||memcmp(sum,sha,32))return EEXIST;
 if(mode>=0&&((m&07777)!=(uint32_t)mode||uid||gid))return EEXIST;
 return 0;
}

/* 0 = current content is one of our earlier builds of this file. */
static int x9_btr_previous(const struct x9_btr_file *f)
{
 int e=EEXIST;
 for(uint32_t i=0;i<f->prev_count;i++){
  e=x9_btr_check(f->path,f->prev[i].bytes,f->prev[i].sha,f->mode);
  if(e!=EEXIST)return e;
 }
 return e;
}

static int x9_btr_unlink(proc_t p,struct x9_btr_user u,const char *path)
{
 user_addr_t a;int e=x9_btr_str(&u,path,&a);if(e)return e;
 int32_t rv=0;struct unlink_args r={.path=a};return unlink(p,&r,&rv);
}
static int x9_btr_link(proc_t p,struct x9_btr_user u,const char *from,const char *to)
{
 user_addr_t a,b;int e=x9_btr_str(&u,from,&a);if(!e)e=x9_btr_str(&u,to,&b);if(e)return e;
 int32_t rv=0;struct link_args r={.path=a,.link=b};return link(p,&r,&rv);
}
static int x9_btr_rename(proc_t p,struct x9_btr_user u,const char *from,const char *to)
{
 user_addr_t a,b;int e=x9_btr_str(&u,from,&a);if(!e)e=x9_btr_str(&u,to,&b);if(e)return e;
 int32_t rv=0;struct rename_args r={.from=a,.to=b};return rename(p,&r,&rv);
}
/* Remove our own leftover temp/backup name only if it is a regular file. */
static int x9_btr_drop(proc_t p,struct x9_btr_user u,const char *path)
{
 vnode_t vp=NULLVP;int e=vnode_lookup(path,VNODE_LOOKUP_NOFOLLOW,&vp,vfs_context_kernel());
 if(e)return e==ENOENT?0:e;
 bool regular=vnode_isreg(vp);vnode_put(vp);
 return regular?x9_btr_unlink(p,u,path):EEXIST;
}
static int x9_btr_name(char *out,size_t size,const char *path,const char *suffix)
{
 int n=snprintf(out,size,"%s%s",path,suffix);
 return n<0||(size_t)n>=size?E2BIG:0;
}

/* Write the embedded bytes to a fresh temp file: mode, root:wheel, fsync. */
static int x9_btr_write(proc_t p,struct x9_btr_user u,const char *tmp,const struct x9_btr_file *f)
{
 int e=x9_btr_drop(p,u,tmp);if(e)return e;
 user_addr_t path;e=x9_btr_str(&u,tmp,&path);if(e)return e;
 if(u.at>u.end||4096>u.end-u.at)return E2BIG;
 user_addr_t buffer=u.at;int32_t rv=0;
 struct open_args o={.path=path,.flags=O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,.mode=f->mode};
 e=open(p,&o,&rv);if(e)return e;int fd=rv;
 struct fchmod_args m={.fd=fd,.mode=f->mode};e=fchmod(p,&m,&rv);
 struct fchown_args own={.fd=fd,.uid=0,.gid=0};if(!e)e=fchown(p,&own,&rv);
 for(uint32_t off=0;!e&&off<f->bytes;){
  uint32_t count=f->bytes-off<4096?f->bytes-off:4096;user_ssize_t wrote=0;
  e=copyout(f->data+off,buffer,count);
  if(!e){struct write_args w={.fd=fd,.cbuf=buffer,.nbyte=count};e=write(p,&w,&wrote);}
  if(!e&&wrote!=(user_ssize_t)count)e=EIO;off+=count;
 }
 if(!e){struct fsync_args s={.fd=fd};e=fsync(p,&s,&rv);}
 struct close_args c={.fd=fd};int ce=sys_close(p,&c,&rv);if(!e)e=ce;
 if(!e)e=x9_btr_check(tmp,f->bytes,f->sha,f->mode);
 if(e)(void)x9_btr_unlink(p,u,tmp);
 return e;
}

enum {X9_BTR_INSTALLED=1,X9_BTR_TODO=2,X9_BTR_UPGRADE=3};
/* Classify one target without changing anything. */
static int x9_btr_state(const struct x9_btr_file *f,int *state)
{
 int e=x9_btr_check(f->path,f->bytes,f->sha,f->mode);
 if(!e){*state=X9_BTR_INSTALLED;return 0;}
 if(e==EEXIST&&!x9_btr_previous(f)){
  if(f->orig_bytes){ /* an upgrade needs the verified Apple original kept earlier */
   char kept[320];e=x9_btr_name(kept,sizeof(kept),f->path,".x9-orig");if(e)return e;
   e=x9_btr_check(kept,f->orig_bytes,f->orig_sha,-1);if(e)return e==ENOENT?ESTALE:e;
  }
  *state=X9_BTR_UPGRADE;return 0;
 }
 if(!f->orig_bytes){if(e==ENOENT){*state=X9_BTR_TODO;return 0;}return e;}
 if(e!=EEXIST)return e; /* an Apple original is missing */
 e=x9_btr_check(f->path,f->orig_bytes,f->orig_sha,-1);if(e)return e==EEXIST?ESTALE:e;
 char bak[320];e=x9_btr_name(bak,sizeof(bak),f->path,".x9-orig");if(e)return e;
 e=x9_btr_check(bak,f->orig_bytes,f->orig_sha,-1);
 if(e&&e!=ENOENT)return e; /* foreign .x9-orig: never replace it */
 *state=X9_BTR_TODO;return 0;
}

static int x9_btr_apply(proc_t p,struct x9_btr_user u,const struct x9_btr_file *f,bool upgrade)
{
 char tmp[320],bak[320];
 int e=x9_btr_name(tmp,sizeof(tmp),f->path,".x9-bt-temp");if(e)return e;
 if(upgrade){ /* our own earlier build: replace it atomically, no backup */
  e=x9_btr_write(p,u,tmp,f);if(e)return e;
  e=x9_btr_rename(p,u,tmp,f->path);
  if(e)(void)x9_btr_unlink(p,u,tmp);
  else e=x9_btr_check(f->path,f->bytes,f->sha,f->mode);
  return e;
 }
 if(f->orig_bytes){
  e=x9_btr_name(bak,sizeof(bak),f->path,".x9-orig");if(e)return e;
  e=x9_btr_check(bak,f->orig_bytes,f->orig_sha,-1);
  if(e==ENOENT){e=x9_btr_link(p,u,f->path,bak);if(!e)e=x9_btr_check(bak,f->orig_bytes,f->orig_sha,-1);}
  if(e)return e;
 }
 e=x9_btr_write(p,u,tmp,f);if(e)return e;
 /* New file: link() never replaces a target that appeared meanwhile.
  * Apple file: rename() replaces the verified original atomically. */
 e=f->orig_bytes?x9_btr_rename(p,u,tmp,f->path):x9_btr_link(p,u,tmp,f->path);
 if(!f->orig_bytes||e){int ue=x9_btr_unlink(p,u,tmp);if(!e&&ue!=ENOENT)e=ue;}
 if(!e)e=x9_btr_check(f->path,f->bytes,f->sha,f->mode);
 return e;
}

static int x9_btr_install(proc_t p,struct x9_btr_user u)
{
 const unsigned count=sizeof(x9_btr_files)/sizeof(x9_btr_files[0]);
 int e,state[sizeof(x9_btr_files)/sizeof(x9_btr_files[0])];
 for(unsigned i=0;i<sizeof(x9_btr_requires)/sizeof(x9_btr_requires[0]);i++){
  const struct x9_btr_require *r=&x9_btr_requires[i];
  e=x9_btr_check(r->path,r->bytes,r->sha,-1);
  if(e){printf("X9 BT ROOT: Apple original %s does not match build errno=%d; nothing written\n",r->path,e);return e;}
 }
 for(unsigned i=0;i<count;i++){
  const struct x9_btr_file *f=&x9_btr_files[i];uint8_t sum[32];SHA256_CTX h;
  SHA256_Init(&h);SHA256_Update(&h,f->data,f->bytes);SHA256_Final(sum,&h);
  if(memcmp(sum,f->sha,32)){printf("X9 BT ROOT: embedded file=%u corrupt; nothing written\n",i);return EIO;}
 }
 unsigned todo=0;
 for(unsigned i=0;i<count;i++){
  e=x9_btr_state(&x9_btr_files[i],&state[i]);
  if(e){printf("X9 BT ROOT: %s unexpected content errno=%d; nothing written\n",x9_btr_files[i].path,e);return e;}
  todo+=state[i]!=X9_BTR_INSTALLED;
 }
 if(!todo){printf("X9 BT ROOT: all %u files already installed\n",count);return 0;}
 for(unsigned i=0;i<count;i++){
  if(state[i]==X9_BTR_INSTALLED)continue;
  e=x9_btr_apply(p,u,&x9_btr_files[i],state[i]==X9_BTR_UPGRADE);
  printf("X9 BT ROOT: %s %s errno=%d\n",state[i]==X9_BTR_UPGRADE?"upgrade":"install",x9_btr_files[i].path,e);
  if(e){printf("X9 BT ROOT: stopped at file=%u; launchd cache %s\n",i,
               i+1<count?"still original":"not replaced");return e;}
 }
 for(unsigned i=0;i<count;i++){char tmp[320];
  if(!x9_btr_name(tmp,sizeof(tmp),x9_btr_files[i].path,".x9-bt-temp"))(void)x9_btr_drop(p,u,tmp);}
 printf("X9 BT ROOT: installed %u/%u files (originals kept as .x9-orig)\n",todo,count);return 0;
}

static int x9_btr_restore(proc_t p,struct x9_btr_user u)
{
 const unsigned count=sizeof(x9_btr_files)/sizeof(x9_btr_files[0]);
 unsigned changed=0;
 for(unsigned k=count;k-->0;){
  const struct x9_btr_file *f=&x9_btr_files[k];char tmp[320],bak[320];
  int e=x9_btr_name(tmp,sizeof(tmp),f->path,".x9-bt-temp");if(!e)e=x9_btr_drop(p,u,tmp);
  if(!e&&f->orig_bytes)e=x9_btr_name(bak,sizeof(bak),f->path,".x9-orig");
  if(!e&&f->orig_bytes){
   int b=x9_btr_check(bak,f->orig_bytes,f->orig_sha,-1);
   int cur=x9_btr_check(f->path,f->orig_bytes,f->orig_sha,-1);
   if(!cur){if(!b){e=x9_btr_unlink(p,u,bak);changed++;}else if(b!=ENOENT)e=b;}
   else if(b)e=b==ENOENT?ESTALE:b; /* no verified original to restore */
   else if(cur!=ENOENT&&x9_btr_check(f->path,f->bytes,f->sha,f->mode)&&x9_btr_previous(f))e=ESTALE; /* unknown content */
   else{e=x9_btr_rename(p,u,bak,f->path);if(!e)e=x9_btr_check(f->path,f->orig_bytes,f->orig_sha,-1);changed++;}
  }else if(!e){
   int cur=x9_btr_check(f->path,f->bytes,f->sha,f->mode);
   if(cur==EEXIST&&!x9_btr_previous(f))cur=0; /* an earlier build of ours */
   if(!cur){e=x9_btr_unlink(p,u,f->path);changed++;}
   else if(cur!=ENOENT)e=cur; /* not ours: keep it */
  }
  if(e){printf("X9 BT ROOT: restore %s errno=%d; stopped\n",f->path,e);return e;}
 }
 printf("X9 BT ROOT: restored originals (%u changes)\n",changed);return 0;
}
#endif

static int x9_bluetooth_root(proc_t p,user_addr_t scratch,user_addr_t end)
{
 int mode=sweet_x9_arg("x9_bt_root",0);
 if(!sweet_early_is_x9()||!mode||proc_getpid(p)!=1)return 0;
#if X9_BT_ROOT_PRESENT
 if(mode!=1&&mode!=2)return EINVAL;
 if(!sweet_x9_ufs_root_active()||!sweet_x9_ufs_root_writable()||!proc_is64bit(p))return EROFS;
 struct x9_btr_user u={.at=scratch,.end=end};
 return mode==1?x9_btr_install(p,u):x9_btr_restore(p,u);
#else
 (void)scratch;(void)end;
 printf("X9 BT ROOT: x9_bt_root=%d but this kernel embeds no root payload\n",mode);return ENOENT;
#endif
}
