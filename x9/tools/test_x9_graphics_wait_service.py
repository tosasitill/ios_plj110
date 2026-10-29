"""Exercise actual passive publication block; no hardware/MMIO emulation."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'work/x9-cryptex-graphics/kernel-source/iokit/Kernel/arm/SweetFramebuffer.cpp'
class GraphicsWaitServiceTests(unittest.TestCase):
 def test_actual_publication_is_passive_and_respects_disable(self):
  s=SOURCE.read_text();a=s.index('\tuint32_t accelOn = 1;');b=s.index('\n\tOSSafeReleaseNULL(accel);',a)+len('\n\tOSSafeReleaseNULL(accel);');block=s[a:b]
  prefix=r'''
#include <cstring>
#include <cstdint>
#include <cassert>
#include <new>
static bool board_x9=true,board_qcom=false,init_ok=true,attach_ok=true,alloc_ok=true;
static unsigned enabled=1;static int published,released,properties;
#define IOLog(...) ((void)0)
struct IOService {
 const char *name="";virtual ~IOService(){released++;}
 bool init(){return init_ok;}bool attach(IOService *){return attach_ok;}
 void setName(const char *s){name=s;}
 void setProperty(const char *,const char *){properties++;}
 void registerService(){assert(!strcmp(name,"sgx"));published++;}
};
struct IOAcceleratorES:IOService {};
template<class T>static T *allocate(){return alloc_ok?new(std::nothrow)T:nullptr;}
#define OSTypeAlloc(type) allocate<type>()
#define OSSafeReleaseNULL(p) do{delete(p);(p)=nullptr;}while(0)
static bool sweet_board_x9(){return board_x9;}
static bool sweet_board_qcom(){return board_qcom;}
static bool PE_parse_boot_argn(const char *k,void *p,unsigned n){assert(!strcmp(k,"sweetaccel")&&n==4);memcpy(p,&enabled,n);return true;}
struct Framebuffer:IOService {void publish(){
'''
  main=r'''
}};
static void reset(){published=released=properties=0;board_x9=init_ok=attach_ok=alloc_ok=true;board_qcom=false;enabled=1;}
int main(){Framebuffer fb;
 reset();fb.publish();assert(published==1&&released==1&&properties==0);
 reset();board_x9=false;fb.publish();assert(published==0&&released==1);
 reset();board_x9=false;board_qcom=true;fb.publish();assert(published==1&&properties==0);
 reset();enabled=0;fb.publish();assert(published==0&&released==0);
 reset();alloc_ok=false;fb.publish();assert(published==0&&released==0);
 reset();init_ok=false;fb.publish();assert(published==0&&released==1);
 reset();attach_ok=false;fb.publish();assert(published==0&&released==1);
 return 0;}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'service.cpp').write_text(prefix+block+main)
   subprocess.run(['/opt/homebrew/opt/llvm/bin/clang++','-std=c++17','-Wall','-Wextra','-Werror','-O1','-fsanitize=address,undefined',str(p/'service.cpp'),'-o',str(p/'service')],check=True);subprocess.run([str(p/'service')],check=True)
if __name__=='__main__':unittest.main()
