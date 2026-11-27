#!/usr/bin/env python3
"""WDS 0x5566/0x5568 status notifications: accept, record, acknowledge.

The handler is extracted from X9WDS.inc and run against synthetic QMI
requests; admission and the no-routing guarantee are source contracts.
"""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9WDS.inc'


class WdsDataNotice(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'test.cpp'
            exe = pathlib.Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_accept_record_and_reject_malformed(self):
        source = SOURCE.read_text()
        start = source.index('static uint8_t x9w_cellular_status')
        end = source.index('static bool x9w_request(', start)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
static const unsigned X9Q_ARG = 48;
struct x9q_view {const uint8_t *p; unsigned n;};
static unsigned x9q_u16(const uint8_t *p){return p[0]|(p[1]<<8);}
static x9q_view x9q_tlv(const uint8_t *p,unsigned n,uint8_t tag){
 for(unsigned i=7;i+3<=n;){unsigned k=x9q_u16(p+i+1);if(k>n-i-3)break;
  if(p[i]==tag)return {p+i+3,k};i+=3+k;}
 return {nullptr,0};
}
''' + source[start:end] + r'''
static unsigned call(unsigned id,unsigned sim,uint8_t value,unsigned n=11,uint8_t tag=0x10,unsigned vlen=1){
 uint8_t req[16]={};req[3]=(uint8_t)id;req[4]=(uint8_t)(id>>8);
 req[7]=tag;req[8]=(uint8_t)vlen;req[9]=(uint8_t)(vlen>>8);req[10]=value;
 return x9w_data_notice(id,sim,req,n);
}
int main(){
 /* Accept + record, per SIM and per message. */
 assert(call(0x5566,0,1)==0);
 assert(x9w_cellular_status_seen[0]&&x9w_cellular_status[0]==1);
 assert(!x9w_cellular_status_seen[1]&&!x9w_switch_allowed_seen[0]);
 assert(call(0x5566,1,0)==0);
 assert(x9w_cellular_status_seen[1]&&x9w_cellular_status[1]==0);
 assert(call(0x5568,1,1)==0);
 assert(x9w_switch_allowed_seen[1]&&x9w_switch_allowed[1]==1);
 assert(!x9w_switch_allowed_seen[0]);
 /* Latest value wins. */
 assert(call(0x5566,0,0)==0&&x9w_cellular_status[0]==0);
 assert(call(0x5566,0,7)==0&&x9w_cellular_status[0]==7);
 /* Malformed requests change nothing. */
 assert(call(0x5566,0,1,10)==X9Q_ARG);
 assert(call(0x5566,0,1,12)==X9Q_ARG);
 assert(call(0x5568,0,1,11,0x11)==X9Q_ARG);
 assert(call(0x5568,0,1,11,0x10,2)==X9Q_ARG);
 assert(call(0x5566,2,1)==X9Q_ARG);
 assert(x9w_cellular_status[0]==7&&!x9w_switch_allowed_seen[0]);
 printf("wds-data-notice ok\n");}
'''
        out = self.compile_run(code)
        self.assertIn('wds-data-notice ok', out)
        self.assertIn('cellular data status', out)
        self.assertIn('data switch allowed', out)

    def test_admission_dispatch_and_no_routing(self):
        source = SOURCE.read_text()
        admit = re.search(r'if\(id!=1&&id!=0x20.*?return false;', source, re.S)
        self.assertIsNotNone(admit)
        self.assertIn('id!=0x5566', admit.group(0))
        self.assertIn('id!=0x5568', admit.group(0))
        branch = re.search(r'\}else if\(id==0x5566\|\|id==0x5568\)\{\n(.*?)\n \}else if', source, re.S)
        self.assertIsNotNone(branch)
        self.assertEqual(branch.group(1).strip(), 'error=x9w_data_notice(id,c.sim_slot,req,n);')
        start = source.index('static uint8_t x9w_cellular_status')
        end = source.index('static bool x9w_request(', start)
        helper = source[start:end]
        # The notifications must never reach DDS routing or bearer state.
        for forbidden in ('x9ds', 'sim_slot=', 'x9w_bearer', 'lease'):
            self.assertNotIn(forbidden, helper)


if __name__ == '__main__':
    unittest.main()
