"""x9_lk_dcs_long (brightness DCS 0x51 between frames), exercised on a host."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HEADER = Path(__file__).resolve().parents[1] / 'kernel/src/pexpert/arm/x9_lk_console.h'


@unittest.skipUnless(shutil.which('clang') or shutil.which('cc'), 'requires a C compiler')
class KernelLKDcsTests(unittest.TestCase):
    def test_long_write_restores_frame_cmdq_and_keeps_te(self):
        fixture = r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
static uint32_t dsi[1024];
static uint32_t log_off[256], log_val[256]; static unsigned nlog, delays, busy_polls, busy_after_start;
static volatile uint32_t *mapio(uint64_t pa) {(void)pa;return dsi;}
static volatile uint32_t *mapfb(uint64_t pa) {(void)pa;return 0;}
static uint32_t mrd(volatile uint32_t *p, unsigned off) {
 if(off==0x0c && busy_polls){busy_polls--;return p[off/4]|0x80000000u;}
 return p[off/4];
}
static void mwr(volatile uint32_t *p, unsigned off, uint32_t v) {
 assert(p==dsi);
 if(nlog<256){log_off[nlog]=off;log_val[nlog]=v;nlog++;}
 if(off==0x0c){p[off/4]&=v;return;}           /* W0C */
 if(off==0x00&&v==1)busy_polls=busy_after_start;
 p[off/4]=v;
}
#define X9_LK_MAP_IO(pa) mapio(pa)
#define X9_LK_MAP_FB(pa) mapfb(pa)
#define X9_LK_READ(p,o) mrd(p,o)
#define X9_LK_WRITE(p,o,v) mwr(p,o,v)
#define X9_LK_FENCE() ((void)0)
#define X9_LK_PET() ((void)0)
#define X9_LK_DELAY_US(us) (delays++)
''' + HEADER.read_text() + r'''
int main(void) {
 const uint8_t bl[3]={0x51,0x0d,0xbb};
 uint32_t r[4];
 x9_lk_dsi=dsi;
 assert(x9_lk_dcs_long(bl,3,0,r)==-1);          /* inactive */
 x9_lk_active=1;
 dsi[0x400/4]=0x002c3909;dsi[0x404/4]=0x11223344;dsi[0x44/4]=0x00008001;dsi[0xa0/4]=0;
 dsi[0x0c/4]=0x80000004u;nlog=0;                /* frame in flight */
 assert(x9_lk_dcs_long(bl,3,0,r)==0);assert(nlog==0);
 dsi[0x0c/4]=0x00000004u;                       /* idle, TE_RDY latched */
 x9_lk_submit_lock=1;assert(x9_lk_dcs_long(bl,3,0,r)==0);assert(nlog==0);
 x9_lk_submit_lock=0;
 assert(x9_lk_dcs_long(bl,3,0,r)==1);
 /* packet: payload word then LONG header, size 2|SIZE_SEL, START 0,1,0 */
 int saw_payload=0,saw_hdr=0,saw_size=0,starts=0;
 for(unsigned i=0;i<nlog;i++){
  if(log_off[i]==0x404&&log_val[i]==0x00bb0d51u)saw_payload=1;
  if(log_off[i]==0x400&&log_val[i]==0x00033902u){assert(saw_payload);saw_hdr=1;}
  if(log_off[i]==0x44&&(log_val[i]&0x83ffu)==0x8002u){assert(saw_hdr);saw_size=1;}
  if(log_off[i]==0x00){assert(saw_size);assert(log_val[i]==(starts==1));starts++;}
 }
 assert(saw_payload&&saw_hdr&&saw_size&&starts==3);
 assert(dsi[0x00/4]==0);
 /* frame packet and CMDQ_CON restored; TE_RDY not cleared */
 assert(dsi[0x400/4]==0x002c3909&&dsi[0x404/4]==0x11223344&&dsi[0x44/4]==0x00008001);
 assert(dsi[0x0c/4]&4u);
 assert(r[2]==0x002c3909&&r[3]==0x00008001);
 assert(!x9_lk_submit_lock);
 /* BUSY clears only after a few polls -> still success, delays counted */
 busy_after_start=3;delays=0;assert(x9_lk_dcs_long(bl,3,0,r)==1);assert(delays>=3);
 /* BUSY never clears -> -2, frame packet still restored, lock released */
 busy_after_start=100000;assert(x9_lk_dcs_long(bl,3,0,r)==-2);
 assert(dsi[0x400/4]==0x002c3909&&dsi[0x44/4]==0x00008001&&!x9_lk_submit_lock);
 busy_after_start=0;busy_polls=0;
 /* busy at entry -> refused without a single write */
 busy_polls=1;nlog=0;assert(x9_lk_dcs_long(bl,3,0,r)==0);assert(nlog==0);busy_polls=0;
 /* readback path restores RX_CON and CMDQ */
 dsi[0x5fc/4]=0x00bb0d22u;dsi[0x0c/4]|=1u;
 assert(x9_lk_dcs_long(bl,3,1,r)==1);
 assert(dsi[0xa0/4]==0&&dsi[0x400/4]==0x002c3909&&dsi[0x44/4]==0x00008001);
 assert((r[0]&0xff)==0x22);
 assert(dsi[0x0c/4]&4u);
 return 0;
}
'''
        cc = shutil.which('clang') or shutil.which('cc')
        with tempfile.TemporaryDirectory(prefix='x9-lk-dcs-') as d:
            p, exe = Path(d) / 'fixture.c', Path(d) / 'fixture'
            p.write_text(fixture)
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', str(p), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
