"""Trace stock LK's U2 start suffix without executing any device I/O.
Unicorn RAM backs MMIO; clock/capability values are explicit analysis fixtures.
This proves instruction effects/order, not hardware behavior or boot reachability.
"""
from pathlib import Path
import json
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *

HERE=Path(__file__).parent
SOURCE=Path('/Users/tosasitill/Downloads/01-Android/内测/x9内测/final/x9/partitions/lk_a.img')
BASE=0xffff00008d800000
IO=0xffff000000000000


def trace(u3_capable=False):
    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    uc.mem_map(BASE,0x400000)
    uc.mem_write(BASE,SOURCE.read_bytes()[0x200:0x1c0550])
    uc.mem_map(IO+0x16750000,0x20000)
    uc.mem_map(0x10000000,0x20000)
    uc.reg_write(UC_ARM64_REG_SP,0x10010000)
    uc.reg_write(UC_ARM64_REG_X29,0x10010000)
    uc.reg_write(UC_ARM64_REG_X21,BASE+0x21a000)
    def w32(address,value):uc.mem_write(address,int(value).to_bytes(4,'little'))
    w32(IO+0x16753e10,0x01010d0f)
    w32(IO+0x16753e14,1)
    w32(IO+0x16753e50,0x48)
    w32(IO+0x16753404,0x60e0)
    w32(IO+0x16760368,0x02b600c4)
    w32(IO+0x1676036c,0)
    # Fields established by udc_init, represented explicitly here.
    w32(BASE+0x21ae64,2) # USB_SPEED_HIGH profile
    uc.mem_write(BASE+0x21afb6,bytes([2 if u3_capable else 0]))
    uc.mem_write(BASE+0x21af50,(BASE+0x220000).to_bytes(8,'little'))
    events=[]
    def code(uc,address,size,user):
        rva=address-BASE
        if rva==0xbff48:
            uc.emu_stop();return
        if rva==0x970d8:
            events.append({'kind':'stubbed_tail_delay','delay_us':uc.reg_read(UC_ARM64_REG_W0)})
            uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30))
            return
        word=int.from_bytes(uc.mem_read(address,4),'little')
        if word&0xfc000000==0x94000000:
            imm=word&0x3ffffff
            if imm&(1<<25):imm-=1<<26
            target=rva+imm*4
            if target==0xc1c30:
                events.append({'kind':'call','rva':hex(rva),'target':'stock PHY recovery 0xc1c30'})
                return
            events.append({'kind':'stubbed_call','rva':hex(rva),'target':hex(target),
                           'w0':uc.reg_read(UC_ARM64_REG_W0)})
            if target==0x970d8:events[-1]['delay_us']=uc.reg_read(UC_ARM64_REG_W0)
            uc.reg_write(UC_ARM64_REG_X30,address+4)
            uc.reg_write(UC_ARM64_REG_X0,0)
            uc.reg_write(UC_ARM64_REG_PC,address+4)
    def write(uc,access,address,size,value,user):
        if IO+0x16750000<=address<IO+0x16770000:
            events.append({'kind':'mmio_write','rva':hex(uc.reg_read(UC_ARM64_REG_PC)-BASE),
                           'address':hex(address-IO),'bytes':size,'value':hex(value)})
    uc.hook_add(UC_HOOK_CODE,code)
    uc.hook_add(UC_HOOK_MEM_WRITE,write)
    uc.emu_start(BASE+0xbfb04,BASE+0xbff48,timeout=2000000,count=100000)
    if uc.reg_read(UC_ARM64_REG_PC)!=BASE+0xbff48:
        raise RuntimeError(f'stock suffix stopped at PC={uc.reg_read(UC_ARM64_REG_PC):#x}; last events={events[-5:]}')
    return {'source':str(SOURCE),'linked_text_base':hex(BASE),'text_file_offset':'0x200',
            'trace_rva':['0xbfb04','0xbff48'],'u3_capable_fixture':u3_capable,
            'limitations':'Mode/clock/MMIO fixtures and stubbed non-MMIO calls; no hardware/PHY/bus emulation.',
            'events':events}


if __name__=='__main__':
    result=trace()
    (HERE/'lk-u2-start-instruction-trace.json').write_text(json.dumps(result,indent=2)+'\n')
    for event in result['events']:
        if event['kind']=='mmio_write':print(event['rva'],event['address'],event['value'])
