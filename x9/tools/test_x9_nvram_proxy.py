"""Test generated proxy with extracted actual XNU CHRP validation/parser code."""
import ast,importlib.util,struct,subprocess,tempfile,unittest,zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
XNU=Path(__file__).resolve().parents[2]/'.local/dxb/xnu'
spec=importlib.util.spec_from_file_location('proxy',ROOT/'x9/tools/nvram_proxy.py');proxy=importlib.util.module_from_spec(spec);spec.loader.exec_module(proxy)
def function(source,needle):
 start=source.index(needle);brace=source.index('{',start);level=1;end=brace+1
 while level:
  if source[end]=='{':level+=1
  if source[end]=='}':level-=1
  end+=1
 return source[start:end]
class NVRAMProxyTests(unittest.TestCase):
 def test_exact_reference_layout_and_adt(self):
  ref=ROOT/'work/references/data-a-live/tools/mkadt_pandora.py'
  if ref.exists():
   module=ast.parse(ref.read_text());fn=next(n for n in module.body if isinstance(n,ast.FunctionDef) and n.name=='empty_nvram_proxy')
   env={'struct':struct,'zlib':zlib,'u32':lambda *v:struct.pack('<'+'I'*len(v),*v)}
   exec(compile(ast.Module(body=[fn],type_ignores=[]),str(ref),'exec'),env)
   self.assertEqual(proxy.chosen_properties(),env['empty_nvram_proxy']())
  image=proxy.image();self.assertEqual(len(image),8192);self.assertEqual(struct.unpack_from('<I',image,16)[0],zlib.adler32(image[20:]))
 @unittest.skipUnless((XNU/'iokit/Kernel/IONVRAMCHRPHandler.cpp').exists(),'needs actual XNU parser')
 def test_original_xnu_chrp_parser_accepts_proxy_and_rejects_corruption(self):
  s=(XNU/'iokit/Kernel/IONVRAMCHRPHandler.cpp').read_text()
  constants=s[s.index('#define NVRAM_CHRP_APPLE_HEADER_NAME_V1'):s.index('typedef struct {\n\tNVRAMPartitionType')]
  pieces=[]
  for needle in ['static const char *\nget_bank_version_string(', 'static uint32_t\nadler32(', 'static uint32_t\nnvram_get_adler(', 'static uint32_t\nadler32_with_version(', 'static uint8_t\nchrp_checksum(', 'static IOReturn\nnvram_validate_header_v1v2(', 'static NVRAMVersion\nvalidateNVRAMVersion(', 'IOReturn\nIONVRAMCHRPHandler::unserializeImage(']:
   body=function(s,needle).replace('IONVRAMCHRPHandler::unserializeImage','parse_image');pieces.append(body)
  prefix=r'''
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <stdexcept>
using IOReturn=int;using IOByteCount=size_t;
enum NVRAMVersion {kNVRAMVersionUnknown=0,kNVRAMVersion1=1,kNVRAMVersion2=2};
#define kIOReturnSuccess 0
#define kIOReturnError -1
#define kIOReturnInvalid -2
#define require(x,label) do{if(!(x))goto label;}while(0)
#define require_action(x,label,action) do{if(!(x)){action;goto label;}}while(0)
#define DEBUG_INFO(...) ((void)0)
#define DEBUG_ALWAYS(...) ((void)0)
#define panic(...) throw std::runtime_error("panic")
#define IOFreeData(p,n) free(p)
#define IONewData(t,n) ((t*)malloc(n))
#define bcopy(a,b,n) memcpy(b,a,n)
class OSDictionary {public:static OSDictionary *withCapacity(int){static OSDictionary d;return &d;}};
static OSDictionary *_varDict;
static uint8_t *_nvramImage;
static uint32_t _bankSize,_generation,_commonPartitionOffset,_commonPartitionSize,_systemPartitionOffset,_systemPartitionSize;
static NVRAMVersion _version;
'''
  main=r'''
int main(int argc,char **argv) {
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);uint8_t image[8192];assert(fread(image,1,8192,f)==8192);fclose(f);
 _bankSize=sizeof(image);assert(parse_image(image,sizeof(image))==0);
 assert(_commonPartitionOffset==48&&_commonPartitionSize==2048);
 assert(_systemPartitionSize==0&&_systemPartitionOffset==0xffffffff);
 assert(_version==kNVRAMVersion1&&_nvramImage&&_varDict);
 assert(_nvramImage[_commonPartitionOffset]==0);
 image[1]^=1;assert(parse_image(image,sizeof(image))==-2);image[1]^=1;
 image[100]^=1;bool panicked=false;try{parse_image(image,sizeof(image));}catch(const std::runtime_error&){panicked=true;}assert(panicked);
 free(_nvramImage);return 0;
}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'proxy.bin').write_bytes(proxy.image());(p/'parse.cpp').write_text(prefix+constants+'\n'+'\n'.join(pieces)+main)
   subprocess.run(['/opt/homebrew/opt/llvm/bin/clang++','-std=c++17','-O1','-fsanitize=address,undefined',str(p/'parse.cpp'),'-o',str(p/'parse')],check=True)
   subprocess.run([str(p/'parse'),str(p/'proxy.bin')],check=True)
if __name__=='__main__':unittest.main()
