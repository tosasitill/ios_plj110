"""Execute the actual preserved-LK C path with guarded MMIO/RAM mocks.
Host tests prove write boundaries, not MT6993 scanout.
"""
from pathlib import Path
import binascii
import re
import shutil
import subprocess
import tempfile
import unittest

from decode_lk_preserve import analyze

SHIM = Path(__file__).resolve().parents[1] / 'shim'


@unittest.skipUnless(shutil.which('clang'), 'requires clang')
class LKPreserveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='x9-lk-preserve-')
        cls.exe = Path(cls.work.name) / 'fixture'
        source = (SHIM / 'display_lk.c').read_text()
        source = re.sub(r'^#include "[^\n]+"\n', '', source, flags=re.M)
        source = source.replace('__asm__ volatile("dsb sy" ::: "memory");', '(void)0;')
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define DSI_BASE 0x3e730000UL
#define FB_BASE 0x4fc910000UL
#define FB_W 1256
#define FB_H 2760
#define FB_STRIDE 5120
#define FB_SIZE 0x3660000UL
static unsigned writes, paints_seen, status_reads, bad_fdt, log_output;
static uint32_t busy_at, mods[8][4][2], sofs[8][4];
static unsigned reads;
struct word { uintptr_t a; uint32_t v; } regs[256];
static unsigned nregs;
static void setreg(uintptr_t a, uint32_t v) {
 for (unsigned i=0;i<nregs;i++) if(regs[i].a==a) {regs[i].v=v;return;}
 assert(nregs<256); regs[nregs++]=(struct word){a,v};
}
static uint32_t rd32(uintptr_t a) {
 reads++;
 if(a==DSI_BASE+0x0c) {status_reads++;return busy_at && status_reads>=busy_at?0x80000000u:0;}
 for(unsigned id=0;id<8;id++) for(unsigned bank=0;bank<4;bank++) {
  const uintptr_t b[]={0x3e320000UL,0x3e720000UL,0x32920000UL,0x32c20000UL};
  uintptr_t off=a-b[bank]-id*0x20;
  if(off==0x30) return sofs[id][bank];
  if(off==0x34) return mods[id][bank][0];
  if(off==0x38) return mods[id][bank][1];
 }
 for(unsigned i=0;i<nregs;i++) if(regs[i].a==a) return regs[i].v;
 return 0;
}
static void wr32(uintptr_t a,uint32_t v) {
 /* Entire active path may write only selected inherited EN and START. */
 const uintptr_t b[]={0x3e320000UL,0x3e720000UL,0x32920000UL,0x32c20000UL};
 if(writes%6<4) {assert(a==b[writes%6]+0x20+3*0x20 && v==1);}
 else {assert(a==DSI_BASE && v==writes%6-4);}
 assert(*(volatile uint32_t*)FB_BASE!=0);
 paints_seen++; writes++;
}
static void plog_str(const char *s) { if(log_output) fputs(s,stdout); }
static void wdt_kick(void) {}
static void wait_ms(unsigned ms) {assert(ms==500);}
static int fdt_chosen_u64(uintptr_t fdt,const char *p,uint64_t *out) {
 assert(fdt==0x1234);
 if(bad_fdt) return 0;
 *out=strstr(p,"base_l")?FB_BASE:strstr(p,"base_h")?0:FB_SIZE;return 1;
}
''' + source + r'''
static void seed(void) {
 mods[3][0][0]=0x100;
 mods[3][1][1]=1;
 mods[3][2][0]=0x17e18;
 mods[3][3][0]=0x400;
 for(unsigned u=3;u<=4;u++) {
  uintptr_t b=0x32930000UL+u*0x10000UL;
  uint64_t pa=FB_BASE+(u-3)*SLOT_BYTES;
  setreg(b+0x20,0x11); setreg(b+0x40,1);
  setreg(b+0x48,FB_H*0x10000u+FB_W);setreg(b+0x50,2);
  setreg(b+0x2f4,FB_STRIDE);setreg(b+0x300,0x1ff);
  setreg(b+0xf40,(uint32_t)pa);setreg(b+0xf4c,(uint32_t)(pa>>32));
 }
}
int main(int argc,char **argv) {
 assert(argc==2);
 unsigned scenario=(unsigned)atoi(argv[1]);
 void *mem=mmap((void*)FB_BASE,FB_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0);
 assert(mem==(void*)FB_BASE);
 seed();
 if(scenario==0) {
  display_lk_preserve(0x1234);
  assert(writes==16*6 && paints_seen==writes);
  assert(!memcmp((void*)FB_BASE,(void*)(FB_BASE+SLOT_BYTES),FRAME_BYTES));
 } else if(scenario==1) {
  busy_at=1;display_lk_preserve(0x1234);assert(!writes && !*(uint32_t*)FB_BASE);
 } else if(scenario==2) {
  mods[4][1][1]=1;mods[4][2][0]=0x18;
  display_lk_preserve(0x1234);assert(!writes && !*(uint32_t*)FB_BASE);
 } else if(scenario==3) {
  setreg(0x32960f40,(uint32_t)(FB_BASE+FB_SIZE-4));
  display_lk_preserve(0x1234);assert(!writes && !*(uint32_t*)FB_BASE);
 } else if(scenario==4) {
  bad_fdt=1;display_lk_preserve(0x1234);assert(!writes && !reads);
 } else if(scenario==5) {
  struct inherited state={0};assert(prepare_layers(&state));assert(prepare_mutex(&state));
  status_reads=0;busy_at=2;assert(!refresh(&state,0));assert(!writes);
 } else if(scenario==6) {
  struct layer l={.en=0x11,.layer=1,.size=FB_H*0x10000u+FB_W,
   .format=2,.pitch=FB_STRIDE,.con=0x1ff,.low=(uint32_t)FB_BASE,.high=FB_BASE>>32};
  uint64_t address;assert(valid_layer(&l,&address));
  struct layer x;
#define REJECT(field,value) x=l;x.field=(value);assert(!valid_layer(&x,&address))
  REJECT(layer,0x11);REJECT(layer,0x101);REJECT(layer,0x201);
  REJECT(en,0);REJECT(size,0);REJECT(pitch,16);REJECT(format,3);
  REJECT(pitch_high,1);REJECT(high,1);REJECT(high,0x10);
  REJECT(offset,1);REJECT(clip,1);REJECT(con,0x200);REJECT(con,0x400);
  REJECT(low,(uint32_t)(FB_BASE+4));REJECT(low,(uint32_t)(FB_BASE-4));REJECT(low,0xfffffffcu);
#undef REJECT
 } else if(scenario==7) {
  log_output=1;record(2,0x3e73000c,0x80000000);
 } else if(scenario==8) {
  sofs[3][1]=1;display_lk_preserve(0x1234);assert(!writes);
 } else if(scenario==9) {
  setreg(DSI_BASE+0x34,1);display_lk_preserve(0x1234);assert(!writes);
 } else if(scenario==10) {
  setreg(0x32970050,0x10002);display_lk_preserve(0x1234);assert(!writes);
 } else if(scenario==11) {
  assert(display_lk_console_init(0x1234));
  *(uint32_t*)FB_BASE=0xff102030;
  assert(display_lk_present());assert(writes==6);
  assert(*(uint32_t*)(FB_BASE+SLOT_BYTES)==0xff102030);
  display_lk_stop();assert(!display_lk_present());assert(writes==6);
 } else {return 2;}
 munmap(mem,FB_SIZE);return 0;
}
'''
        path = Path(cls.work.name) / 'fixture.c'
        path.write_text(fixture)
        subprocess.run([shutil.which('clang'), '-std=c11', '-D_DARWIN_C_SOURCE',
                        '-Wall', '-Wextra', '-Werror', str(path), '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_case(self, n):
        result = subprocess.run([str(self.exe), str(n)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        return result.stdout

    def test_preserved_nonzero_mutex_id_and_mirrored_buffers(self): self.run_case(0)
    def test_busy_transfer_never_paints_or_restarts(self): self.run_case(1)
    def test_ambiguous_mutex_never_writes(self): self.run_case(2)
    def test_out_of_bounds_canonical_never_writes(self): self.run_case(3)
    def test_bad_fdt_prevents_all_display_access(self): self.run_case(4)
    def test_busy_after_render_prevents_submit(self): self.run_case(5)
    def test_layer_guards_reject_invalid_sources_formats_geometry_addresses(self): self.run_case(6)
    def test_console_present_mirrors_and_stop_ends_hardware_writes(self): self.run_case(11)
    def test_video_sof_rejected(self): self.run_case(8)
    def test_video_dsi_mode_rejected(self): self.run_case(9)
    def test_conflicting_buffer_formats_rejected(self): self.run_case(10)

    def test_crc_records_match_independent_python_calculation(self):
        data = self.run_case(7)
        lines = data.splitlines()
        self.assertEqual(len(lines), 4)
        for line in lines:
            prefix, crc = line.rsplit(b':', 1)
            self.assertEqual(int(crc, 16), binascii.crc32(prefix))
            self.assertEqual(prefix, b'lk50:2:000000003e73000c:80000000')
        result = analyze([('fixture', data)])
        self.assertEqual(len(result['samples']), 4)
        self.assertFalse(result['rejected'])
        self.assertEqual(result['samples'][0]['phase'], 'after_submit')
        for i in range(len(lines[0])):
            damaged = bytearray(lines[0])
            damaged[i] ^= 1
            self.assertFalse(analyze([('damaged', damaged)])['samples'])

    def test_active_boot_path_cannot_link_legacy_display(self):
        main = (SHIM / 'main.c').read_text()
        self.assertIn('display_lk_console_init((uintptr_t)dtb)', main)
        for call in ('display_panel_wake(', 'display_video_start(', 'dsi_trigger_lk(',
                     'display_backlight_on(', 'display_mutex_kick('):
            self.assertNotIn(call, main)
        build = (SHIM / 'build.py').read_text()
        sources = next(l for l in build.splitlines() if l.startswith('SOURCES ='))
        self.assertIn("'display_lk.c'", sources)
        self.assertNotIn("'display.c'", sources)


if __name__ == '__main__': unittest.main()
