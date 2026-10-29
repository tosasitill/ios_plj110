/* X9 update mailbox consumer. Built-in exact manifest is the authority; the
 * mailbox never selects paths, permissions, code identities or lengths. */
extern int sweet_x9_update_read(uint64_t,void *,size_t);
extern boolean_t sweet_x9_ufs_root_writable(void);
static int x9_update_hash(const struct X9ExternalFile *file) {
 SHA256_CTX h;uint8_t b[4096],sum[32];SHA256_Init(&h);
 for(uint64_t off=0;off<file->bytes;) {
  size_t n=file->bytes-off<sizeof(b)?(size_t)(file->bytes-off):sizeof(b);
  int e=sweet_x9_update_read(file->offset+off,b,n);if(e)return e;
  SHA256_Update(&h,b,n);off+=n;
 }
 SHA256_Final(sum,&h);return memcmp(sum,file->sha256,32)?EINVAL:0;
}
static int x9_update_directory(proc_t p,user_addr_t scratch,user_addr_t end,const char *path) {
 size_t n=strlen(path)+1;if(scratch>end||n>end-scratch)return E2BIG;
 int e=copyout(path,scratch,n);if(e)return e;int32_t rv=0;
 struct mkdir_args m={.path=scratch,.mode=0755};e=mkdir(p,&m,&rv);if(e&&e!=EEXIST)return e;
 vnode_t vp=NULLVP;e=vnode_lookup(path,VNODE_LOOKUP_NOFOLLOW,&vp,vfs_context_kernel());
 if(e)return e;bool valid=vnode_isdir(vp);vnode_put(vp);return valid?0:ENOTDIR;
}
static int x9_update_file(proc_t p,user_addr_t scratch,user_addr_t end,const struct X9ExternalFile *f) {
 if(!x9_render_external_check(f))return 0;
 vnode_t v=NULLVP;int e=vnode_lookup(f->path,VNODE_LOOKUP_NOFOLLOW,&v,vfs_context_kernel());
 if(!e){vnode_put(v);return EEXIST;}if(e!=ENOENT)return e;
 char tmp[512];int len=snprintf(tmp,sizeof(tmp),"%s.x9-update-tmp",f->path);
 if(len<0||(size_t)len>=sizeof(tmp))return E2BIG;
 size_t n=(size_t)len+1;if(scratch>end||n+4096>end-scratch)return E2BIG;
 e=copyout(tmp,scratch,n);if(e)return e;int32_t rv=0;
 e=vnode_lookup(tmp,VNODE_LOOKUP_NOFOLLOW,&v,vfs_context_kernel());
 if(!e){bool regular=vnode_isreg(v);vnode_put(v);if(!regular)return EEXIST;
  struct unlink_args u={.path=scratch};e=unlink(p,&u,&rv);if(e)return e;
 }else if(e!=ENOENT)return e;
 struct open_args o={.path=scratch,.flags=O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,.mode=f->mode};
 e=open(p,&o,&rv);if(e)return e;int fd=rv;
 struct fchmod_args chmod_request={.fd=fd,.mode=f->mode};e=fchmod(p,&chmod_request,&rv);
 uint8_t b[4096];
 for(uint64_t off=0;!e&&off<f->bytes;) {
  size_t count=f->bytes-off<sizeof(b)?(size_t)(f->bytes-off):sizeof(b);
  e=sweet_x9_update_read(f->offset+off,b,count);if(e)break;
  e=copyout(b,scratch+n,count);if(e)break;user_ssize_t wrote=0;
  struct write_args wr={.fd=fd,.cbuf=scratch+n,.nbyte=count};e=write(p,&wr,&wrote);
  if(!e&&wrote!=(user_ssize_t)count)e=EIO;off+=count;
 }
 if(!e){struct fsync_args sync_request={.fd=fd};e=fsync(p,&sync_request,&rv);}
 struct close_args close_request={.fd=fd};int close_error=sys_close(p,&close_request,&rv);if(!e)e=close_error;
 if(e)return e;
 /* Do not replace an existing target. PID1 runs before any plugin process. */
 e=vnode_lookup(f->path,VNODE_LOOKUP_NOFOLLOW,&v,vfs_context_kernel());
 if(!e){vnode_put(v);return EEXIST;}if(e!=ENOENT)return e;
 size_t destlen=strlen(f->path)+1;if(scratch>end||n+destlen>end-scratch)return E2BIG;
 e=copyout(f->path,scratch+n,destlen);if(e)return e;
 struct link_args link_request={.path=scratch,.link=scratch+n};e=link(p,&link_request,&rv);if(e)return e;
 struct unlink_args unlink_request={.path=scratch};e=unlink(p,&unlink_request,&rv);if(e)return e;
 return 0; /* The caller verifies every published file before discovery. */
}
static int x9_update_install(proc_t p,user_addr_t scratch,user_addr_t end) {
 if(!sweet_x9_ufs_root_writable()||!proc_is64bit(p))return EINVAL;
 uint8_t header[40];int e=sweet_x9_update_read(0,header,sizeof(header));if(e)return e;
 if(memcmp(header,"X9UPD001",8)||memcmp(header+8,x9_update_identity,32))return ENOENT;
 /* Verify the complete input before mutating the filesystem. */
 for(unsigned i=0;i<sizeof(x9_external_files)/sizeof(x9_external_files[0]);i++) {
  e=x9_update_hash(&x9_external_files[i]);if(e){printf("X9 UPDATE: payload hash mismatch file=%u errno=%d\n",i,e);return e;}
 }
 for(unsigned i=0;i<sizeof(x9_update_dirs)/sizeof(x9_update_dirs[0]);i++) {
  e=x9_update_directory(p,scratch,end,x9_update_dirs[i]);if(e)return e;
 }
 for(unsigned i=0;i<sizeof(x9_external_files)/sizeof(x9_external_files[0]);i++) {
  e=x9_update_file(p,scratch,end,&x9_external_files[i]);if(e){printf("X9 UPDATE: install error file=%u errno=%d\n",i,e);return e;}
 }
 printf("X9 UPDATE: payload installed and verified from reserved mailbox\n");return 0;
}
