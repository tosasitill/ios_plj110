#!/usr/bin/env python3
"""Host-check the actual diagnostic CMGR frame construction, not modem I/O."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'

class DiagnosticReadOnly(unittest.TestCase):
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
