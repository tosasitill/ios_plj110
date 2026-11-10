/* Add the original CBN bundle before PID1 execs launchd. Existing CMCC and
 * conflicting files/links are never replaced. No loader/signature/identity patch.
 * A partial install does not publish the MCCMNC alias or claim success. */
#include "../../../generated/x9_carrier_cbn.inc"
#if X9_CBN_PRESENT
static int x9_cbn_alias(proc_t p,user_addr_t scratch,user_addr_t end,int create)
{
 static const char path[]="/System/Library/Carrier Bundles/iPhone/46015";
 static const char target[]="CBN_cn.bundle";
 if(scratch>end||sizeof(path)+sizeof(target)+64>end-scratch)return E2BIG;
 user_addr_t buffer=scratch+sizeof(path);
 int error=copyout(path,scratch,sizeof(path));if(error)return error;
 int32_t rv=0;
 struct readlink_args read_request={.path=scratch,.buf=buffer,.count=64};
 error=readlink(p,&read_request,&rv);
 if(!error){
  char current[64];
  if(rv!=(int32_t)(sizeof(target)-1))return EEXIST;
  error=copyin(buffer,current,(size_t)rv);if(error)return error;
  return memcmp(current,target,sizeof(target)-1)?EEXIST:0;
 }
 if(error!=ENOENT)return error;
 if(create==2)return ENOENT;
 if(!create)return 0;
 error=copyout(target,buffer,sizeof(target));if(error)return error;
 struct symlink_args link_request={.path=buffer,.link=scratch};
 error=symlink(p,&link_request,&rv);
 if(error&&error!=EEXIST)return error;
 return x9_cbn_alias(p,scratch,end,2);
}
static int x9_cbn_install(proc_t p,user_addr_t scratch,user_addr_t end)
{
 if(!sweet_early_is_x9()||sweet_x9_arg("x9_bb",0)!=1||sweet_x9_arg("x9_carrier_cbn",1)!=1||proc_getpid(p)!=1)return 0;
 if(!sweet_x9_ufs_root_active()||!sweet_x9_ufs_root_writable())return EROFS;
 if((size_t)(x9_cbn_pack_end-x9_cbn_pack)!=X9_CBN_PACK_BYTES)return EINVAL;
 int error=x9_cbn_alias(p,scratch,end,0);if(error)return error;
 unsigned created=0,kept=0;
 for(unsigned i=0;i<sizeof(x9_cbn_files)/sizeof(x9_cbn_files[0]);i++){
  const struct x9_cbn_file *file=&x9_cbn_files[i];
  if(file->offset>X9_CBN_PACK_BYTES||file->bytes>X9_CBN_PACK_BYTES-file->offset)return EINVAL;
  const uint8_t *data=x9_cbn_pack+file->offset;uint8_t sha[32];SHA256_CTX hash;
  SHA256_Init(&hash);SHA256_Update(&hash,data,file->bytes);SHA256_Final(sha,&hash);
  if(memcmp(sha,file->sha,32))return EIO;
  char parent[256];size_t n=strlen(file->path);if(n>=sizeof(parent))return E2BIG;
  memcpy(parent,file->path,n+1);char *slash=strrchr(parent,'/');if(!slash)return EINVAL;*slash=0;
  error=x9_bbfw_directory(p,scratch,end,parent);if(error)return error;
  int made=0;error=x9_bbfw_write(p,scratch,end,file->path,data,file->bytes,file->sha,&made);
  if(error){printf("X9 CBN: file %u conflict/write errno=%d; alias not published\n",i,error);return error;}
  error=x9_bbfw_equal(file->path,file->sha,file->bytes);if(error)return error;
  created+=(unsigned)made;kept+=(unsigned)!made;
 }
 error=x9_cbn_alias(p,scratch,end,1);
 if(!error)printf("X9 CBN: original 54.1.0 bundle/IPCC verified created=%u kept=%u; 46015 alias ready (D22 original overrides)\n",created,kept);
 return error;
}
#else
static int x9_cbn_install(proc_t p __unused,user_addr_t scratch __unused,user_addr_t end __unused){return 0;}
#endif
