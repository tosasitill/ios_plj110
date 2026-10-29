"""Native receiver, LZ4 and final RAM state tests; no USB hardware emulation."""
import ctypes
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
import lz4.block
from send_ramroot import frame, upload, status, command, BEGIN, DATA, COMMIT, BOOT, ABORT, MAGIC, BLOCK
from shared import adt, payload, load
import mkadt

stub=load('x9_receiver_test_stub','x676c/tools/stub/mkstub.py')
SHIM=Path(__file__).resolve().parents[1]/'shim'


class Device:
    def __init__(self,lib,capacity=8*1024*1024):
        self.lib=lib
        self.session=ctypes.create_string_buffer(65536)
        self.memory=ctypes.create_string_buffer(capacity+1)
        lib.ramrx_init(self.session,self.memory,capacity)
        self.lost_ack=False
        self.lose_next=False

    def ctrl_transfer(self,direction,request,value,index,data,timeout):
        if direction==0xc0:
            result=ctypes.create_string_buffer(48)
            self.lib.ramrx_status(self.session,result)
            return result.raw
        data=bytes(data);buf=ctypes.create_string_buffer(data)
        result=self.lib.ramrx_command(self.session,request,buf,len(data))
        if result:raise IOError('receiver stall')
        if request==DATA and self.lose_next:
            self.lose_next=False;self.lost_ack=True
            raise TimeoutError('simulated lost control status packet')
        return len(data)


class USBReceiverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='x9-usb-ramrx-')
        cls.work=Path(cls.tmp.name)
        output=cls.work/'receiver.dylib'
        subprocess.run([shutil.which('clang'),'-O2','-shared','-fPIC','-Wall','-Wextra','-Werror',
                        *[str(SHIM/n) for n in ['ramrx.c','resident.c','validate.c','fdt_ram.c']],
                        '-o',str(output)],check=True)
        cls.lib=ctypes.CDLL(str(output))
        cls.lib.ramrx_init.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_uint64]
        cls.lib.ramrx_command.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_void_p,ctypes.c_size_t]
        cls.lib.ramrx_status.argtypes=[ctypes.c_void_p,ctypes.c_void_p]
        cls.lib.ramrx_lz4.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t]
        cls.lib.resident_patch_adt.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64,ctypes.c_uint64]
        cls.lib.resident_layout.argtypes=[ctypes.c_void_p,ctypes.c_size_t,
                                         ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.resident_layout.restype=ctypes.c_char_p
        cls.lib.resident_layout_ufs.argtypes=cls.lib.resident_layout.argtypes
        cls.lib.resident_layout_ufs.restype=ctypes.c_char_p
        cls.lib.fdt_ram_available.argtypes=[ctypes.c_void_p,ctypes.c_uint64,ctypes.c_uint64]

    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()

    def begin(self,device,data):
        command(device,BEGIN,struct.pack('<8sQII',MAGIC,len(data),zlib.crc32(data),1))

    def test_host_sender_to_native_receiver_verifies_exact_resident_bytes(self):
        root=bytearray(8*1024*1024)
        root[1024:1026]=b'HX';struct.pack_into('>II',root,1064,4096,len(root)//4096)
        sample=b'original-iOS-bin!'*1000
        root[65536:65536+len(sample)]=sample
        root[-BLOCK:]=random.Random(71).randbytes(BLOCK)
        path=self.work/'root.hfs';path.write_bytes(root)
        device=Device(self.lib);device.lose_next=True
        result=upload(device,path,lambda b:lz4.block.compress(b,store_size=False),boot=True,output=lambda x:None)
        self.assertEqual(result.state,2)
        self.assertEqual(status(device).state,3)
        self.assertTrue(device.lost_ack)
        self.assertEqual(device.memory.raw[:-1],bytes(root))

    def test_bad_block_does_not_mutate_destination_or_advance_offset(self):
        device=Device(self.lib,4096);data=b'original'*128;self.begin(device,data)
        bad=bytearray(frame(0,data));bad[-1]^=1
        before=device.memory.raw
        with self.assertRaises(IOError):command(device,DATA,bad)
        self.assertEqual(status(device).received,0)
        self.assertEqual(device.memory.raw,before)
        with self.assertRaises(IOError):command(device,COMMIT)
        command(device,DATA,frame(0,data))
        with self.assertRaises(IOError):command(device,DATA,frame(0,data))
        command(device,COMMIT)
        self.assertEqual(status(device).state,2)
        command(device,ABORT)
        self.assertEqual(status(device).state,0)
        with self.assertRaises(IOError):command(device,BOOT)

    def test_capacity_and_commit_crc_cannot_be_bypassed(self):
        device=Device(self.lib,64)
        with self.assertRaises(IOError):self.begin(device,bytes(65))
        command(device,BEGIN,struct.pack('<8sQII',MAGIC,16,1,1))
        command(device,DATA,frame(0,bytes(16)))
        with self.assertRaises(IOError):command(device,COMMIT)
        with self.assertRaises(IOError):command(device,BOOT)
        self.assertEqual(status(device).state,1)

    def test_lz4_decodes_original_blocks_and_rejects_bad_lengths_and_offsets(self):
        samples=[bytes(BLOCK),b'A'*BLOCK,random.Random(3).randbytes(BLOCK),b'abcd'*1234]
        for original in samples:
            packed=lz4.block.compress(original,store_size=False)
            out=ctypes.create_string_buffer(len(original)+32)
            before=out.raw
            src=ctypes.create_string_buffer(packed)
            self.assertEqual(self.lib.ramrx_lz4(src,len(packed),out,len(original)),0)
            self.assertEqual(out.raw[:len(original)],original)
            self.assertEqual(out.raw[len(original):],before[len(original):])
        for packed,n in [(b'\x00\x00\x00',4),(b'\x00\x01\x00',4),(b'\xf0\xff',32),
                          (b'\x10A\x01\x00',3),(b'\xf0'+b'\xff'*200,32)]:
            src=ctypes.create_string_buffer(packed);out=ctypes.create_string_buffer(64)
            self.assertEqual(self.lib.ramrx_lz4(src,len(packed),out,n),-1)
            self.assertEqual(out.raw[n:],bytes(64-n))

    def test_large_root_layout_and_adt_handoff_are_consistent(self):
        tree=mkadt.build(seed=bytes(64),ram_window='large5g')
        chosen=next(n for n in tree.children if n.name=='chosen')
        mmap=next(n for n in chosen.children if n.name=='memory-map')
        mmap.props.append(('RAMDisk',0,bytes(16)))
        blob=adt.serialize(tree)
        kernel=self.work/'kernel';path=self.work/'tree'
        kernel.write_bytes(stub.macho(bytes(0x2000)));path.write_bytes(blob)
        data,_=payload.build(kernel,path,'rd=md0')
        buf=ctypes.create_string_buffer(data);base,capacity=ctypes.c_uint64(),ctypes.c_uint64()
        self.assertIsNone(self.lib.resident_layout(buf,len(data),ctypes.byref(base),ctypes.byref(capacity)))
        self.assertGreater(base.value,0x200000000)
        self.assertGreater(capacity.value,4*1024**3)
        root_size=0x110000000
        resident=ctypes.create_string_buffer(blob)
        self.assertEqual(self.lib.resident_patch_adt(resident,len(blob),base.value,root_size),0)
        updated,_=adt.parse(resident.raw[:len(blob)])
        chosen=next(n for n in updated.children if n.name=='chosen')
        mmap=next(n for n in chosen.children if n.name=='memory-map')
        self.assertEqual(struct.unpack('<QQ',mmap.prop('RAMDisk')),(base.value,root_size))
        broken=ctypes.create_string_buffer(b'\xff'*32)
        self.assertEqual(self.lib.resident_patch_adt(broken,32,base.value,root_size),-1)

    def test_ufs_root_uses_proven_512MiB_window_without_enabling_large_usb_profile(self):
        tree=mkadt.build(seed=bytes(64),ram_window='legacy512')
        chosen=next(n for n in tree.children if n.name=='chosen')
        mmap=next(n for n in chosen.children if n.name=='memory-map')
        mmap.props.append(('RAMDisk',0,bytes(16)))
        kernel=self.work/'storage-kernel';path=self.work/'storage-tree'
        kernel.write_bytes(stub.macho(bytes(0x2000)));path.write_bytes(adt.serialize(tree))
        data,_=payload.build(kernel,path,'rd=md0')
        buf=ctypes.create_string_buffer(data);base,capacity=ctypes.c_uint64(),ctypes.c_uint64()
        self.assertIsNone(self.lib.resident_layout_ufs(buf,len(data),ctypes.byref(base),ctypes.byref(capacity)))
        self.assertGreaterEqual(base.value,0x140000000)
        self.assertGreaterEqual(capacity.value,64*1024**2)
        self.assertLessEqual(base.value+capacity.value+256*1024**2,0x160000000)
        self.assertIsNotNone(self.lib.resident_layout(buf,len(data),ctypes.byref(base),ctypes.byref(capacity)))

    @staticmethod
    def fdt(reserved_base=0x160000000,reserved_size=0x1000000):
        strings=bytearray();names={}
        def be(*v):return struct.pack('>'+'I'*len(v),*v)
        def name(s):
            if s not in names:names[s]=len(strings);strings.extend(s.encode()+b'\0')
            return names[s]
        def node(n):
            b=n.encode()+b'\0';return be(1)+b+bytes(-len(b)&3)
        def prop(n,b):return be(3,len(b),name(n))+b+bytes(-len(b)&3)
        body=node('')+prop('#address-cells',be(2))+prop('#size-cells',be(2))
        body+=node('memory@80000000')+prop('device_type',b'memory\0')+prop('reg',struct.pack('>QQ',0x80000000,0x300000000))+be(2)
        body+=node('reserved-memory')+prop('#address-cells',be(2))+prop('#size-cells',be(2))
        body+=node('reserved')+prop('reg',struct.pack('>QQ',reserved_base,reserved_size))+be(2,2,2,9)
        total=56+len(body)+len(strings)
        return be(0xd00dfeed,total,56,56+len(body),40,17,16,0,len(strings),len(body))+bytes(16)+body+strings

    def test_current_fdt_must_exclude_reserved_ram_and_bounds(self):
        fdt=self.fdt();b=ctypes.create_string_buffer(fdt)
        self.assertEqual(self.lib.fdt_ram_available(b,0x200000000,0x13e000000),1)
        self.assertEqual(self.lib.fdt_ram_available(b,0x160000000,0x10000),0)
        self.assertEqual(self.lib.fdt_ram_available(b,0x37f000000,0x2000000),0)
        changed=self.fdt(0x240000000,0x100000)
        self.assertEqual(self.lib.fdt_ram_available(ctypes.create_string_buffer(changed),0x200000000,0x13e000000),0)
        truncated=bytearray(fdt);struct.pack_into('>I',truncated,8,999999)
        self.assertEqual(self.lib.fdt_ram_available(ctypes.create_string_buffer(bytes(truncated)),0x200000000,0x100000),0)


if __name__=='__main__':unittest.main()
