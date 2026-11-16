/* Hash-addressed own code, opt-in only. No Apple executable/cache/plist
 * replacement. Preserve caller-supplied DYLD chains; no duplicate env key. */
#include "../../../generated/x9_motion_bridge.inc"
static bool x9_motion_bridge_installed;
static int x9_motion_mode(void)
{ return sweet_x9_arg("x9_motion",(sweet_x9_arg("x9s",0)&16)?3:0); }
static int x9_motion_install(proc_t p,user_addr_t scratch,user_addr_t end)
{
 if (!sweet_early_is_x9() || x9_motion_mode()!=3 || proc_getpid(p)!=1) return 0;
 if (!sweet_x9_ufs_root_active() || !sweet_x9_ufs_root_writable()) return EROFS;
 int e=x9_bbfw_directory(p,scratch,end,"/private/var/x9/motion");if(e)return e;
 int created=0;
 e=x9_bbfw_write(p,scratch,end,X9_MOTION_BRIDGE_PATH,x9_motion_bridge_data,
     sizeof(x9_motion_bridge_data),x9_motion_bridge_sha,&created);
 if (!e) e=x9_bbfw_equal(X9_MOTION_BRIDGE_PATH,x9_motion_bridge_sha,sizeof(x9_motion_bridge_data));
 if (!e) __atomic_store_n(&x9_motion_bridge_installed,true,__ATOMIC_RELEASE);
 printf("X9 MOTION: own-code bridge install created=%d errno=%d\n",created,e);return e;
}
static int x9_motion_exec_env(struct image_params *imgp,int ptr_size)
{
 if (!sweet_early_is_x9() || x9_motion_mode()!=3 ||
     !__atomic_load_n(&x9_motion_bridge_installed,__ATOMIC_ACQUIRE) ||
     strcmp(imgp->ip_strings+strlen(EXECUTABLE_KEY),"/usr/libexec/backboardd")) return 0;
 const char *s=imgp->ip_endargv;
 for (int i=0;i<imgp->ip_envc;i++,s+=strlen(s)+1)
  if (!strncmp(s,"DYLD_INSERT_LIBRARIES=",22)) {
   printf("X9 MOTION: existing injection chain preserved; bridge not appended\n");return 0;
  }
 if (x9_bbfw_equal(X9_MOTION_BRIDGE_PATH,x9_motion_bridge_sha,sizeof(x9_motion_bridge_data))) return 0;
 static const char env[]="DYLD_INSERT_LIBRARIES=" X9_MOTION_BRIDGE_PATH;
 if (imgp->ip_argspace<sizeof(env)+(size_t)ptr_size) return E2BIG;
 int e=exec_add_user_string(imgp,CAST_USER_ADDR_T(env),UIO_SYSSPACE,TRUE);
 if (!e) {
  imgp->ip_argspace-=ptr_size;imgp->ip_envc++;
  printf("X9 MOTION: backboardd orientation adapter environment appended\n");
 }
 return e;
}
