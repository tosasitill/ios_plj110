"""Exercise actual imported CoreAudio interval cases with thread-type mocks."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];SRC=ROOT/'work/x9-userspace-compat/kernel-source/bsd/kern/sys_work_interval.c'
class WorkIntervalTests(unittest.TestCase):
 def test_audio_only_and_bounds(self):
  s=SRC.read_text();a=s.index('\t/* hackios: CoreAudio only;');b=s.index('\n\tdefault:',a);cases=s[a:b].replace('\n#endif','')
  code=r'''
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#define USER_ADDR_NULL 0
#define WORK_INTERVAL_TYPE_COREAUDIO 0x10000000u
struct args {uint64_t arg,work_interval_id;};
static int x9=1;static uint32_t type=WORK_INTERVAL_TYPE_COREAUDIO;
static int sweet_early_is_x9(void){return x9;}
static void *current_thread(void){return 0;}
static uint32_t kern_work_interval_type_of_thread(void *t){(void)t;return type;}
static int run(int operation,struct args *uap){switch(operation){
'''+cases+r'''
 default:return ENOTSUP;}return 0;}
int main(void){struct args a={8,9};for(int op=6;op<=8;op++)assert(!run(op,&a));
 type=0;assert(run(6,&a)==ENOTSUP);type=WORK_INTERVAL_TYPE_COREAUDIO;x9=0;assert(run(7,&a)==ENOTSUP);
 x9=1;a.arg=0;assert(run(8,&a)==EINVAL);a.arg=8;a.work_interval_id=0;assert(run(6,&a)==EINVAL);assert(run(9,&a)==ENOTSUP);return 0;}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'wi.c').write_text(code);subprocess.run(['/opt/homebrew/opt/llvm/bin/clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'wi.c'),'-o',str(p/'wi')],check=True);subprocess.run([str(p/'wi')],check=True)
if __name__=='__main__':unittest.main()
