#!/usr/bin/env python3
"""Exercise production native storage ordering independently of host receipt admission."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

class StorageQueue(unittest.TestCase):
    def test_native_ownership_and_receipt_backpressure(self):
        source = (ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc').read_text()
        start = source.index('static bool x9m_store_blocked(')
        end = source.index('static uint64_t x9m_store_submit(', start)
        code = r'''
#include <cstdint>
#define X9M_DIAG 8
struct x9m_job { unsigned op=0,sim=0; uint64_t token=0,store_order=0; bool hold=false,follow=false,receipt_pending=false; };
static x9m_job x9m_jobs[4];
static bool x9m_storage(unsigned op) {return op==5 || op==6 || op==7;}
''' + source[start:end] + r'''
int main() {
 auto &old=x9m_jobs[0]; auto &next=x9m_jobs[1];
 old.op=5;old.sim=0;old.store_order=1;
 next.op=6;next.sim=0;next.store_order=2;next.follow=true;
 // A pending terminal receipt is still owned, but its native CNF is drained.
 old.receipt_pending=true;
 if(x9m_store_blocked(next)||!old.receipt_pending)return 1;
 old.follow=true;if(!x9m_store_blocked(next))return 2;
 old.follow=false;old.token=42;if(!x9m_store_blocked(next))return 3;
 old.token=0;old.hold=true;if(!x9m_store_blocked(next))return 4;
 old.sim=1;if(x9m_store_blocked(next))return 5;
 old.sim=0;old.hold=false;old.op=X9M_DIAG;old.token=42;
 if(!x9m_store_blocked(next))return 6;
 old.token=0;if(x9m_store_blocked(next))return 7;
 old.op=7;old.follow=true;old.store_order=3;
 if(x9m_store_blocked(next))return 8;
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            binary = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    unittest.main()
