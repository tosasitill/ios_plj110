"""Test actual selector31 DER response and buffer handling."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];SOURCE=ROOT/'work/x9-userspace-compat/kernel-source/iokit/Kernel/arm/SweetKeyStore.cpp'
def function(s,needle):
 a=s.index(needle);i=s.index('{',a)+1;d=1
 while d:
  if s[i]=='{':d+=1
  if s[i]=='}':d-=1
  i+=1
 return s[a:i]
class AKSConfigTests(unittest.TestCase):
 def test_actual_der_output_and_short_buffer(self):
  s=SOURCE.read_text();helpers='\n'.join(function(s,x) for x in ['static IOReturn\nsweet_aks_struct_out(','static uint32_t\nsweet_aks_der_pair(','static IOReturn\nsweet_aks_configuration(']);prefix=r'''
#include <cstdint>
#include <cstring>
#include <cassert>
using IOReturn=int;
#define kIOReturnNoSpace -1
#define kIOReturnSuccess 0
struct Descriptor {unsigned len;uint8_t *out;unsigned getLength(){return len;}void writeBytes(unsigned off,const void *p,unsigned n){assert(off+n<=len);memcpy(out+off,p,n);}};
struct IOExternalMethodArguments {Descriptor *structureOutputDescriptor;uint32_t structureOutputDescriptorSize;void *structureOutput;uint32_t structureOutputSize;};
''';main=r'''
int main(){const uint8_t expected[]={0x31,0x1c,0x30,0x07,0x0c,0x02,'b','d',0x02,0x01,0,0x30,0x07,0x0c,0x02,'g','p',0x02,0x01,0,0x30,0x08,0x0c,0x03,'m','u','a',0x02,0x01,10};
 uint8_t output[128]={};IOExternalMethodArguments a={nullptr,0,output,sizeof(output)};assert(sweet_aks_configuration(&a)==0);assert(a.structureOutputSize==sizeof(expected)&&!memcmp(output,expected,sizeof(expected)));
 a.structureOutputSize=3;assert(sweet_aks_configuration(&a)==-1);
 Descriptor d{sizeof(output),output};a.structureOutputDescriptor=&d;assert(sweet_aks_configuration(&a)==0&&a.structureOutputDescriptorSize==sizeof(expected));d.len=3;assert(sweet_aks_configuration(&a)==-1);return 0;}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'aks.cpp').write_text(prefix+helpers+main);subprocess.run(['/opt/homebrew/opt/llvm/bin/clang++','-std=c++17','-fsanitize=address,undefined',str(p/'aks.cpp'),'-o',str(p/'aks')],check=True);subprocess.run([str(p/'aks')],check=True)
if __name__=='__main__':unittest.main()
