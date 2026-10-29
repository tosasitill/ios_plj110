"""Actual dali-derived line framing with deterministic host counter reads."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];SOURCE=ROOT/'work/x9-dali-compat/kernel-source/bsd/dev/sweet_ufs.c'
class TimestampTests(unittest.TestCase):
 @unittest.skipUnless(SOURCE.exists(),'requires isolated source')
 def test_chunked_lines_and_non_x9_passthrough(self):
  s=SOURCE.read_text();a=s.index('static uint64_t sweet_log_t0;');b=s.index('/*\n * Copy console bytes',a);helper=s[a:b]
  helper=helper.replace('__asm__ volatile ("isb" ::: "memory");','((void)0);').replace('__asm__ volatile ("mrs %0, cntpct_el0" : "=r"(cnt));','cnt=counter;').replace('__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(frq));','frq=1000000;')
  prefix=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
static uint64_t counter=1000000;static bool x9=true;static char text[4096];static size_t used;
static bool sweet_early_is_x9(void){return x9;}
static void sweet_log_put(const char *p,int n){if(n<=0)return;assert(used+n<sizeof(text));memcpy(text+used,p,n);used+=n;text[used]=0;}
'''
  main=r'''
int main(void) {
 sweet_log_append("first",5);counter+=1000;sweet_log_append(" line\nnext\n",11);
 assert(!strcmp(text,"[     0.000] first line\n[     0.001] next\n"));
 used=0;text[0]=0;x9=false;sweet_log_append("plain\n",6);assert(!strcmp(text,"plain\n"));
 sweet_log_append(0,0);return 0;
}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'log.c').write_text(prefix+helper+main)
   subprocess.run(['/opt/homebrew/opt/llvm/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'log.c'),'-o',str(p/'log')],check=True)
   subprocess.run([str(p/'log')],check=True)
if __name__=='__main__':unittest.main()
