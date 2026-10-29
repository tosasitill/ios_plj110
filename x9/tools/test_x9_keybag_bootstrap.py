"""Compile actual bootstrap helper with bounded copyout/mkdir/VFS mocks."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'work/x9-keybag-bootstrap/kernel-source/bsd/kern/kern_exec.c'
class KeybagBootstrapTests(unittest.TestCase):
 @unittest.skipUnless(SOURCE.exists(),'requires isolated kernel source')
 def test_actual_directory_bootstrap(self):
  s=SOURCE.read_text();a=s.index('static int\nsweet_x9_prepare_keybags(');b=s.index('static int\nsweet_x9_init_cache_env(',a);helper=s[a:b]
  code=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
typedef uint64_t user_addr_t;
typedef void *proc_t;
typedef void *vnode_t;
typedef int boolean_t;
#define NULLVP ((vnode_t)0)
#define VNODE_LOOKUP_NOFOLLOW 1
struct mkdir_args {user_addr_t path;int mode;};
static unsigned char page[4096];
static int present=1,enabled=1,writable=1,wide=1,copy_error,mkdir_error,lookup_error,isdir=1,copies,mkdirs,put_count;
static int PE_parse_boot_argn(const char *key,void *out,size_t n)
{assert(!strcmp(key,"x9_bootstrap_dirs")&&n==4);memcpy(out,&enabled,n);return present;}
boolean_t sweet_x9_ufs_root_writable(void){return writable;}
static int proc_is64bit(proc_t p){(void)p;return wide;}
static int copyout(const void *s,user_addr_t d,size_t n)
{copies++;if(copy_error)return copy_error;assert(d+n<=sizeof(page));memcpy(page+d,s,n);return 0;}
static int mkdir(proc_t p,struct mkdir_args *args,int32_t *value)
{(void)p;(void)value;mkdirs++;assert(args->mode==0700&&!strcmp((char*)page+args->path,"/private/var/keybags"));return mkdir_error;}
static void *vfs_context_kernel(void){return 0;}
static int vnode_lookup(const char *path,int flags,vnode_t *vp,void *ctx)
{(void)ctx;assert(!strcmp(path,"/private/var/keybags")&&flags==VNODE_LOOKUP_NOFOLLOW);*vp=(void*)1;return lookup_error;}
static int vnode_isdir(vnode_t vp){assert(vp==(void*)1);return isdir;}
static void vnode_put(vnode_t vp){assert(vp==(void*)1);put_count++;}
'''+helper+r'''
static void reset(void)
{present=enabled=writable=wide=isdir=1;copy_error=mkdir_error=lookup_error=copies=mkdirs=put_count=0;}
int main(void)
{
 reset();assert(!sweet_x9_prepare_keybags(0,128,sizeof(page))&&mkdirs==1&&put_count==1);
 reset();mkdir_error=EEXIST;assert(!sweet_x9_prepare_keybags(0,128,sizeof(page))&&put_count==1);
 reset();enabled=0;assert(!sweet_x9_prepare_keybags(0,128,sizeof(page))&&!copies&&!mkdirs);
 reset();present=0;assert(!sweet_x9_prepare_keybags(0,128,sizeof(page))&&!copies&&!mkdirs);
 reset();writable=0;assert(sweet_x9_prepare_keybags(0,128,sizeof(page))==EROFS&&!copies&&!mkdirs);
 reset();copy_error=EFAULT;assert(sweet_x9_prepare_keybags(0,128,sizeof(page))==EFAULT&&!mkdirs);
 reset();mkdir_error=EACCES;assert(sweet_x9_prepare_keybags(0,128,sizeof(page))==EACCES&&!put_count);
 reset();isdir=0;assert(sweet_x9_prepare_keybags(0,128,sizeof(page))==ENOTDIR&&put_count==1);
 reset();lookup_error=ENOENT;assert(sweet_x9_prepare_keybags(0,128,sizeof(page))==ENOENT&&!put_count);
 reset();assert(sweet_x9_prepare_keybags(0,4090,sizeof(page))==E2BIG&&!copies);
 reset();wide=0;assert(sweet_x9_prepare_keybags(0,128,sizeof(page))==E2BIG&&!copies);
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'check.c').write_text(code)
   subprocess.run(['/opt/homebrew/opt/llvm/bin/clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'check.c'),'-o',str(p/'check')],check=True)
   subprocess.run([str(p/'check')],check=True)
if __name__=='__main__':unittest.main()
