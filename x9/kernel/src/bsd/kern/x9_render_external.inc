/* SPDX-License-Identifier: MIT */
/* Only checks independently installed payload; performs no file writes. */
#include "x9_render_external_manifest.h"
extern int sweet_x9_render_payload_ready;
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *name, int def);
static char x9_render_active_plugin[64];
/* Published with payload_ready, and derived from the same verified allowlist. */
const char *sweet_x9_render_plugin_name(void) {
 if(!__atomic_load_n(&sweet_x9_render_payload_ready,__ATOMIC_ACQUIRE))return NULL;
 return x9_render_active_plugin[0]?x9_render_active_plugin:NULL;
}
static int x9_render_external_check(const struct X9ExternalFile *file) {
 vnode_t vp=NULLVP;vfs_context_t ctx=vfs_context_kernel();
 int error=vnode_lookup(file->path,VNODE_LOOKUP_NOFOLLOW,&vp,ctx);if(error)return error;
 if(!vnode_isreg(vp)){vnode_put(vp);return EINVAL;}
 off_t bytes=0;error=vnode_size(vp,&bytes,ctx);
 if(!error&&(bytes<0||(uint64_t)bytes!=file->bytes))error=EINVAL;
 SHA256_CTX state;uint8_t digest[SHA256_DIGEST_LENGTH],buf[4096];SHA256_Init(&state);
 for(uint64_t off=0;!error&&off<file->bytes;) {
  int count=(int)((file->bytes-off)<sizeof(buf)?file->bytes-off:sizeof(buf)),resid=0;
  error=vn_rdwr(UIO_READ,vp,(caddr_t)buf,count,(off_t)off,UIO_SYSSPACE,0,vfs_context_ucred(ctx),&resid,current_proc());
  if(!error&&resid)error=EIO;
  if(!error)SHA256_Update(&state,buf,count);off+=count;
 }
 SHA256_Final(digest,&state);if(!error&&memcmp(digest,file->sha256,sizeof(digest)))error=EINVAL;
 vnode_put(vp);return error;
}
static int sweet_x9_render_external_ready(void) {
 int mode=0;if(!sweet_early_is_x9()||(mode=sweet_x9_arg("x9_metal_render",1))!=1)return 0;
 for(unsigned i=0;i<sizeof(x9_external_files)/sizeof(x9_external_files[0]);i++) {
  int error=x9_render_external_check(&x9_external_files[i]);
  if(error){printf("X9 RENDER EXTERNAL: missing/mismatched path=%s errno=%d; discovery disabled\n",x9_external_files[i].path,error);return error;}
 }
 /* Require one versioned General Metal bundle in the signed file allowlist.
  * Never independently hard-code a revision in accelerator discovery. */
 const char prefix[]="/System/Library/Extensions/";
 const char suffix[]=".bundle/Info.plist";
 char plugin[sizeof(x9_render_active_plugin)]={0};unsigned found=0;
 for(unsigned i=0;i<sizeof(x9_external_files)/sizeof(x9_external_files[0]);i++) {
  const char *path=x9_external_files[i].path;
  size_t size=strlen(path),head=sizeof(prefix)-1,tail=sizeof(suffix)-1;
  if(size<=head+tail||strncmp(path,prefix,head)||strcmp(path+size-tail,suffix))continue;
  size_t name_size=size-head-tail;
  if(name_size<=sizeof("X9GeneralMetal")-1||name_size>=sizeof(plugin)||
     strncmp(path+head,"X9GeneralMetal",sizeof("X9GeneralMetal")-1))continue;
  for(size_t j=sizeof("X9GeneralMetal")-1;j<name_size;j++)
   if(path[head+j]<'0'||path[head+j]>'9')return EINVAL;
  if(++found!=1)return EINVAL;
  memcpy(plugin,path+head,name_size);
 }
 if(found!=1)return EINVAL;
 memcpy(x9_render_active_plugin,plugin,sizeof(plugin));
 printf("X9 RENDER EXTERNAL: active plugin=%s from verified allowlist\n",plugin);
 __atomic_store_n(&sweet_x9_render_payload_ready,1,__ATOMIC_RELEASE);
 printf("X9 RENDER EXTERNAL: exact signed Metal/PanVK files verified on root volume; native discovery armed\n");
 return 0;
}

#include "x9_update_install.inc"
