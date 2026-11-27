#!/usr/bin/env python3
"""Compile the real post-take completion branch with a controlled clock."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'

class StorageDeadline(unittest.TestCase):
    def test_completion_guard(self):
        source = SOURCE.read_text()
        start = source.index('   if(taken==1){', source.index('static void x9m_poll()'))
        end = source.index('\n  }\n }\n x9m_ims_poll(', start)
        block = source[start:end]
        harness = r'''
#include <cstring>
#include <cstdio>
#include <cstdint>
#define bzero(p,n) std::memset(p,0,n)
#define X9Q_NOT_READY 7
#define X9M_SEND 3
struct Job { unsigned op, sim; bool answered, hold, follow; uint64_t deadline; unsigned char req[16], stored_pdu[16]; uint64_t token=0; bool receipt_pending=false; };
static uint64_t clock_ms; static int error_count, done_count; static bool answered_at_done;
static bool x9m_storage(unsigned op) { return op==1; }
static uint64_t x9q_ms() { return clock_ms; }
static void x9m_error(Job &j, int) { error_count++; j.answered=true; }
static void x9m_job_done(Job &j, const uint8_t *, unsigned) { done_count++; answered_at_done=j.answered; }
static void run(Job &j, uint64_t now) {
 clock_ms=now; error_count=done_count=0; answered_at_done=false;
 int taken=1; uint8_t p[16]={}; unsigned n=0;
 for (int once=0;once<1;once++) {
''' + block + r'''
 }
}
int main() {
 Job a={1,0,false,false,false,100,{},{} }; run(a,99);
 if(error_count||done_count!=1||answered_at_done) return 1;
 Job b={1,0,false,false,false,100,{},{} }; run(b,100);
 if(error_count!=1||done_count!=1||!answered_at_done) return 2;
 Job c={1,0,true,false,false,100,{},{} }; run(c,101);
 if(error_count||done_count!=1||!answered_at_done) return 3;
 Job d={2,0,false,false,false,100,{},{} }; run(d,101);
 if(error_count||done_count!=1||answered_at_done) return 4;
 Job e={3,0,false,false,false,100,{},{} }; run(e,99);
 if(error_count||done_count!=1||answered_at_done) return 5;
 Job f={3,0,false,false,false,100,{},{} }; run(f,100);
 if(error_count!=1||done_count!=1||!answered_at_done) return 6;
 Job g={3,0,true,false,false,100,{},{} }; run(g,101);
 if(error_count||done_count!=1||!answered_at_done) return 7;
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            binary = pathlib.Path(tmp) / 'test'
            src.write_text(harness)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    unittest.main()
