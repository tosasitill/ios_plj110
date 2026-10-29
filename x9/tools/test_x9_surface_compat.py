"""Exercise actual transplanted surface sizing/serialization helpers with mock IOKit containers."""
import re,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'work/x9-dali-compat/kernel-source/iokit/Kernel/arm/SweetSurface.cpp'
def function(source,needle):
 a=source.index(needle);b=source.index('{',a);depth=1;i=b+1
 while depth:
  if source[i]=='{':depth+=1
  if source[i]=='}':depth-=1
  i+=1
 return source[a:i]
class SurfaceCompatTests(unittest.TestCase):
 @unittest.skipUnless(SOURCE.exists(),'requires isolated surface source')
 def test_actual_geometry_and_plane_wire_layout(self):
  s=SOURCE.read_text();names=[('uint32_t','sweet_dict_u32'),('uint32_t','sweet_dict_u32_nz'),('uint32_t','sweet_div_up'),('uint32_t','sweet_row_bytes'),('IOReturn','sweet_surface_layout'),('void','put32'),('void','put64'),('void','sweet_surface_fill_result')]
  helpers='\n'.join(function(s,'static '+kind+'\n'+name+'(') for kind,name in names)
  constants='\n'.join(x for x in s.splitlines() if x.startswith('#define SWEET_SURFACE_'))
  prefix=r'''
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <strings.h>
#include <map>
#include <string>
#include <vector>
#include <cassert>
using IOReturn=int;
#define kIOReturnSuccess 0
#define kIOReturnBadArgument -1
#define kIOReturnNoMemory -2
#define SWEET_SURFACE_MAX_PLANES 20
#define round_page(x) (((uint64_t)(x)+16383)&~16383ULL)
struct OSObject {virtual ~OSObject()=default;};
struct OSNumber:OSObject {uint32_t v;explicit OSNumber(uint32_t n):v(n){} uint32_t unsigned32BitValue(){return v;}};
struct OSDictionary:OSObject {std::map<std::string,OSObject*> v;OSObject *getObject(const char *key){auto it=v.find(key);return it==v.end()?nullptr:it->second;}};
struct OSArray:OSObject {std::vector<OSObject*> v;unsigned getCount(){return (unsigned)v.size();}OSObject *getObject(unsigned n){return v[n];}};
#define OSDynamicCast(type,obj) dynamic_cast<type*>(obj)
struct sweet_surface_plane {uint32_t width,height,bytesPerRow,offset,size;uint16_t bytesPerElement;uint8_t elementWidth,elementHeight;};
struct SweetSurface {uint32_t id,width,height,bytesPerRow,offset,pixelFormat,allocSize;uint16_t bytesPerElement;uint8_t elementWidth,elementHeight;uint32_t cacheMode,planeCount,parentID; sweet_surface_plane planes[20];uint32_t wrapDelta;};
struct Map {uint64_t getAddress(){return 0x100000;}};
struct SweetSurfaceRef {Map *data,*state;};
'''
  main=r'''
static uint32_t read32(uint8_t *p){uint32_t v;memcpy(&v,p,4);return v;}
int main(void) {
 OSNumber width(1256),height(2760),zero(0),row(5120),small(1),huge(17000);
 OSDictionary props;props.v["IOSurfaceWidth"]=&width;props.v["IOSurfaceHeight"]=&height;
 SweetSurface a={};assert(sweet_surface_layout(&a,&props)==0);assert(a.bytesPerRow==5056);
 props.v["IOSurfaceBytesPerRow"]=&zero;SweetSurface b={};assert(sweet_surface_layout(&b,&props)==0);assert(b.bytesPerRow==a.bytesPerRow);
 props.v["IOSurfaceBytesPerRow"]=&row;assert(sweet_surface_layout(&b,&props)==0&&b.bytesPerRow==5120);
 props.v["IOSurfaceBytesPerRow"]=&small;assert(sweet_surface_layout(&b,&props)==-1);
 props.v["IOSurfaceBytesPerRow"]=&zero;
 OSDictionary plane;plane.v["IOSurfacePlaneBytesPerRow"]=&zero;plane.v["IOSurfacePlaneSize"]=&zero;
 OSArray planes;planes.v.push_back(&plane);props.v["IOSurfacePlaneInfo"]=&planes;
 assert(sweet_surface_layout(&b,&props)==0&&b.planeCount==1&&b.planes[0].bytesPerRow==1280);
 assert(b.planes[0].size==1280*2760);
 uint8_t result[SWEET_SURFACE_RESULT_SIZE];Map m;SweetSurfaceRef ref{&m,&m};sweet_surface_fill_result(&b,&ref,result);
 assert(read32(result+0x68+8)==1280&&read32(result+0x68+0x10)==1280);
 assert(read32(result+0x68+0x14)==b.planes[0].size);
 plane.v["IOSurfacePlaneBytesPerRow"]=&small;assert(sweet_surface_layout(&b,&props)==-1);
 plane.v["IOSurfacePlaneBytesPerRow"]=&zero;plane.v["IOSurfacePlaneSize"]=&small;assert(sweet_surface_layout(&b,&props)==-1);
 plane.v["IOSurfacePlaneSize"]=&zero;

 props.v["IOSurfaceWidth"]=&huge;assert(sweet_surface_layout(&b,&props)==-1);
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'surface.cpp').write_text(prefix+constants+'\n'+helpers+main)
   subprocess.run(['/opt/homebrew/opt/llvm/bin/clang++','-std=c++17','-O1','-fsanitize=address,undefined',str(p/'surface.cpp'),'-o',str(p/'surface')],check=True)
   subprocess.run([str(p/'surface')],check=True)
if __name__=='__main__':unittest.main()
