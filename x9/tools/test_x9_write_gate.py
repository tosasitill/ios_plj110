"""Compile the actual native write-contract gate with DT/bind mocks."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'work/x9-writable-root/kernel-source/bsd/dev/sweet_x9_ufs.c'
class WriteGateTests(unittest.TestCase):
 @unittest.skipUnless(SOURCE.exists(),'requires frozen native source')
 def test_actual_gate_refuses_missing_malformed_overlapping_or_failed_contract(self):
  text=SOURCE.read_text();start=text.index('static int enable_writes(void)');end=text.index('static int initialize(void)',start);helper=text[start:end]
  code=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef void *DTEntry;
#define kSuccess 0
struct x9u_extent {uint64_t file,part,count;};
static struct {struct {uint64_t count;} partition;struct x9u_extent extents[2];unsigned extent_count;uint64_t bytes;int host;bool writable;} xu;
static uint32_t requested;
static int contract_present,lookup_present,protected_present,bind_error,binds;
static uint64_t contract[2],reservations[4];static unsigned reservation_bytes;
static int PE_parse_boot_argn(const char *name,void *out,size_t n)
{assert(!strcmp(name,"ufsrw")&&n==sizeof(requested));memcpy(out,&requested,n);return 1;}
static int SecureDTLookupEntry(void *unused,const char *path,DTEntry *out)
{(void)unused;assert(!strcmp(path,"/chosen"));*out=(void*)1;return lookup_present?0:-1;}
static int prop(DTEntry node,const char *key,void *out,unsigned n)
{(void)node;assert(!strcmp(key,"x9-ufs-root-write-contract")&&n==16);if(!contract_present)return -1;memcpy(out,contract,n);return 0;}
static int SecureDTGetProperty(DTEntry node,const char *key,const void **out,unsigned *n)
{(void)node;assert(!strcmp(key,"x9-ufs-protected-extents"));*out=reservations;*n=reservation_bytes;return protected_present?0:-1;}
static int x9u_bind_extent_writes(void *h,unsigned lun,const void *p,const struct x9u_extent *e,size_t n,uint64_t blocks)
{assert(h==&xu.host&&lun==2&&p==&xu.partition&&e==xu.extents&&n==xu.extent_count&&blocks==xu.bytes/4096);binds++;return bind_error;}
'''+helper+r'''
static void reset(void)
{
 memset(&xu,0,sizeof(xu));xu.partition.count=10000;xu.bytes=8192;xu.extent_count=2;
 xu.extents[0]=(struct x9u_extent){0,100,1};xu.extents[1]=(struct x9u_extent){1,300,1};
 requested=1;contract_present=lookup_present=protected_present=1;bind_error=binds=0;
 contract[0]=1;contract[1]=8192;reservations[0]=101;reservations[1]=199;reservation_bytes=16;
}
int main(void)
{
 reset();assert(enable_writes()==0&&xu.writable&&binds==1);
 for(int i=0;i<11;i++) {
  reset();
  switch(i) {
   case 0:lookup_present=0;break;
   case 1:contract_present=0;break;
   case 2:protected_present=0;break;
   case 3:contract[0]=2;break;
   case 4:contract[1]=4096;break;
   case 5:reservation_bytes=0;break;
   case 6:reservation_bytes=15;break;
   case 7:reservations[0]=100;break;
   case 8:reservations[0]=299;reservations[1]=2;break;
   case 9:reservations[0]=10000;break;
   case 10:reservations[1]=0;break;
  }
  assert(enable_writes()==EPERM&&!xu.writable&&!binds);
 }
 reset();bind_error=-12;assert(enable_writes()==EPERM&&!xu.writable&&binds==1);
 reset();requested=0;lookup_present=0;assert(enable_writes()==0&&!xu.writable&&!binds);
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'gate.c').write_text(code)
   subprocess.run(['/opt/homebrew/opt/llvm/bin/clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'gate.c'),'-o',str(p/'gate')],check=True)
   subprocess.run([str(p/'gate')],check=True)
if __name__=='__main__':unittest.main()
