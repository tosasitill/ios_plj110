"""Execute plog hooks and framebuffer text/scroll against real C sources."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

SHIM = Path(__file__).resolve().parents[1] / 'shim'


def source(name):
    s = (SHIM / name).read_text()
    s = re.sub(r'^#include "[^\n]+"\n', '', s, flags=re.M)
    return s.replace('__asm__ volatile("dsb sy" ::: "memory");', '(void)0;')


def run(text):
    with tempfile.TemporaryDirectory(prefix='x9-screen-test-') as d:
        p, exe = Path(d) / 'fixture.c', Path(d) / 'fixture'
        p.write_text(text)
        subprocess.run([shutil.which('clang'), '-std=c11', '-D_DARWIN_C_SOURCE',
                        '-Wall', '-Wextra', '-Werror', str(p), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True)
        if result.returncode:
            raise AssertionError(result.stderr.decode(errors='replace'))


@unittest.skipUnless(shutil.which('clang'), 'requires clang')
class ScreenConsoleTests(unittest.TestCase):
    def test_plog_replay_crc_filter_hex_and_detach(self):
        run(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define X9_PSTORE_MIRROR 0
#define X9_CONSOLE_BASE 0
''' + source('plog.c') + r'''
static char screen[50000];
static unsigned screen_len, flushes;
static void screen_char(char c) {assert(screen_len+1<sizeof(screen));screen[screen_len++]=c;screen[screen_len]=0;}
static void screen_commit(void) {flushes++;}
int main(void) {
 plog_init();
 plog_str("early boot\n");plog_str("lk50:0:CRC-data-only\n");
 plog_attach_console(screen_char,screen_commit);
 assert(!strcmp(screen,"early boot\n"));assert(flushes==1);
 plog_str("loader ");plog_hex("jump",0x140000000UL);
 assert(strstr(screen,"loader jump=0x0000000140000000\n"));
 unsigned n=screen_len;
 plog_str("lk50:1:more diagnostic data\n");assert(screen_len==n);
 assert(strstr(logbuf,"lk50:1:more diagnostic data"));
 /* Local buffer exhaustion cannot stop the live screen/persistent sink. */
 for(unsigned i=0;i<20000;i++) put('x');
 assert(pos==LOCAL_SIZE-1 && screen_len==n+20000);
 plog_detach_console();n=screen_len;
 plog_str("no screen after handoff\n");assert(screen_len==n);
 plog_init();assert(!screen_put && !screen_flush);
 return 0;
}
''')

    def test_font_wrap_scroll_bounds_and_busy_retry(self):
        run(r'''
#include <assert.h>
#include <stdint.h>
#include <sys/mman.h>
#define FB_BASE 0x4fc910000UL
#define FB_W 1256
#define FB_H 2760
#define FB_STRIDE 5120
static unsigned present_calls, accept;
static void wdt_kick(void) {}
static void wait_ms(unsigned ms) {assert(ms==1);}
static int display_lk_present(void) {present_calls++;return accept;}
''' + (SHIM / 'font.h').read_text() + '\n' + source('fbconsole.c') + r'''
int main(void) {
 unsigned bytes=FB_STRIDE*FB_H;
 void *mem=mmap((void*)FB_BASE,bytes+0x10000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0);
 assert(mem==(void*)FB_BASE);
 uint32_t *guard=(uint32_t*)(FB_BASE+bytes);*guard=0x12345678;
 fbconsole_init();assert(dirty);
 fbconsole_putc('A');
 unsigned white=0;
 for(unsigned y=24;y<56;y++)for(unsigned x=24;x<40;x++)
  white+=*(uint32_t*)(FB_BASE+y*FB_STRIDE+x*4)==0xffffffff;
 assert(white>0 && white<512);
 fbconsole_flush();assert(dirty && present_calls==1);
 accept=1;fbconsole_flush();assert(!dirty && present_calls==2);
 fbconsole_flush();assert(present_calls==2);
 for(unsigned i=0;i<(ROWS+4)*COLS;i++) fbconsole_putc('A');
 assert(row<ROWS && col<=COLS && *guard==0x12345678);
 accept=0;fbconsole_drain();assert(dirty);
 accept=1;fbconsole_drain();assert(!dirty);
 fbconsole_putc('\r');assert(col==0);
 fbconsole_putc('\n');assert(row<ROWS);
 munmap(mem,bytes+0x10000);return 0;
}
''')


if __name__ == '__main__': unittest.main()
