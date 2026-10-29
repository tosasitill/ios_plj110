"""Execute named BSD milestone dispatch from the patched source on the host."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/out/pan54-root99-lk-console/kernel-source'


def body(text, name):
    a = text.index(name)
    return text[a:text.index('\n}', a) + 2]


@unittest.skipUnless(shutil.which('clang'), 'requires clang')
class BSDRootBoundaryTests(unittest.TestCase):
    def test_real_name_dispatch_moves_reset_from_80_to_99_and_ignores_other_boards(self):
        early = (SOURCE / 'osfmk/arm64/sweet_early.c').read_text()
        bsd = (SOURCE / 'bsd/kern/bsd_init.c').read_text()
        a = early.index('struct sweet_bootstrap_stage {')
        b = early.index('\n};', early.index('static const struct sweet_bootstrap_stage')) + 3
        fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define SWEET_X9_CHECKPOINT_STAGE 99
static unsigned board=1, resets, checkpoints, journals, last;
static int sweet_early_is_x9(void) {return (int)board;}
static void sweet_diag_journal(unsigned s) {assert(s==99);journals++;}
static void sweet_diag_entry_reset(unsigned s) {if(s==99)resets++;}
static void sweet_diag_checkpoint(unsigned s,const char *label) {(void)label;checkpoints++;last=s;}
''' + early[a:b] + '\nvoid\n' + body(early, 'sweet_bootstrap_mark(const char *message)') + '\nstatic inline void\n' + body(bsd, 'sweet_bsd_mark(unsigned stage, const char *label)') + r'''
int main(void) {
 sweet_bootstrap_mark("bsd_init");assert(last==80 && !resets);
 const char *names[]={"bsd_entered","bsd_proc0_ready","bsd_buffers_ready","bsd_vfs_ready","bsd_filesystems_registered","bsd_mbuf_ready","bsd_posix_ready","bsd_network_ready","bsd_sysctl_ready","bsd_autoconf_enter","bsd_autoconf_ready","bsd_net_clients_ready","bsd_calendar_ready","bsd_root_select_enter"};
 for(unsigned i=0;i<14;i++) {unsigned stage=i<13?81+i:99;sweet_bsd_mark(stage,names[i]);assert(last==stage);assert(resets==(i==13));}
 assert(resets==1 && journals==1 && checkpoints==15);
 sweet_bootstrap_mark("not_a_stage");assert(checkpoints==15);
 board=0;sweet_bsd_mark(99,names[13]);assert(resets==1 && journals==1 && checkpoints==15);
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
        self.assertEqual([x[0] for x in marks], list(range(81,94))+[99])
        self.assertLess(function.index('sweet_bsd_mark(90,'), function.index('\tbsd_autoconf();'))
        self.assertLess(function.index('\tbsd_autoconf();'), function.index('\t\tsetconf();'))
        self.assertLess(function.index('inittodr(0);'), function.index('sweet_bsd_mark(93,'))
        self.assertLess(function.index('sweet_bsd_mark(99,'), function.index('\t\tsetconf();'))
        self.assertLess(function.index('\tbsd_autoconf();'), function.index('sweet_bsd_mark(91,'))
        for call, stage in [('bsd_bufferinit();',83), ('vfsinit();',84), ('psem_cache_init();',87), ('sysctl_mib_init();',89)]:
            self.assertLess(function.index(call), function.index(f'sweet_bsd_mark({stage},'))


if __name__ == '__main__': unittest.main()
