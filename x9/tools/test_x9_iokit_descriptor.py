"""Exercise actual descriptor output helpers with a strict wired-state model."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];SRC=ROOT/'work/x9-iokit-descriptor/kernel-source/iokit/Kernel/arm'
def function(s,needle):
 a=s.index(needle);i=s.index('{',a)+1;depth=1
 while depth:
  if s[i]=='{':depth+=1
  if s[i]=='}':depth-=1
  i+=1
 return s[a:i]
class DescriptorTests(unittest.TestCase):
 def test_actual_key_and_surface_output_prepare_and_failures(self):
  helpers=[]
  for file,name in [('SweetKeyStore.cpp','sweet_aks_struct_out'),('SweetSurface.cpp','sweet_struct_out')]:
   helpers.append(function((SRC/file).read_text(),'static IOReturn\n'+name+'('))
  code=r'''
#include <cstdint>
#include <cstring>
#include <cassert>
using IOReturn=int;using IOByteCount=unsigned long;
#define kIOReturnSuccess 0
#define kIOReturnNoSpace -1
#define kIOReturnUnderrun -2
struct IOMemoryDescriptor {unsigned char buf[128]={};unsigned len=128;int prep_error=0,complete_error=0;bool wired=false,short_write=false;int prep=0,done=0,writes=0;
 unsigned getLength(){return len;}
 int prepare(){prep++;if(prep_error)return prep_error;assert(!wired);wired=true;return 0;}
 unsigned long writeBytes(unsigned long off,const void *p,unsigned long n){assert(wired&&off+n<=len);writes++;if(short_write)n--;memcpy(buf+off,p,n);return n;}
 int complete(){assert(wired);wired=false;done++;return complete_error;}
};
struct IOExternalMethodArguments {IOMemoryDescriptor *structureOutputDescriptor=nullptr;unsigned structureOutputDescriptorSize=0;void *structureOutput=nullptr;unsigned structureOutputSize=0;};
'''+ '\n'.join(helpers)+r'''
using Fn=int(*)(IOExternalMethodArguments*,const void*,uint32_t);
void test(Fn fn){const char value[]="abc";uint8_t out[4]={};IOExternalMethodArguments a;
 a.structureOutput=out;a.structureOutputSize=4;assert(fn(&a,value,4)==0&&!memcmp(out,value,4));a.structureOutputSize=1;assert(fn(&a,value,4)==-1);
 IOMemoryDescriptor d;a.structureOutputDescriptor=&d;assert(fn(&a,value,4)==0&&d.prep==1&&d.done==1&&d.writes==1&&!d.wired&&!memcmp(d.buf,value,4));
 d.len=1;assert(fn(&a,value,4)==-1&&d.prep==1);d.len=128;d.prep_error=7;assert(fn(&a,value,4)==7&&d.done==1);d.prep_error=0;
 d.short_write=true;assert(fn(&a,value,4)==-2&&!d.wired&&d.done==2);d.short_write=false;d.complete_error=8;assert(fn(&a,value,4)==8&&!d.wired&&d.done==3);
}
int main(){test(sweet_aks_struct_out);test(sweet_struct_out);return 0;}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'test.cpp').write_text(code);subprocess.run(['/opt/homebrew/opt/llvm/bin/clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
