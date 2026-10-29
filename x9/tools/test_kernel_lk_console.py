"""Native XNU inherited-LK submit implementation, exercised on a host."""
from pathlib import Path
import subprocess
import tempfile
import shutil
import unittest

HEADER = Path(__file__).resolve().parents[1] / 're/x9_lk_console.h'


@unittest.skipUnless(shutil.which('clang'), 'requires clang')
class KernelLKConsoleTests(unittest.TestCase):
    def test_mapping_guards_write_allowlist_busy_and_second_buffer(self):
        fixture = r'''
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static uint32_t dsi[1024],mutexes[4][1024],engine[13][1024];
static uint32_t *canonical,*secondary;
static unsigned writes,fail_map;
static uint64_t mapped_fb;
static volatile uint32_t *mapio(uint64_t pa) {
 if(fail_map) return 0;
 const uint64_t banks[]={0x3e320000,0x3e720000,0x32920000,0x32c20000};
 const uint64_t engines[]={0x32930000,0x32950000,0x32960000,0x32970000,0x32980000,0x32990000,0x329a0000,0x32c50000,0x32c60000,0x32c70000,0x32c80000,0x32c90000,0x32ca0000};
 if(pa==0x3e730000) return dsi;
 for(unsigned i=0;i<4;i++)if(pa==banks[i])return mutexes[i];
 for(unsigned i=0;i<13;i++)if(pa==engines[i])return engine[i];
 assert(0);return 0;
}
static volatile uint32_t *mapfb(uint64_t pa) {mapped_fb=pa;return secondary;}
static void writeio(volatile uint32_t *p,unsigned off,uint32_t val) {
 if(writes<4) {assert(p==mutexes[writes]);assert(off==0x20+3*0x20 && val==1);}
 else {assert(p==dsi && off==0 && val==writes-4);}
 writes++;
}
#define X9_LK_MAP_IO(pa) mapio(pa)
#define X9_LK_MAP_FB(pa) mapfb(pa)
#define X9_LK_READ(p,o) ((p)[(o)/4])
#define X9_LK_WRITE(p,o,v) writeio(p,o,v)
#define X9_LK_FENCE() ((void)0)
#define X9_LK_PET() ((void)0)
''' + HEADER.read_text() + r'''
int main(void) {
 canonical=calloc(1,X9_LK_BYTES);secondary=calloc(1,X9_LK_BYTES);
 assert(canonical && secondary);
 for(unsigned bank=0;bank<4;bank++)mutexes[bank][(3*0x20+0x34)/4]=0x100;
 mutexes[1][(3*0x20+0x38)/4]=1;mutexes[2][(3*0x20+0x34)/4]=0x17e18;
 for(unsigned i=2;i<=3;i++) {
  uint32_t *p=engine[i];p[0x20/4]=0x11;p[0x40/4]=1;
  p[0x48/4]=(2760u<<16)|1256;p[0x50/4]=2;p[0x2f4/4]=5120;
  p[0x300/4]=0x1ff;p[0xf40/4]=X9_LK_FB+(i-2)*X9_LK_SLOT;
 }
 assert(x9_lk_prepare(canonical));assert(x9_lk_id==3 && x9_lk_count==2);
 assert(mapped_fb==X9_LK_FB+X9_LK_SLOT);
 canonical[0]=0xff123456;canonical[X9_LK_BYTES/4-1]=0xff654321;
 x9_lk_present();assert(writes==6);
 assert(secondary[0]==canonical[0] && secondary[X9_LK_BYTES/4-1]==canonical[X9_LK_BYTES/4-1]);
 writes=0;dsi[0x0c/4]=0x80000000;
 x9_lk_present();assert(!writes);dsi[0x0c/4]=0;
 uint64_t pa;unsigned swap;
 uint32_t saved=engine[2][0x40/4];engine[2][0x40/4]=0x11;
 assert(!x9_lk_layer(engine[2],&pa,&swap));engine[2][0x40/4]=saved;
 engine[2][0xf40/4]=X9_LK_FB+X9_LK_RESERVE-4;
 assert(!x9_lk_prepare(canonical));assert(!x9_lk_active);
 engine[2][0xf40/4]=X9_LK_FB;
 mutexes[4-1][(4*0x20+0x34)/4]=0;
 mutexes[1][(4*0x20+0x38)/4]=1;mutexes[2][(4*0x20+0x34)/4]=0x18;
 assert(!x9_lk_prepare(canonical));x9_lk_present();assert(!writes);
 mutexes[1][(4*0x20+0x38)/4]=0;mutexes[2][(4*0x20+0x34)/4]=0;
 fail_map=1;assert(!x9_lk_prepare(canonical));assert(!x9_lk_active);
 free(canonical);free(secondary);return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='x9-native-console-') as d:
            p, exe = Path(d) / 'fixture.c', Path(d) / 'fixture'
            p.write_text(fixture)
            subprocess.run([shutil.which('clang'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(p), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))


if __name__ == '__main__': unittest.main()
