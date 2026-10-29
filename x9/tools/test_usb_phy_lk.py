"""Compare compiled C recovery against decoded stock LK instruction effects."""
import ctypes
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest
import capstone
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_REG

STOCK=Path('/Users/tosasitill/Downloads/01-Android/内测/x9内测/final/x9/partitions/lk_a.img')
SHIM=Path(__file__).resolve().parents[1]/'shim'


def stock_trace(memory):
    code=STOCK.read_bytes()[0x200+0xc1c30:0x200+0xc1d20]
    decoder=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM);decoder.detail=True
    registers={};events=[]
    def get(name):return registers.get('x'+name[1:] if name.startswith('w') else name,0)
    def operand(ins,op):
        if op.type==ARM64_OP_IMM:return op.imm
        if op.type==ARM64_OP_REG:return get(ins.reg_name(op.reg))
        raise AssertionError(ins.op_str)
    def put(name,value):
        bits=32 if name.startswith('w') else 64
        registers['x'+name[1:] if name.startswith('w') else name]=value&((1<<bits)-1)
    for ins in decoder.disasm(code,0xc1c30):
        name=ins.mnemonic;ops=ins.operands
        if name in ['paciasp','autiasp','stp','ldp']:continue
        if name=='mov' and 'sp' in ins.op_str:continue
        if name in ['ldr','ldur','str','stur']:
            mem=ops[1].mem;base=ins.reg_name(mem.base)
            if base=='sp':continue
            address=(get(base)+mem.disp)&0xffffffffffff
            if name in ['ldr','ldur']:put(ins.reg_name(ops[0].reg),memory[address])
            else:
                value=get(ins.reg_name(ops[0].reg))&0xffffffff
                memory[address]=value;events.append(('write',address,value))
        elif name=='mov':put(ins.reg_name(ops[0].reg),operand(ins,ops[1]))
        elif name=='movk':
            reg=ins.reg_name(ops[0].reg);shift=ops[1].shift.value
            put(reg,(get(reg)&~(0xffff<<shift))|(ops[1].imm<<shift))
        elif name in ['and','orr']:
            a,b=operand(ins,ops[1]),operand(ins,ops[2])
            put(ins.reg_name(ops[0].reg),a&b if name=='and' else a|b)
        elif name in ['bl','b']:
            if ops[0].imm!=0x970d8:raise AssertionError('unexpected call in stock PHY recovery')
            events.append(('delay',get('w0')))
            if name=='b':break
        else:raise AssertionError(f'unhandled stock instruction: {ins.mnemonic} {ins.op_str}')
    return events


def vendor_power_trace(memory):
    capture=SHIM.parents[1]/'work/usb-lk-analysis/vendor-xsphy.text'
    code=capture.read_bytes()
    decoder=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM);decoder.detail=True
    instructions={i.address:i for i in decoder.disasm(code[0x53a8:0x5554],0x53a8)}
    registers={'x26':0x16760000,'x20':0x50000000};events=[];equal=False
    def get(name):return registers.get('x'+name[1:] if name.startswith('w') else name,0)
    def put(name,value):registers['x'+name[1:] if name.startswith('w') else name]=value&((1<<(32 if name.startswith('w') else 64))-1)
    def value(ins,op):return op.imm if op.type==ARM64_OP_IMM else get(ins.reg_name(op.reg))
    pc=0x53a8
    while pc<0x5554:
        ins=instructions[pc];ops=ins.operands;pc+=4
        name=ins.mnemonic
        if name=='add':put(ins.reg_name(ops[0].reg),value(ins,ops[1])+value(ins,ops[2]))
        elif name=='mov':put(ins.reg_name(ops[0].reg),value(ins,ops[1]))
        elif name=='movk':
            reg=ins.reg_name(ops[0].reg);shift=ops[1].shift.value
            put(reg,(get(reg)&~(0xffff<<shift))|(ops[1].imm<<shift))
        elif name in ['and','orr']:
            a,b=value(ins,ops[1]),value(ins,ops[2]);put(ins.reg_name(ops[0].reg),a&b if name=='and' else a|b)
        elif name=='ldrb':
            if get(ins.reg_name(ops[1].mem.base))+ops[1].mem.disp!=0x500000b0:
                raise AssertionError('unexpected vendor instance field')
            put(ins.reg_name(ops[0].reg),0) # captured DT has no hwpll-mode property
        elif name=='cmp':equal=value(ins,ops[0])==value(ins,ops[1])
        elif name=='b.ne':
            if not equal:pc=ops[0].imm
        elif name=='bl':
            target=ops[0].imm
            if target==0x2d6c:put('w0',memory[get('x0')])
            elif target==0x2dec:
                address=get('x1');v=get('w0');memory[address]=v;events.append(('write',address,v))
            elif target==ins.address:
                # The relocatable module calls __const_udelay here. Its
                # argument uses fixed-point microseconds scaled by 2**32/1e6.
                events.append(('delay',round(get('w0')*1000000/(1<<32))))
            else:raise AssertionError('unexpected call in vendor core sequence')
        else:raise AssertionError(f'unhandled vendor instruction {name} {ins.op_str}')
    return events


