#!/usr/bin/env python3
"""Host-check the actual diagnostic CMGR frame construction, not modem I/O."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'

class DiagnosticReadOnly(unittest.TestCase):
    def test_storage_job_and_diagnostic_do_not_block_each_other(self):
        source = SOURCE.read_text()
        storage_start = source.index('static bool x9m_store_blocked(')
        storage_end = source.index('static uint64_t x9m_store_submit(', storage_start)
        diagnostic_start = source.index('static bool x9m_diag_storage_busy(')
        diagnostic_end = source.index('static void x9m_diag_poll(', diagnostic_start)
        poll = source[source.index('  if(j.op==X9M_DIAG&&!j.token){'):]
        self.assertLess(poll.index('if(x9m_diag_storage_busy(j.sim,&j))continue;'),
                        poll.index('x9w_frame f(j.step?0x607:0x60b'))
        code = r'''
#include <cassert>
#include <cstdint>
enum { X9M_LIST=5, X9M_READ, X9M_TAG, X9M_DIAG };
struct x9m_job { unsigned op, sim; uint64_t token, store_order; bool hold; };
static x9m_job x9m_jobs[3];
static bool x9m_storage(unsigned op){return op==X9M_LIST||op==X9M_READ||op==X9M_TAG;}
''' + source[storage_start:storage_end] + source[diagnostic_start:diagnostic_end] + r'''
int main(){
 auto &diag=x9m_jobs[0], &list=x9m_jobs[1], &other_sim=x9m_jobs[2];
 diag={X9M_DIAG,0,0,0,false};
 list={X9M_LIST,0,0,1,false};
 other_sim={X9M_READ,1,0,2,false};
 assert(x9m_diag_storage_busy(0,&diag)&&!x9m_store_blocked(list));
 diag.token=3;
 assert(x9m_store_blocked(list)&&!x9m_store_blocked(other_sim));
 diag.token=0;list.token=4;
 assert(x9m_diag_storage_busy(0,&diag));
 list={};
 assert(!x9m_diag_storage_busy(0,&diag));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'schedule.cpp'
            binary = pathlib.Path(tmp) / 'schedule'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_actual_cmgr_fields(self):
        source = SOURCE.read_text()
        start = source.index('   if(j.step){\n    const uint8_t index[2]={1,0};', source.index('static void x9m_poll()'))
        end = source.index('\n   j.token=f.submit();', start)
        code = r'''
#include <cstdint>
struct Job { unsigned step; };
struct Frame {
 unsigned value[0x105]={}; bool seen[0x105]={}; unsigned index=0;
 void byte(unsigned tag,unsigned v) { seen[tag]=true; value[tag]=v; }
 void tlv(unsigned tag,const uint8_t *p,unsigned n) {
  if(tag==0x102 && n==2) { seen[tag]=true; index=p[0]|(p[1]<<8); }
 }
};
static void apply(Job j,Frame &f) {
''' + source[start:end] + r'''
}
int main() {
 Frame f; apply({1},f);
 if(!f.seen[0x100]||f.value[0x100]!=3||!f.seen[0x101]||f.value[0x101]!=5) return 1;
 if(!f.seen[0x102]||f.index!=1) return 2;
 if(!f.seen[0x103]||f.value[0x103]!=1) return 3;
 if(!f.seen[0x104]||f.value[0x104]!=0) return 4;
 Frame query; apply({0},query); if(query.seen[0x103]) return 5;
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'frame.cpp'
            binary = pathlib.Path(tmp) / 'frame'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    unittest.main()
