"""Actual kernel alias helper against a temporary POSIX namespace, ASan/UBSan.
No Apple/kernel binary is executed; tests exercise path creation/ownership only.
"""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'work/x9-cryptex-graphics/kernel-source/bsd/kern/kern_exec.c'
ALIAS='usr/lib/libobjc-trampolines.dylib';TARGET='private/preboot/Cryptexes/OS/'+ALIAS
class CryptexAliasTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();cls.dir=Path(cls.tmp.name);s=SOURCE.read_text();a=s.index('static int\nsweet_x9_prepare_objc_trampolines(');b=s.index('static int\nsweet_x9_init_cache_env(',a);helper=s[a:b]
  prefix=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
typedef uint64_t user_addr_t;typedef void *proc_t;typedef int boolean_t;
struct node {dev_t dev;ino_t ino;mode_t mode;unsigned refs;};typedef struct node *vnode_t;
#define NULLVP ((vnode_t)0)
#define VNODE_LOOKUP_NOFOLLOW 1
struct symlink_args {user_addr_t path,link;};
static struct node nodes[16];static unsigned nn,creates;static unsigned char page[4096];
static const char *root,*scenario;static int writable=1,wide=1,enabled=1;
static void mapped(char *out,const char *s){assert(s[0]=='/');assert(snprintf(out,4096,"%s%s",root,s)<4096);}
static int PE_parse_boot_argn(const char *k,void *v,size_t n){assert(!strcmp(k,"x9_cryptex_aliases")&&n==sizeof(enabled));memcpy(v,&enabled,n);return 1;}
boolean_t sweet_x9_ufs_root_writable(void){return writable;}
static int proc_is64bit(proc_t p){(void)p;return wide;}
static void *vfs_context_kernel(void){return 0;}
static int vnode_lookup(const char *p,int flags,vnode_t *out,void *ctx){
 (void)ctx;char path[4096];mapped(path,p);struct stat st;
 if((flags?lstat(path,&st):stat(path,&st)))return errno;
 unsigned i;for(i=0;i<nn;i++)if(nodes[i].dev==st.st_dev&&nodes[i].ino==st.st_ino)break;
 if(i==nn){assert(nn<16);nodes[nn++]=(struct node){st.st_dev,st.st_ino,st.st_mode,0};}
 nodes[i].refs++;*out=&nodes[i];return 0;
}
static int vnode_isreg(vnode_t n){return S_ISREG(n->mode);}
static int vnode_islnk(vnode_t n){return S_ISLNK(n->mode);}
static void vnode_put(vnode_t n){assert(n->refs);n->refs--;}
static int copyout(const void *s,user_addr_t dest,size_t n){if(!strcmp(scenario,"copy-error"))return EFAULT;assert(dest+n<=sizeof(page));memcpy(page+dest,s,n);return 0;}
static int kernel_symlink(proc_t p,struct symlink_args *a,int32_t *v){
 (void)p;(void)v;char target[4096],alias[4096];mapped(target,(char*)page+a->path);mapped(alias,(char*)page+a->link);creates++;
 if(!strcmp(scenario,"create-error"))return EACCES;
 if(!strcmp(scenario,"race-wrong"))mapped(target,"/usr/lib/other");
 int ret=symlinkat(target,AT_FDCWD,alias);if(ret)return errno;
 if(!strcmp(scenario,"race")||!strcmp(scenario,"race-wrong"))return EEXIST;
 return 0;
}
#define symlink kernel_symlink
'''
  main=r'''
int main(int argc,char **argv){assert(argc==4);root=argv[1];scenario=argv[2];int want=atoi(argv[3]);
 if(!strcmp(scenario,"disabled"))enabled=0;
 if(!strcmp(scenario,"readonly"))writable=0;
 if(!strcmp(scenario,"32bit"))wide=0;
 user_addr_t end=!strcmp(scenario,"small")?132:sizeof(page);
 int got=sweet_x9_prepare_objc_trampolines(0,128,end);assert(got==want);
 for(unsigned i=0;i<nn;i++)assert(nodes[i].refs==0);
 if(!want&&!strcmp(scenario,"new"))assert(creates==1);
 return 0;}
'''
  (cls.dir/'alias.c').write_text(prefix+helper+main);cls.exe=cls.dir/'alias'
  subprocess.run(['/opt/homebrew/opt/llvm/bin/clang','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-fsanitize=address,undefined',str(cls.dir/'alias.c'),'-o',str(cls.exe)],check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def test_actual_helper_cases(self):
  import errno
  cases={'new':0,'existing-link':0,'existing-file':0,'wrong-link':errno.EEXIST,'directory':errno.EEXIST,'target-missing':errno.ENOENT,'target-directory':errno.EINVAL,'disabled':0,'readonly':errno.EROFS,'32bit':errno.E2BIG,'small':errno.E2BIG,'copy-error':errno.EFAULT,'create-error':errno.EACCES,'race':0,'race-wrong':errno.EEXIST}
  for name,want in cases.items():
   with self.subTest(name=name),tempfile.TemporaryDirectory() as td:
    p=Path(td);target=p/TARGET;alias=p/ALIAS;target.parent.mkdir(parents=True);alias.parent.mkdir(parents=True)
    if name=='target-directory':target.mkdir()
    elif name!='target-missing':target.write_bytes(b'untouched-source-payload')
    other=alias.with_name('other');other.write_bytes(b'existing-other-file')
    if name=='existing-link':alias.symlink_to(target)
    if name=='wrong-link':alias.symlink_to(other)
    if name=='existing-file':alias.write_bytes(b'existing-canonical-file')
    if name=='directory':alias.mkdir()
    subprocess.run([str(self.exe),str(p),name,str(want)],check=True,capture_output=True)
    if name in ['new','existing-link','race']:self.assertTrue(alias.samefile(target));self.assertEqual(alias.read_bytes(),b'untouched-source-payload')
    if name=='existing-file':self.assertEqual(alias.read_bytes(),b'existing-canonical-file')
    if name=='wrong-link':self.assertEqual(alias.resolve(),other.resolve())
    if name not in ['target-directory','target-missing']:self.assertEqual(target.read_bytes(),b'untouched-source-payload')
if __name__=='__main__':unittest.main()