@unittest.skipUnless(STOCK.exists(),'requires captured X9 LK input')
class StockPHYTests(unittest.TestCase):
    def test_compiled_recovery_matches_stock_lk_writes_and_delays(self):
        with tempfile.TemporaryDirectory(prefix='x9-usb-phy-') as tmp:
            lib=Path(tmp)/'phy.dylib'
            subprocess.run([shutil.which('clang'),'-O2','-shared','-fPIC','-Wall','-Wextra','-Werror',
                            str(SHIM/'usb_phy_lk.c'),'-o',str(lib)],check=True)
            module=ctypes.CDLL(str(lib))
            read_type=ctypes.CFUNCTYPE(ctypes.c_uint32,ctypes.c_void_p,ctypes.c_size_t)
            write_type=ctypes.CFUNCTYPE(None,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32)
            delay_type=ctypes.CFUNCTYPE(None,ctypes.c_void_p,ctypes.c_uint)
            module.x9_usb_phy_recover.argtypes=[ctypes.c_void_p,read_type,write_type,delay_type]
            addresses=[0x16760300,0x16760314,0x16760318,0x16760320,0x16760368,0x1676036c]
            for seed in range(8):
                rng=random.Random(seed);initial={a:rng.getrandbits(32) for a in addresses}
                expected_memory=initial.copy();expected=stock_trace(expected_memory)
                actual_memory=initial.copy();actual=[]
                def read(_,address):return actual_memory[address]
                def write(_,address,value):
                    actual_memory[address]=value;actual.append(('write',address,value))
                def delay(_,us):actual.append(('delay',us))
                module.x9_usb_phy_recover(None,read_type(read),write_type(write),delay_type(delay))
                self.assertEqual(actual,expected)
                self.assertEqual(actual_memory,expected_memory)
                self.assertEqual([e for e in actual if e[0]=='delay'],[('delay',2000),('delay',2000)])


    def test_compiled_core_power_on_matches_captured_vendor_module(self):
        with tempfile.TemporaryDirectory(prefix='x9-xsphy-on-') as tmp:
            lib=Path(tmp)/'phy.dylib'
            subprocess.run([shutil.which('clang'),'-O2','-shared','-fPIC','-Wall','-Wextra','-Werror',str(SHIM/'usb_phy_lk.c'),'-o',str(lib)],check=True)
            module=ctypes.CDLL(str(lib))
            read_type=ctypes.CFUNCTYPE(ctypes.c_uint32,ctypes.c_void_p,ctypes.c_size_t)
            write_type=ctypes.CFUNCTYPE(None,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32)
            delay_type=ctypes.CFUNCTYPE(None,ctypes.c_void_p,ctypes.c_uint)
            module.x9_usb_phy_power_on.argtypes=[ctypes.c_void_p,read_type,write_type,delay_type]
            addresses=[0x16760300,0x16760318,0x16760320,0x16760344,0x16760368,0x1676036c]
            for seed in range(8):
                rng=random.Random(seed);initial={a:rng.getrandbits(32) for a in addresses}
                expected_memory=initial.copy();expected=vendor_power_trace(expected_memory)
                actual_memory=initial.copy();actual=[]
                def read(_,address):return actual_memory[address]
                def write(_,address,v):actual_memory[address]=v;actual.append(('write',address,v))
                def delay(_,us):actual.append(('delay',us))
                module.x9_usb_phy_power_on(None,read_type(read),write_type(write),delay_type(delay))
                self.assertEqual(actual,expected)
                self.assertEqual(actual_memory,expected_memory)
                self.assertEqual([e for e in actual if e[0]=='delay'],[('delay',250),('delay',30),('delay',800)])


    def test_device_reset_matches_stock_lk_local_reset_and_preserves_other_bits(self):
        code=STOCK.read_bytes()[0x200+0xbfb14:0x200+0xbfb38]
        decoder=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM);decoder.detail=True
        decoded=list(decoder.disasm(code,0xbfb14))
        self.assertEqual([i.mnemonic for i in decoded],['ldr','mov','orr','str','bl','ldr','mov','and','str'])
        # The unrelated mov w22,#0x501 is the next clock-check mask. The
        # pulse touches only +0x68 from x20=0x16753e30; the call is LK's delay.
        self.assertEqual(decoded[0].operands[1].mem.disp,0x68)
        self.assertEqual(decoded[1].operands[1].imm,1)
        self.assertEqual(decoded[4].operands[0].imm,0x970d8)
        with tempfile.TemporaryDirectory(prefix='x9-udc-reset-') as tmp:
            output=Path(tmp)/'reset.dylib'
            subprocess.run([shutil.which('clang'),'-O2','-shared','-fPIC',str(SHIM/'usb_phy_lk.c'),'-o',str(output)],check=True)
            module=ctypes.CDLL(str(output))
            read_type=ctypes.CFUNCTYPE(ctypes.c_uint32,ctypes.c_void_p,ctypes.c_size_t)
            write_type=ctypes.CFUNCTYPE(None,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32)
            delay_type=ctypes.CFUNCTYPE(None,ctypes.c_void_p,ctypes.c_uint)
            module.x9_usb_device_reset.argtypes=[ctypes.c_void_p,read_type,write_type,delay_type]
            for initial in [0,0x48,0xa5a50000,0xffffffff]:
                value=initial;events=[]
                def read(_,address):
                    self.assertEqual(address,0x16753e98);return value
                def write(_,address,new):
                    nonlocal value
                    self.assertEqual(address,0x16753e98);value=new;events.append(('write',address,new))
                def delay(_,us):events.append(('delay',us))
                module.x9_usb_device_reset(None,read_type(read),write_type(write),delay_type(delay))
                self.assertEqual(events,[('write',0x16753e98,initial|1),('delay',1),('write',0x16753e98,initial&~1)])


if __name__=='__main__':unittest.main()
