#!/usr/bin/env python3
"""Exercise the real active-storage owner guard with compiled host stubs."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'

class StorageReuse(unittest.TestCase):
    def test_guard_and_admission_order(self):
        text = SOURCE.read_text()
        start = text.index('static bool x9m_storage_repeated(')
        end = text.index('\nstruct x9m_incoming {', start)
        branch = text.index('}else if(service==5&&(id==0x31||id==0x22||id==0x23)){')
        guard = text.index('if(x9m_storage_repeated(', branch)
        decode = text.index('x9ms_storage_decode(req,len,&storage)', branch)
        self.assertLess(guard, decode)
        code = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
struct x9m_job { unsigned op,slot,len; uint64_t generation; uint8_t req[64]; };
static x9m_job x9m_jobs[4];
static bool x9m_storage(unsigned op) { return op>=5&&op<=7; }
static unsigned x9q_u16(const uint8_t *p) { return p[0]|(p[1]<<8); }
''' + text[start:end] + r'''
int main() {
 uint8_t req[7]={0,42,0,0x31,0,0,0};
 x9m_job &j=x9m_jobs[0]; j.op=5;j.slot=2;j.len=7;j.generation=10; memcpy(j.req,req,7);
 if(!x9m_storage_repeated(2,10,req,7))return 1;
 req[3]=0x23; if(!x9m_storage_repeated(2,10,req,7))return 2;
 if(x9m_storage_repeated(3,10,req,7)||x9m_storage_repeated(2,11,req,7))return 3;
 req[1]=43; if(x9m_storage_repeated(2,10,req,7))return 4;
 req[1]=42; j.op=1; if(x9m_storage_repeated(2,10,req,7))return 5;
 if(x9m_storage_repeated(2,10,nullptr,7)||x9m_storage_repeated(2,10,req,6))return 6;
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src=pathlib.Path(tmp)/'guard.cpp'; binary=pathlib.Path(tmp)/'guard'
            src.write_text(code)
            subprocess.run(['c++','-std=c++17','-Wall','-Werror',str(src),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

if __name__ == '__main__':
    unittest.main()
