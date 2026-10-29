"""Execute actual diagnostic functions with reset disabled; verify logs survive."""
from pathlib import Path
import json
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'x9/out/pan55-run-until-blocked'


def function(text, signature):
    a = text.index(signature)
    return text[a:text.index('\n}', a) + 2]


@unittest.skipUnless(shutil.which('clang'), 'requires clang')
class LogOnlyTests(unittest.TestCase):
    def test_reset_off_keeps_late_text_and_journal_even_at_selected_stage(self):
        source = (OUT / 'kernel-source/osfmk/arm64/sweet_early.c').read_text()
        fixture = r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#define SWEET_X9_DIAG_RESET 0
#define SWEET_X9_CHECKPOINT_STAGE 99
static unsigned early_disabled=1, board_x9=1, journals, marks;
static volatile uint32_t header[3];
static volatile uint32_t *pstore=header;
static char logtext[1000];
static void sweet_diag_journal(unsigned s) {assert(s<=99);journals++;}
static void sweet_early_mark(unsigned s,const char *label) {(void)s;(void)label;marks++;}
static void sweet_pstore_append(const char *s,int n) {unsigned at=(unsigned)strlen(logtext);assert(at+(unsigned)n<sizeof(logtext));memcpy(logtext+at,s,(unsigned)n);}
''' + function(source, '__attribute__((noinline)) void\nsweet_diag_checkpoint') + '\nvoid\n' + function(source, 'sweet_diag_probe(unsigned int stage, const char *label)') + '\nvoid\n' + function(source, 'sweet_diag_entry_reset(unsigned int stage)') + r'''
int main(void) {
 sweet_diag_checkpoint(80,"bsd_init");
 sweet_diag_checkpoint(99,"bsd_root_select_enter");
 sweet_diag_probe(99,"root probe");sweet_diag_entry_reset(99);
 assert(journals==2 && marks==2);
 assert(!strcmp(logtext,"CKPT bsd_init\nCKPT bsd_root_select_enter\n"));
 board_x9=0;unsigned n=(unsigned)strlen(logtext);
 sweet_diag_checkpoint(99,"other_board");assert(strlen(logtext)==n);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='x9-log-only-') as d:
            p, exe = Path(d) / 'fixture.c', Path(d) / 'fixture'
            p.write_text(fixture)
            # SMC is not valid on the host: successful compilation plus execution
            # exercises that reset=0 strips all three diagnostic reset paths.
            subprocess.run([shutil.which('clang'), '-std=c11', '-Wall', '-Wextra', '-Werror', str(p), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))

    def test_all_changed_units_explicitly_disable_diagnostic_reset(self):
        commands = json.loads((OUT / 'kernel-build-commands.json').read_text())
        for unit in ['sweet_early','bsd_init','IOKitBSDInit']:
            self.assertIn('-DSWEET_X9_DIAG_RESET=0', commands[unit]['args'])
            self.assertIn('-DSWEET_X9_CHECKPOINT_STAGE=0', commands[unit]['args'])
            self.assertIn('-DSWEET_X9_DIAG_PMSG=1', commands[unit]['args'])


if __name__ == '__main__': unittest.main()
