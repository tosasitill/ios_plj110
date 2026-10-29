"""Exercise actual X9 AMFI policy and entitlement grant with bounded copy mocks."""
from pathlib import Path
import re,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2];SRC=ROOT/'work/x9-userspace-compat/kernel-source/bsd/kern/sweet_amfi.c'
def func(s,needle):
 a=s.index(needle);i=s.index('{',a)+1;depth=1
 while depth:
  if s[i]=='{':depth+=1
  if s[i]=='}':depth-=1
  i+=1
 return s[a:i]
class PolicyTests(unittest.TestCase):
 def test_actual_policy_grants_and_fails_closed(self):
  s=SRC.read_text();start=s.index('static const struct {\n\tint priv;\n\tconst char *entitlement;\n} sweet_x9_privileges');end=s.index('#define SWEET_PRIV_LOG_MAX',start);table=s[start:end];ids=list(dict.fromkeys(re.findall(r'PRIV_[A-Z0-9_]+',table)));defs='\n'.join('#define '+name+' '+str(i+100) for i,name in enumerate(ids));policy=func(s,'static int\nsweet_x9_amfi_policy_syscall(');grant=func(s,'static int\nsweet_sandbox_priv_grant(')
  prefix=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define KERN_SUCCESS 0
#define KERN_DENIED 8
#define PRIV_WORK_INTERVAL 9999
typedef void *kauth_cred_t;struct proc {int dummy;};
static struct proc process;static int x9=1,has_entitlement,copyin_error,copyout_error;static const char *granted;static char cred1,cred2;
static int sweet_early_is_x9(void){return x9;}
static kauth_cred_t kauth_cred_get(void){return &cred1;}
static struct proc *current_proc(void){return &process;}
static int sweet_amfi_query_boolean_with_proc(struct proc *p,const char *name){assert(p==&process);return has_entitlement&&granted&&!strcmp(granted,name)?KERN_SUCCESS:KERN_DENIED;}
static int copyin(user_addr_t p,void *out,size_t n){if(copyin_error)return EFAULT;memcpy(out,(void*)p,n);return 0;}
static int copyout(const void *in,user_addr_t p,size_t n){if(copyout_error)return EFAULT;memcpy((void*)p,in,n);return 0;}
static void proc_selfname(char *p,size_t n){snprintf(p,n,"test");}
#define printf(...) (0)
'''
  after=r'''
#define SWEET_PRIV_LOG_MAX 64
static struct {int priv;char name[20];} sweet_priv_logged[SWEET_PRIV_LOG_MAX];static int sweet_priv_logged_count;
'''
  main=r'''
int main(void){uint64_t value=0;struct {uint64_t in;user_addr_t out;} request={0,(user_addr_t)&value};
 assert(sweet_x9_amfi_policy_syscall(&process,0x60,(user_addr_t)&value)==0&&value==0x86);
 value=0;assert(sweet_x9_amfi_policy_syscall(&process,0x5a,(user_addr_t)&request)==0&&value==0xff);
 copyin_error=1;assert(sweet_x9_amfi_policy_syscall(&process,0x5a,(user_addr_t)&request)==EFAULT);copyin_error=0;
 copyout_error=1;assert(sweet_x9_amfi_policy_syscall(&process,0x60,(user_addr_t)&value)==EFAULT);copyout_error=0;
 assert(sweet_x9_amfi_policy_syscall(&process,33,(user_addr_t)&value)==ENOPOLICY);
 assert(sweet_sandbox_priv_grant(&cred1,PRIV_WORK_INTERVAL)==0);
 has_entitlement=1;granted="com.apple.private.kernel.global-proc-info";
 assert(sweet_sandbox_priv_grant(&cred1,PRIV_GLOBAL_PROC_INFO)==0);
 assert(sweet_sandbox_priv_grant(&cred2,PRIV_GLOBAL_PROC_INFO)==EPERM);
 assert(sweet_sandbox_priv_grant(&cred1,PRIV_PROC_UUID_POLICY)==EPERM);
 has_entitlement=0;assert(sweet_sandbox_priv_grant(&cred1,PRIV_GLOBAL_PROC_INFO)==EPERM);
 x9=0;has_entitlement=1;assert(sweet_sandbox_priv_grant(&cred1,PRIV_GLOBAL_PROC_INFO)==EPERM);
 assert(sweet_x9_amfi_policy_syscall(&process,0x60,(user_addr_t)&value)==ENOPOLICY);
 return 0;}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'policy.c').write_text(prefix+defs+'\n'+policy+'\n'+table+after+grant+main)
   subprocess.run(['/opt/homebrew/opt/llvm/bin/clang','-std=gnu11','-O1','-fsanitize=address,undefined',str(p/'policy.c'),'-o',str(p/'policy')],check=True);subprocess.run([str(p/'policy')],check=True)
if __name__=='__main__':unittest.main()
