"""Reproduce the current poll_ep0 STALL-clear write using its actual C body.
Fake MMIO implements the documented EP0 W1C mask. No USB or device access.
"""
from pathlib import Path
import ctypes,json,shutil,subprocess,tempfile

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'x9/shim/usb_mtu3.c'
source=SOURCE.read_text()
start=source.index('static void poll_ep0(void)')
brace=source.index('{',start);depth=1;end=brace+1
while depth:
    if source[end]=='{':depth+=1
    elif source[end]=='}':depth-=1
    end+=1
body=source[start:end]
defines='\n'.join(line for line in source.splitlines() if line.startswith('#define '))
harness=r'''
#include <stdint.h>
#include <stddef.h>
#include "ramrx.h"
DEFINES
static struct ramrx session;
static uint8_t control[RAMRX_FRAME_MAX];
static unsigned transferred,wanted,in_length,command,configured;
static enum { IDLE, RECEIVE, SEND, SEND_END } phase;
static uint32_t csr_value,isr_value,last_csr_write;
static uint32_t rr(unsigned offset) {
    if(offset==EP0)return csr_value;
    if(offset==EPISR)return isr_value;
    return 0;
}
static void wr(unsigned offset,uint32_t value) {
    if(offset==EPISR)isr_value&=~value;
    if(offset==EP0) {
        last_csr_write=value;
        csr_value=(value&~W1C)|((csr_value&W1C)&~value);
    }
}
static uint32_t epcsr(void){return rr(EP0)&~W1C;}
static void setup(void){}
static void stall(unsigned n){(void)n;}
static void fifo_read(uint8_t *p,unsigned n){(void)p;(void)n;}
static void transmit(void){}
POLL_BODY
uint32_t run_stall_case(uint32_t *write_value) {
    phase=IDLE;isr_value=1;csr_value=64|STALL|SENTSTALL;
    poll_ep0();*write_value=last_csr_write;return csr_value;
}
'''.replace('DEFINES',defines).replace('POLL_BODY',body)
with tempfile.TemporaryDirectory(prefix='x9-ep0-audit-') as tmp:
    tmp=Path(tmp);c=tmp/'case.c';c.write_text(harness);library=tmp/'case.dylib'
    subprocess.run([shutil.which('clang'),'-O0','-shared','-fPIC','-I',str(ROOT/'x9/shim'),str(c),str(ROOT/'x9/shim/ramrx.c'),'-o',str(library)],check=True)
    module=ctypes.CDLL(str(library));module.run_stall_case.argtypes=[ctypes.POINTER(ctypes.c_uint32)];module.run_stall_case.restype=ctypes.c_uint32
    write=ctypes.c_uint32();result=module.run_stall_case(ctypes.byref(write))
    assert not result&(1<<25),'SENDSTALL still asserted after clear'
    assert not result&(1<<22),'SENTSTALL was not acknowledged'
    output={'source':str(SOURCE),'function':'poll_ep0','input_ep0csr':hex(64|(1<<25)|(1<<22)),
            'actual_csr_write':hex(write.value),'fake_w1c_ep0csr_after':hex(result),'sendstall_still_set':False,
            'stock_lk_clear_mask_rva':'0xc0648..0xc0660','stock_lk_csr_write':hex((64|(1<<25)|(1<<22))&0xfdbcffff|(1<<22)),
            'limitations':'Actual C body, mocked documented W1C behavior; no real USB controller execution.'}
    (Path(__file__).parent/'shim-ep0-stall-fixed.json').write_text(json.dumps(output,indent=2)+'\n')
    print(json.dumps(output,indent=2))
