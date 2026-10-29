"""Test transplanted execswap framing and exact noexec matches; no process execution."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];SRC=ROOT/'work/x9-dali-compat/kernel-source/bsd/kern'
def function(source,needle):
 a=source.index(needle);b=source.index('{',a);depth=1;i=b+1
 while depth:
  if source[i]=='{':depth+=1
  if source[i]=='}':depth-=1
  i+=1
 return source[a:i]
class DaliExecTests(unittest.TestCase):
 @unittest.skipUnless((SRC/'sweet_execswap.h').exists(),'needs isolated imported helpers')
 def test_parser_and_inactive_defaults(self):
  s=(SRC/'kern_exec.c').read_text();block=function(s,'static bool\nsweet_exec_blocked(')
  prefix=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
static const char *boot;
static int PE_parse_boot_argn(const char *name,char *out,size_t n)
{assert(!strcmp(name,"sweetnoexec"));if(!boot||!boot[0])return 0;snprintf(out,n,"%s",boot);return 1;}
'''
  main=r'''
int main(int argc,char **argv) {
 assert(argc==2);boot=argv[1];
 assert(sweet_exec_blocked("alpha",false)==(boot[0]!=0));
 assert(!sweet_exec_blocked("alphabet",false));assert(!sweet_exec_blocked("beta1",false));
 struct sweet_execswap swap={0};
 assert(!sweet_execswap_parse(&swap)&&!sweet_execswap_match(&swap,"/sbin/launchd"));
 const char *invalid[]={"abc","abc:relative","abc:/","abc:/some path","a,b:/tmp/x","/abc:/tmp/x","abc:/tmp/x:y"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){memset(&swap,0,sizeof(swap));strcpy(swap.arg,invalid[i]);assert(!sweet_execswap_parse(&swap));}
 memset(&swap,0,sizeof(swap));strcpy(swap.arg,"alpha:/usr/local/libexec/diag");assert(sweet_execswap_parse(&swap));
 assert(!strcmp(sweet_execswap_match(&swap,"/usr/bin/alpha"),"/usr/local/libexec/diag"));
 assert(!sweet_execswap_match(&swap,"/usr/bin/alphabet"));
 memset(&swap,'x',sizeof(swap));assert(!sweet_execswap_parse(&swap));return 0;
}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'controls.c').write_text(prefix+block+(SRC/'sweet_execswap.h').read_text()+main)
   subprocess.run(['/opt/homebrew/opt/llvm/bin/clang','-std=gnu11','-O1','-fsanitize=address,undefined',str(p/'controls.c'),'-o',str(p/'controls')],check=True)
   for args in ['', 'alpha,beta']:subprocess.run([str(p/'controls'),args],check=True)
if __name__=='__main__':unittest.main()
