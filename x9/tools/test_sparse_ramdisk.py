"""Replay sparse resident roots through the actual native payload validator."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import mkadt
from shared import adt, payload, load
from sparse_ramdisk import append_ramdisk, PAGE, TABLE

stub=load('x9_sparse_test_stub','x676c/tools/stub/mkstub.py')


class SparseRAMDiskTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='x9-sparse-root-')
        cls.work=Path(cls.tmp.name)
        lib=cls.work/'validate.dylib'
        source=Path(__file__).resolve().parents[1]/'shim/validate.c'
        subprocess.run([shutil.which('clang'),'-shared','-O2','-fPIC',str(source),'-o',str(lib)],check=True)
        cls.lib=ctypes.CDLL(str(lib))
        cls.lib.payload_validate.argtypes=[ctypes.c_void_p,ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.payload_validate.restype=ctypes.c_char_p

    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()

    def base(self):
        kernel=self.work/'kernel';tree=self.work/'tree'
        kernel.write_bytes(stub.macho(bytes(0x2000)))
        tree.write_bytes(adt.serialize(mkadt.build(seed=bytes(64),ram_window='large4g')))
        return payload.build(kernel,tree,'rd=md0 unentitled_ios_sim_launch=1')[0]

    def replay(self,base,image):
        result,info=append_ramdisk(base,image,payload.add_ramdisk_to_adt)
        buf=ctypes.create_string_buffer(result);dt,ba=ctypes.c_uint64(),ctypes.c_uint64()
        self.assertIsNone(self.lib.payload_validate(buf,len(result),ctypes.byref(dt),ctypes.byref(ba)))
        old_count=struct.unpack_from('<I',base,12)[0]
        count=struct.unpack_from('<I',result,12)[0]
        start=struct.unpack_from('<Q',base,0x30)[0]
        resident=bytearray(b'\xa5'*((len(image)+PAGE-1)&~(PAGE-1)))
        for i in range(old_count,count):
            va,size,off,stored=struct.unpack_from('<4Q',result,TABLE+i*32)
            offset=va-start
            resident[offset:offset+stored]=result[off:off+stored]
            resident[offset+stored:offset+size]=bytes(size-stored)
        self.assertEqual(bytes(resident),image+bytes(-len(image)&(PAGE-1)))
        self.assertEqual(base[0x48:TABLE],result[0x48:TABLE])
        virt=struct.unpack_from('<Q',result,0x10)[0]
        phys=struct.unpack_from('<Q',result,0x38)[0]
        off,size=struct.unpack_from('<QQ',result,0x20)
        root,_=adt.parse(result[off:off+size])
        chosen=next(n for n in root.children if n.name=='chosen')
        mmap=next(n for n in chosen.children if n.name=='memory-map')
        self.assertEqual(struct.unpack('<QQ',mmap.prop('RAMDisk')),(phys+start-virt,len(resident)))
        return result,info

    def test_nonzero_contents_and_all_zero_gaps_reconstruct_exactly(self):
        image=bytearray(8*1024*1024+71)
        image[0:16]=b'original-header!'
        image[3*1024*1024:3*1024*1024+32]=bytes(range(32))
        image[-71:]=bytes(range(71))
        result,info=self.replay(self.base(),bytes(image))
        self.assertLess(info['ramdisk_stored_bytes'],1024*1024)
        self.assertLess(len(result),len(image)//2)

    def test_all_zero_root_and_non_page_aligned_root(self):
        self.replay(self.base(),bytes(PAGE*3))
        self.replay(self.base(),b'original bytes'*43)

    def test_oversized_resident_root_and_empty_input_rejected(self):
        base=bytearray(self.base());struct.pack_into('<Q',base,0x40,0x4000)
        with self.assertRaises(ValueError):append_ramdisk(base,bytes(PAGE),payload.add_ramdisk_to_adt)
        with self.assertRaises(ValueError):append_ramdisk(self.base(),b'',payload.add_ramdisk_to_adt)


if __name__=='__main__':unittest.main()
