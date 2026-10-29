/* SPDX-License-Identifier: MIT */
/* Only checks independently installed payload; performs no file writes. */
#include "x9_render_external_manifest.h"
extern int sweet_x9_render_payload_ready;
extern int sweet_early_is_x9(void);
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
 int mode=0;if(!sweet_early_is_x9()||!PE_parse_boot_argn("x9_metal_render",&mode,sizeof(mode))||mode!=1)return 0;
 for(unsigned i=0;i<sizeof(x9_external_files)/sizeof(x9_external_files[0]);i++) {
  int error=x9_render_external_check(&x9_external_files[i]);
  if(error){printf("X9 RENDER EXTERNAL: missing/mismatched path=%s errno=%d; discovery disabled\n",x9_external_files[i].path,error);return error;}
 }
 __atomic_store_n(&sweet_x9_render_payload_ready,1,__ATOMIC_RELEASE);
 printf("X9 RENDER EXTERNAL: exact signed Metal/PanVK files verified on root volume; native discovery armed\n");
 return 0;
}

#include "x9_update_install.inc"
