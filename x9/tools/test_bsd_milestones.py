"""Execute named BSD milestone dispatch from the patched source on the host."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/out/pan53-bsd90-lk-console/kernel-source'


def body(text, name):
    a = text.index(name)
    return text[a:text.index('\n}', a) + 2]


@unittest.skipUnless(shutil.which('clang'), 'requires clang')
class BSDMilestoneTests(unittest.TestCase):
    def test_real_name_dispatch_moves_reset_from_80_to_90_and_ignores_other_boards(self):
        early = (SOURCE / 'osfmk/arm64/sweet_early.c').read_text()
        bsd = (SOURCE / 'bsd/kern/bsd_init.c').read_text()
        a = early.index('struct sweet_bootstrap_stage {')
        b = early.index('\n};', early.index('static const struct sweet_bootstrap_stage')) + 3
        fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define SWEET_X9_CHECKPOINT_STAGE 90
static unsigned board=1, resets, checkpoints, journals, last;
static int sweet_early_is_x9(void) {return (int)board;}
static void sweet_diag_journal(unsigned s) {assert(s==90);journals++;}
static void sweet_diag_entry_reset(unsigned s) {if(s==90)resets++;}
static void sweet_diag_checkpoint(unsigned s,const char *label) {(void)label;checkpoints++;last=s;}
''' + early[a:b] + '\nvoid\n' + body(early, 'sweet_bootstrap_mark(const char *message)') + '\nstatic inline void\n' + body(bsd, 'sweet_bsd_mark(unsigned stage, const char *label)') + r'''
int main(void) {
 sweet_bootstrap_mark("bsd_init");assert(last==80 && !resets);
 const char *names[]={"bsd_entered","bsd_proc0_ready","bsd_buffers_ready","bsd_vfs_ready","bsd_filesystems_registered","bsd_mbuf_ready","bsd_posix_ready","bsd_network_ready","bsd_sysctl_ready","bsd_autoconf_enter"};
 for(unsigned i=0;i<10;i++) {sweet_bsd_mark(81+i,names[i]);assert(last==81+i);assert(resets==(i==9));}
 assert(resets==1 && journals==1 && checkpoints==11);
 sweet_bootstrap_mark("not_a_stage");assert(checkpoints==11);
 board=0;sweet_bsd_mark(90,names[9]);assert(resets==1 && journals==1 && checkpoints==11);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='x9-bsd-milestones-') as d:
            p, exe = Path(d) / 'fixture.c', Path(d) / 'fixture'
            p.write_text(fixture)
            subprocess.run([shutil.which('clang'), '-std=c11', '-Wall', '-Wextra', '-Werror', str(p), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))

    def test_marks_are_ordered_inside_bsd_and_target_precedes_autoconf_and_mount(self):
        text = (SOURCE / 'bsd/kern/bsd_init.c').read_text()
        function = body(text, 'bsd_init(void)')
        marks = [(int(m[0]), m[1]) for m in re.findall(r'sweet_bsd_mark\((\d+), "([^"]+)"\)', function)]
        self.assertEqual([x[0] for x in marks], list(range(81,91)))
        self.assertLess(function.index('sweet_bsd_mark(90,'), function.index('\tbsd_autoconf();'))
        self.assertLess(function.index('\tbsd_autoconf();'), function.index('\t\tsetconf();'))
        for call, stage in [('bsd_bufferinit();',83), ('vfsinit();',84), ('psem_cache_init();',87), ('sysctl_mib_init();',89)]:
            self.assertLess(function.index(call), function.index(f'sweet_bsd_mark({stage},'))


if __name__ == '__main__': unittest.main()
