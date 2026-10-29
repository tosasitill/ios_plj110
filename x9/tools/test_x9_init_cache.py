#!/usr/bin/env python3
"""Host checks for the actual kernel PID1 environment helper (copyout mocked)."""
from pathlib import Path
import subprocess, tempfile, unittest

root=Path(__file__).resolve().parents[2]
source_path=root/'work/x9-cryptex/kernel-source/bsd/kern/kern_exec.c'
prefix=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
typedef uint64_t user_addr_t;
typedef uint64_t user64_addr_t;
typedef void *proc_t;
#ifndef USER_ADDR_NULL
#define USER_ADDR_NULL 0
#endif
#define USER_ADDR_ALIGN(a,n) (((a)+(n)-1)&~((n)-1))
static unsigned char page[16384];
static uint64_t dyld_flags=0x40, published;
static void commpage_update_dyld_flags(uint64_t flags) { published=flags; }
static int width=1, native=1, present=1, enabled=1, calls, fail_at;
static int proc_is64bit(proc_t p) { (void)p; return width; }
static int sweet_x9_ufs_root_active(void) { return native; }
static int PE_parse_boot_argn(const char *key, void *out, int n) {
 assert(!strcmp(key,"x9_init_cache") && n==sizeof(enabled));
 memcpy(out,&enabled,n); return present;
}
static int copyout(const void *src, user_addr_t dst, size_t n) {
 ++calls; if (calls==fail_at) return EFAULT;
 assert(dst<=sizeof(page) && n<=sizeof(page)-dst);
 memcpy(page+dst,src,n); return 0;
}
'''
main=r'''
int main(void) {
 user_addr_t env=999; int checks=0;
 assert(!sweet_x9_init_cache_env(0,73,sizeof(page),&env));
 assert(env%8==0 && env>73 && calls==2);
 assert(dyld_flags==0x48 && published==0x48);
 uint64_t vector[2]; memcpy(vector,page+env,sizeof(vector));
 assert(vector[0]==73 && vector[1]==0);
 assert(!strcmp((char*)page+73,"DYLD_SHARED_CACHE_DIR=/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld"));
 ++checks;
 for(int which=0;which<5;which++) {
  width=native=present=enabled=1;
  if(which==0)width=0;
  if(which==1)native=0;
  if(which==2)present=0;
  if(which==3)enabled=0;
  if(which==4)enabled=2;
  calls=0;env=999;
  assert(!sweet_x9_init_cache_env(0,73,sizeof(page),&env));
  assert(!env && !calls); ++checks;
 }
 width=native=present=enabled=1;
 for(int which=1;which<=2;which++) {
  calls=0;fail_at=which;env=999;
  assert(sweet_x9_init_cache_env(0,73,sizeof(page),&env)==EFAULT);
  assert(!env && calls==which);++checks;
 }
 fail_at=0;
 for(int which=0;which<3;which++) {
  calls=0;env=999;
  user_addr_t cursor=which==0?sizeof(page)+1:which==1?sizeof(page)-1:sizeof(page)-90;
  assert(sweet_x9_init_cache_env(0,cursor,sizeof(page),&env)==E2BIG);
  assert(!env && !calls);++checks;
 }
 printf("PID1 environment helper: %d checks passed (actual helper source, mocked copyout).\n",checks);
 return 0;
}
'''
class InitCacheTests(unittest.TestCase):
    @unittest.skipUnless(source_path.exists(), "frozen kernel source unavailable")
    def test_initial_environment_and_failures(self):
        source=source_path.read_text()
        start=source.index('static int\nsweet_x9_init_cache_env(')
        end=source.index('\n#endif',start)
        helper=source[start:end]
        with tempfile.TemporaryDirectory() as td:
            c=Path(td)/'check.c';exe=Path(td)/'check'
            c.write_text(prefix+helper+main)
            subprocess.run(['/opt/homebrew/opt/llvm/bin/clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__ == '__main__':
    unittest.main()
