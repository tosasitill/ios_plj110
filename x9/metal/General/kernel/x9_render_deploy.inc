/* SPDX-License-Identifier: MIT */
/* Included after x9_metal_deploy.inc, reusing its bounded exclusive install. */
#include "x9_render_payload.h"
extern int sweet_x9_render_payload_ready;
static int x9_render_directory(proc_t p,user_addr_t scratch,user_addr_t end,const char *path) {
 size_t n=strlen(path)+1;if(scratch>end||n>end-scratch)return E2BIG;
 int error=copyout(path,scratch,n);if(error)return error;int32_t rv=0;
 struct mkdir_args request={.path=scratch,.mode=0755};error=mkdir(p,&request,&rv);
 if(error&&error!=EEXIST)return error;
 vnode_t dir=NULLVP;error=vnode_lookup(path,VNODE_LOOKUP_NOFOLLOW,&dir,vfs_context_kernel());if(error)return error;
 boolean_t valid=vnode_isdir(dir);vnode_put(dir);return valid?0:ENOTDIR;
}
static int sweet_x9_render_deploy(proc_t p,user_addr_t scratch,user_addr_t end) {
 int mode=0;extern boolean_t sweet_x9_ufs_root_writable(void);
 if(!PE_parse_boot_argn("x9_metal_render",&mode,sizeof(mode))||mode!=1)return 0;
 if(!sweet_early_is_x9()||!sweet_x9_ufs_root_writable()||!proc_is64bit(p))return EINVAL;
 static const char target[]="/System/Library/xpc/launchd.plist";
 static const char backup[]="/System/Library/xpc/launchd.x9-render-pan170-before.plist";
 static const char temporary[]="/System/Library/xpc/launchd.x9-render-pan170.tmp";
 struct {const unsigned char *bytes;size_t size;} known[]={
  {x9_launchd_original,sizeof(x9_launchd_original)},
  {x9_launchd_older,sizeof(x9_launchd_older)},
  {x9_launchd_previous,sizeof(x9_launchd_previous)},
  {x9_launchd_metal,sizeof(x9_launchd_metal)},
  {x9_render_launchd,x9_render_launchd_size},
 };
 unsigned selected=0;while(selected<sizeof(known)/sizeof(known[0])&&x9_metal_same(target,known[selected].bytes,known[selected].size))selected++;
 if(selected==sizeof(known)/sizeof(known[0])) {
  printf("X9 RENDER DEPLOY: launchd cache unrecognized; conflict preserved, no files installed\n");return EEXIST;
 }
 for(unsigned i=0;i<sizeof(x9_render_directories)/sizeof(x9_render_directories[0]);i++) {
  int error=x9_render_directory(p,scratch,end,x9_render_directories[i]);if(error)return error;
 }
 for(unsigned i=0;i<sizeof(x9_render_files)/sizeof(x9_render_files[0]);i++) {
  const struct X9RenderPayloadFile *file=&x9_render_files[i];
  int error=x9_metal_install(p,scratch,end,file->path,file->data,file->bytes,file->mode);
  if(error){printf("X9 RENDER DEPLOY: file install failed path=%s errno=%d; existing conflicts kept\n",file->path,error);return error;}
 }
 if(selected!=sizeof(known)/sizeof(known[0])-1) {
  if(scratch>end||sizeof(target)+sizeof(backup)+4096>end-scratch)return E2BIG;
  int32_t rv=0;int error=copyout(target,scratch,sizeof(target));if(error)return error;
  user_addr_t dest=scratch+sizeof(target);error=copyout(backup,dest,sizeof(backup));if(error)return error;
  struct link_args link_request={.path=scratch,.link=dest};error=link(p,&link_request,&rv);
  if(error==EEXIST)error=x9_metal_same(backup,known[selected].bytes,known[selected].size);
  if(error)return error;
  error=x9_metal_install(p,scratch,end,temporary,x9_render_launchd,x9_render_launchd_size,0644);if(error)return error;
  error=x9_metal_same(target,known[selected].bytes,known[selected].size);if(error)return error;
  error=copyout(temporary,scratch,sizeof(temporary));if(error)return error;
  dest=scratch+sizeof(temporary);error=copyout(target,dest,sizeof(target));if(error)return error;
  struct rename_args rename_request={.from=scratch,.to=dest};error=rename(p,&rename_request,&rv);if(error)return error;
 }
 __atomic_store_n(&sweet_x9_render_payload_ready,1,__ATOMIC_RELEASE);
 printf("X9 RENDER DEPLOY: pan170 general Metal+native PanVK+render probe installed; exact cache/backup/exclusive files; desktop discovery armed\n");
 return 0;
}
