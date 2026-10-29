/* Candidate-specific guarded deployment, invoked before original PID1 exec.
 * Apple executable bytes are untouched. Only install if the current launchd
 * cache is exactly the captured original or our exact derived cache. */
#include "x9_metal_payload.inc"
extern int sweet_early_is_x9(void);
static int x9_metal_same(const char *path,const unsigned char *data,size_t size)
{
 vnode_t vp=NULLVP;vfs_context_t ctx=vfs_context_kernel();int error;
 error=vnode_lookup(path,VNODE_LOOKUP_NOFOLLOW,&vp,ctx);if(error)return error;
 if(!vnode_isreg(vp)){vnode_put(vp);return EINVAL;}
 off_t actual=0;error=vnode_size(vp,&actual,ctx);
 if(!error&&(actual<0||(uint64_t)actual!=size))error=EINVAL;
 unsigned char buffer[1024];
 for(size_t off=0;!error&&off<size;off+=sizeof(buffer)) {
  int count=(int)((size-off)<sizeof(buffer)?size-off:sizeof(buffer)),resid=0;
  error=vn_rdwr(UIO_READ,vp,(caddr_t)buffer,count,(off_t)off,UIO_SYSSPACE,0,vfs_context_ucred(ctx),&resid,current_proc());
  if(!error&&(resid||memcmp(buffer,data+off,count)))error=EINVAL;
 }
 vnode_put(vp);return error;
}
static int x9_metal_install(proc_t p,user_addr_t scratch,user_addr_t end,
 const char *path,const unsigned char *data,size_t size,int mode)
{
 if(!x9_metal_same(path,data,size))return 0;
 vnode_t vp=NULLVP;int error=vnode_lookup(path,VNODE_LOOKUP_NOFOLLOW,&vp,vfs_context_kernel());
 if(!error){vnode_put(vp);printf("X9 METAL DEPLOY: existing file differs path=%s; conflict preserved\n",path);return EEXIST;}if(error!=ENOENT)return error;
 size_t n=strlen(path)+1;
 if(scratch>end||n+4096>end-scratch)return E2BIG;
 error=copyout(path,scratch,n);if(error)return error;
 int32_t rv=0;struct open_args o={.path=scratch,.flags=O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,.mode=mode};
 error=open(p,&o,&rv);if(error)return error;int fd=rv;
 struct fchmod_args permissions={.fd=fd,.mode=mode};error=fchmod(p,&permissions,&rv);
 for(size_t off=0;!error&&off<size;) {
  size_t count=size-off;if(count>4096)count=4096;
  error=copyout(data+off,scratch+n,count);if(error)break;
  user_ssize_t written=0;struct write_args w={.fd=fd,.cbuf=scratch+n,.nbyte=count};error=write(p,&w,&written);
  if(!error&&written!=(user_ssize_t)count)error=EIO;
  off+=count;
 }
 if(!error){struct fsync_args f={.fd=fd};error=fsync(p,&f,&rv);}
 struct close_args c={.fd=fd};int close_error=sys_close(p,&c,&rv);if(!error)error=close_error;
 if(error){struct unlink_args u={.path=scratch};(void)unlink(p,&u,&rv);}return error;
}
static int sweet_x9_metal_deploy(proc_t p,user_addr_t scratch,user_addr_t end)
{
 int enabled=0;extern boolean_t sweet_x9_ufs_root_writable(void);
 if(!PE_parse_boot_argn("x9_metal_system",&enabled,sizeof(enabled))||(enabled!=1&&enabled!=2))return 0;
 if(!sweet_early_is_x9()||!sweet_x9_ufs_root_writable()||!proc_is64bit(p))return EINVAL;
 static const char target[]="/System/Library/xpc/launchd.plist";
 static const char backup[]="/System/Library/xpc/launchd.x9-metal-pan168-before.plist";
 static const char temporary[]="/System/Library/xpc/launchd.x9-metal-pan168.tmp";
 static const char directory[]="/private/var/x9-metal-pan168";
 static const char executable[]="/private/var/x9-metal-pan168/x9-metal-probe";
 boolean_t installed=!x9_metal_same(target,x9_launchd_metal,sizeof(x9_launchd_metal));
 boolean_t previous=!installed&&!x9_metal_same(target,x9_launchd_previous,sizeof(x9_launchd_previous));
 boolean_t older=!installed&&!previous&&!x9_metal_same(target,x9_launchd_older,sizeof(x9_launchd_older));
 int error=(installed||previous||older)?0:x9_metal_same(target,x9_launchd_original,sizeof(x9_launchd_original));
 if(error){printf("X9 METAL DEPLOY: unrecognized launchd cache errno=%d; no replacement\n",error);return error;}
 printf("X9 METAL DEPLOY: pan168 predecessor=%s; unique paths, exact cache guard\n",installed?"pan168":previous?"pan167":older?"pan166":"original");
 if(scratch>end||sizeof(backup)+sizeof(temporary)+4096>end-scratch)return E2BIG;
 int32_t rv=0;error=copyout(directory,scratch,sizeof(directory));if(error)return error;
 struct mkdir_args md={.path=scratch,.mode=0755};error=mkdir(p,&md,&rv);if(error&&error!=EEXIST)return error;
 vnode_t dir=NULLVP;error=vnode_lookup(directory,VNODE_LOOKUP_NOFOLLOW,&dir,vfs_context_kernel());
 if(error)return error;boolean_t isdir=vnode_isdir(dir);vnode_put(dir);if(!isdir)return ENOTDIR;
 error=x9_metal_install(p,scratch,end,executable,x9_probe_bytes,sizeof(x9_probe_bytes),0755);if(error)return error;
 error=x9_metal_install(p,scratch,end,"/private/var/x9-metal-pan168/x9-metal-mobile",x9_probe_bytes,sizeof(x9_probe_bytes),0755);if(error)return error;
 static const char bundle[]="/System/Library/Extensions/X9MetalCompute168.bundle";
 error=copyout(bundle,scratch,sizeof(bundle));if(error)return error;
 md.path=scratch;error=mkdir(p,&md,&rv);if(error&&error!=EEXIST)return error;
 dir=NULLVP;error=vnode_lookup(bundle,VNODE_LOOKUP_NOFOLLOW,&dir,vfs_context_kernel());
 if(error)return error;isdir=vnode_isdir(dir);vnode_put(dir);if(!isdir)return ENOTDIR;
 error=x9_metal_install(p,scratch,end,"/System/Library/Extensions/X9MetalCompute168.bundle/X9MetalCompute",x9_plugin_bytes,sizeof(x9_plugin_bytes),0755);if(error)return error;
 error=x9_metal_install(p,scratch,end,"/System/Library/Extensions/X9MetalCompute168.bundle/Info.plist",x9_plugin_info,sizeof(x9_plugin_info),0644);if(error)return error;
 static const char signatures[]="/System/Library/Extensions/X9MetalCompute168.bundle/_CodeSignature";
 error=copyout(signatures,scratch,sizeof(signatures));if(error)return error;
 md.path=scratch;error=mkdir(p,&md,&rv);if(error&&error!=EEXIST)return error;
 dir=NULLVP;error=vnode_lookup(signatures,VNODE_LOOKUP_NOFOLLOW,&dir,vfs_context_kernel());
 if(error)return error;isdir=vnode_isdir(dir);vnode_put(dir);if(!isdir)return ENOTDIR;
 error=x9_metal_install(p,scratch,end,"/System/Library/Extensions/X9MetalCompute168.bundle/_CodeSignature/CodeResources",x9_plugin_resources,sizeof(x9_plugin_resources),0644);if(error)return error;
 if(installed)return 0;
 error=copyout(target,scratch,sizeof(target));if(error)return error;
 user_addr_t dest=scratch+sizeof(target);error=copyout(backup,dest,sizeof(backup));if(error)return error;
 struct link_args l={.path=scratch,.link=dest};error=link(p,&l,&rv);
 if(error==EEXIST)error=previous?x9_metal_same(backup,x9_launchd_previous,sizeof(x9_launchd_previous)):
  older?x9_metal_same(backup,x9_launchd_older,sizeof(x9_launchd_older)):
  x9_metal_same(backup,x9_launchd_original,sizeof(x9_launchd_original));
 if(error)return error;
 error=x9_metal_install(p,scratch,end,temporary,x9_launchd_metal,sizeof(x9_launchd_metal),0644);if(error)return error;
 error=copyout(temporary,scratch,sizeof(temporary));if(error)return error;
 dest=scratch+sizeof(temporary);error=copyout(target,dest,sizeof(target));if(error)return error;
 struct rename_args r={.from=scratch,.to=dest};return rename(p,&r,&rv);
}
