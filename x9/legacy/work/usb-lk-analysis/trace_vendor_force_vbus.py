"""Execute only the captured driver's force-VBUS policy against fake MMIO.
No controller/PHY emulation, USB access, or real MMIO occurs.
"""
from pathlib import Path
import json,struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *

ROOT=Path(__file__).parent
SOURCE=Path('/Users/tosasitill/Downloads/01-Android/内测/x9内测/vendor_boot_unpack/ramdisk_0/lib/modules/mtu3.ko')


def text_section():
    data=SOURCE.read_bytes()
    offset=struct.unpack_from('<Q',data,40)[0]
    size,count,names=struct.unpack_from('<HHH',data,58)
    sections=[struct.unpack_from('<IIQQQQIIQQ',data,offset+i*size) for i in range(count)]
    header=sections[names];strings=data[header[4]:header[4]+header[5]]
    for section in sections:
        end=strings.index(0,section[0])
        if strings[section[0]:end]==b'.text':return data[section[4]:section[4]+section[5]]
    raise ValueError('no .text')


def trace(vbus_on,keep_ao):
    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    base=0x10000000;context=0x20000000;stop=base+0x30000
    uc.mem_map(base,0x40000);uc.mem_write(base,text_section())
    uc.mem_map(context,0x20000);uc.mem_map(0x16750000,0x10000)
    uc.mem_write(context+0x10,(0x16751000).to_bytes(8,'little'))
    uc.mem_write(context+0x18,(0x16753e00).to_bytes(8,'little'))
    uc.mem_write(context+0x1a0,bytes([1,int(keep_ao)]))
    uc.mem_write(0x16753e50,(0x18c8).to_bytes(4,'little'))
    uc.mem_write(0x16751c84,(0x80000003).to_bytes(4,'little'))
    uc.reg_write(UC_ARM64_REG_SP,context+0x10000)
    uc.reg_write(UC_ARM64_REG_X0,context);uc.reg_write(UC_ARM64_REG_W1,int(vbus_on))
    uc.reg_write(UC_ARM64_REG_X30,stop)
    writes=[]
    def hook(uc,pc,size,user):
        if pc==stop:uc.emu_stop();return
        rva=pc-base
        if rva==0x89cc: # instrumented readl wrapper
            address=uc.reg_read(UC_ARM64_REG_X0)
            uc.reg_write(UC_ARM64_REG_W0,int.from_bytes(uc.mem_read(address,4),'little'))
            uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30))
        elif rva==0x9228: # instrumented writel(value,address) wrapper
            address=uc.reg_read(UC_ARM64_REG_X1);value=uc.reg_read(UC_ARM64_REG_W0)
            uc.mem_write(address,value.to_bytes(4,'little'))
            writes.append({'address':hex(address),'value':hex(value)})
            uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30))
    uc.hook_add(UC_HOOK_CODE,hook)
    uc.emu_start(base+0x87f4,stop,timeout=1000000,count=2000)
    if uc.reg_read(UC_ARM64_REG_PC)!=stop:raise RuntimeError('function did not return')
    expected=(0x18c8 if keep_ao else 0x1848) if vbus_on else 0x18c8
    actual=int.from_bytes(uc.mem_read(0x16753e50,4),'little')
    assert actual==expected,(hex(actual),hex(expected))
    return {'vbus_on':vbus_on,'keep_ao':keep_ao,'writes':writes}


if __name__=='__main__':
    results=[trace(on,ao) for on in (True,False) for ao in (False,True)]
    output={'source':str(SOURCE),'function':'ssusb_set_force_vbus','text_rva':['0x87f4','0x8894'],
            'input_u2':'0x18c8','input_misc':'0x80000003','cases':results,
            'limits':'Captured module policy under explicit structure/MMIO fixtures; not proof of hardware root cause.'}
    (ROOT/'vendor-force-vbus-trace.json').write_text(json.dumps(output,indent=2)+'\n')
    print(json.dumps(results,indent=2))
