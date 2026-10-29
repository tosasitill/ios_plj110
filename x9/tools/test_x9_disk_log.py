"""Actual disk-log flush routine with bounded in-memory transport and source ring.
Exercises initial capture, incremental writes, wrap and failed payload/header sync.
"""
import importlib.util,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('pull_disk_log',ROOT/'x9/tools/pull_disk_log.py');decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
class DiskLogTests(unittest.TestCase):
 def test_actual_flush_and_decoder(self):
  path=ROOT/'work/x9-disk-log/sweet_x9_log.inc'
  if not path.exists():self.skipTest('requires frozen logger source')
  s=path.read_text();s=s[:s.index('void sweet_x9_ufs_log_flush(void)')]
  prefix=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
struct x9u_host {unsigned poisoned;};
struct x9u_extent {uint64_t file,part,count;};
static struct {bool ready,failed;struct x9u_host host;} xu;
static unsigned char image[131072],source[400000];static uint64_t source_end;
static int writes, syncs, fail_write,fail_sync;
uint32_t sweet_log_copy(uint64_t *pos,char *out,uint32_t max)
{
 uint64_t n=source_end-*pos;if(n>max)n=max;
 memcpy(out,source+*pos,n);*pos+=n;return n;
}
static uint32_t x9u_crc32(uint32_t crc,const void *data,size_t size)
{
 const uint8_t *p=data;crc=~crc;
 while(size--){crc^=*p++;for(unsigned i=0;i<8;i++)crc=(crc>>1)^((0U-(crc&1))&0xedb88320U);}
 return ~crc;
}
static int x9u_image_write(struct x9u_host *h,uint64_t offset,const void *data,size_t n)
{
 (void)h;writes++;if(writes==fail_write)return -7;
 assert(offset<=sizeof(image)&&n<=sizeof(image)-offset);memcpy(image+offset,data,n);return 0;
}
static int x9u_sync_image(struct x9u_host *h)
{(void)h;syncs++;return syncs==fail_sync?-7:0;}
'''
  main=r'''
static void reset(void)
{
 memset(&xl,0,sizeof(xl));memset(image,0,sizeof(image));xl.bytes=sizeof(image);
 xl.ready=true;xu.ready=true;xu.failed=false;source_end=0;writes=syncs=fail_write=fail_sync=0;
}
int main(int argc,char **argv)
{
 assert(argc==2);for(unsigned i=0;i<sizeof(source);i++)source[i]=(uint8_t)(i*7+i/251);
 reset();source_end=50000;assert(!log_flush_locked());assert(xl.total==source_end&&xl.first==0&&xl.seq==1);
 assert(!memcmp(image+4096,source,50000));int before=writes;assert(!log_flush_locked()&&writes==before);
 source_end=400000;assert(!log_flush_locked());assert(xl.total==source_end&&xl.first==source_end-(sizeof(image)-4096));
 FILE *f=fopen(argv[1],"wb");assert(f&&fwrite(image,1,sizeof(image),f)==sizeof(image));fclose(f);
 for(int mode=0;mode<2;mode++) {
  reset();source_end=50000;if(mode==0)fail_write=1;else fail_sync=1;
  assert(log_flush_locked()==-7&&!xl.ready&&memcmp(image,"X9UFSLOG",8));
 }
 reset();xl.host.poisoned=1;fail_write=1;source_end=100;
 assert(log_flush_locked()==-7&&xu.failed&&xu.host.poisoned);
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'log.c').write_text(prefix+s+main)
   subprocess.run(['/opt/homebrew/opt/llvm/bin/clang','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'log.c'),'-o',str(p/'log')],check=True)
   subprocess.run([str(p/'log'),str(p/'log.bin')],check=True)
   raw=(p/'log.bin').read_bytes();payload,meta=decoder.decode(raw)
   source=bytes((i*7+i//251)%256 for i in range(400000))
   self.assertEqual(payload,source[-(131072-4096):]);self.assertTrue(meta['header_crc_valid'])
   bad=bytearray(raw);bad[24]^=1
   with self.assertRaises(ValueError):decoder.decode(bad)
if __name__=='__main__':unittest.main()
