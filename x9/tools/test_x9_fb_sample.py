"""Validate actual framebuffer sampling helper on bounded padded frames."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];SRC=ROOT/'work/x9-display-probe/kernel-source/iokit/Kernel/arm/SweetFramebuffer.cpp'
class SampleTests(unittest.TestCase):
 def test_actual_grid_bounds_and_color(self):
  s=SRC.read_text();a=s.index('struct x9_fb_sample {');b=s.index('static uint32_t x9_fb_probe_count;',a);helper=s[a:b]
  code=r'''
#include <cstdint>
#include <cstring>
#include <vector>
#include <cassert>
'''+helper+r'''
int main(){
 auto z=x9_fb_sample_grid(nullptr,1,1,4);assert(z.samples==0);
 uint8_t single[4]={0,0,0,0xff};z=x9_fb_sample_grid(single,1,1,4);assert(z.samples==1024&&z.nonblack==0&&z.opaque==1024);
 single[0]=0x22;z=x9_fb_sample_grid(single,1,1,4);assert(z.nonblack==1024);
 assert(x9_fb_sample_grid(single,2,1,4).samples==0);
 unsigned w=1256,h=2760,stride=5120;std::vector<uint8_t> a((size_t)h*stride),b((size_t)h*stride);
 for(uint32_t y=0;y<32;y++)for(uint32_t x=0;x<32;x++){auto off=(uint64_t)((uint64_t)y*(h-1)/31)*stride+(uint64_t)((uint64_t)x*(w-1)/31)*4;a[off]=(x+y)%2?0x77:0;a[off+3]=0xff;}
 b=a;auto x=x9_fb_sample_grid(a.data(),w,h,stride),y=x9_fb_sample_grid(b.data(),w,h,stride);assert(x.samples==1024&&x.nonblack==512&&x.opaque==1024&&x.checksum==y.checksum);
 b[0]=0x66;y=x9_fb_sample_grid(b.data(),w,h,stride);assert(x.checksum!=y.checksum);
 return 0;}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'test.cpp').write_text(code);subprocess.run(['/opt/homebrew/opt/llvm/bin/clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
