"""RAM-root geometry and real payload RAMDisk descriptor checks."""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from build_rootprobe import macho_contract, raw_hfs_volume
from shared import adt, payload, load
import mkadt

ROOT = Path(__file__).resolve().parents[2]
stub = load('x9_ramroot_test_stub', 'x676c/tools/stub/mkstub.py')


class RAMRootTests(unittest.TestCase):
    def test_hfs_geometry_trims_container_and_rejects_corruption(self):
        volume = bytearray(8 * 1024 * 1024)
        volume[1024:1026] = b'HX'
        struct.pack_into('>II', volume, 1064, 4096, len(volume) // 4096)
        self.assertEqual(raw_hfs_volume(volume + b'UDIF trailer'), volume)
        for block, total in [(0,2048),(3000,2048),(4096,999999)]:
            bad=bytearray(volume);struct.pack_into('>II',bad,1064,block,total)
            with self.assertRaises(ValueError): raw_hfs_volume(bad)
        with self.assertRaises(ValueError): raw_hfs_volume(b'not HFS')

    def test_macho_contract_requires_static_entry_and_signature(self):
        header=struct.pack('<8I',0xfeedfacf,0x100000c,0,2,2,304,0,0)
        thread=bytearray(288);struct.pack_into('<4I',thread,0,5,288,6,68)
        struct.pack_into('<Q',thread,16+32*8,0x100004000)
        sig=struct.pack('<4I',0x1d,16,0,0)
        macho_contract(header+thread+sig)
        bad=bytearray(header+thread+sig);struct.pack_into('<Q',bad,32+16+32*8,0x100004001)
        with self.assertRaises(ValueError): macho_contract(bad)
        dynamic=struct.pack('<8I',0xfeedfacf,0x100000c,0,2,3,312,0,0)+thread+sig+struct.pack('<2I',0xe,8)
        with self.assertRaises(ValueError): macho_contract(dynamic)

    def test_real_payload_places_ramdisk_in_managed_ram_before_adt_and_args(self):
        with tempfile.TemporaryDirectory(prefix='x9-ramroot-test-') as d:
            d=Path(d);kernel=d/'kernel';tree=d/'tree';ramdisk=d/'root'
            kernel.write_bytes(stub.macho(bytes(0x2000)))
            tree.write_bytes(adt.serialize(mkadt.build(seed=bytes(range(64)))))
            content=b'HFS-like test data'*4096;ramdisk.write_bytes(content)
            blob,info=payload.build(str(kernel),str(tree),'rd=md0',str(ramdisk))
            lib=d/'validate.dylib'
            subprocess.run(['clang','-shared','-fPIC','-O2',str(ROOT/'x9/shim/validate.c'),'-o',str(lib)],check=True)
            checker=ctypes.CDLL(str(lib));checker.payload_validate.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(ctypes.c_uint64)];checker.payload_validate.restype=ctypes.c_char_p
            buffer=ctypes.create_string_buffer(blob);adt_pa=ctypes.c_uint64();args_pa=ctypes.c_uint64()
            self.assertIsNone(checker.payload_validate(buffer,len(blob),ctypes.byref(adt_pa),ctypes.byref(args_pa)))
            adt_off,adt_size=struct.unpack_from('<QQ',blob,32)
            tree_obj,_=adt.parse(blob[adt_off:adt_off+adt_size])
            chosen=next(n for n in tree_obj.children if n.name=='chosen')
            mmap=next(n for n in chosen.children if n.name=='memory-map')
            pa,size=struct.unpack('<2Q',mmap.prop('RAMDisk'))
            self.assertEqual(pa%0x4000,0);self.assertEqual(size%0x4000,0)
            self.assertGreaterEqual(pa,0x140000000);self.assertLessEqual(pa+size,adt_pa.value)
            self.assertLess(adt_pa.value,args_pa.value)
            version,count=struct.unpack_from('<II',blob,8)
            segments=[struct.unpack_from('<4Q',blob,0x2a8+i*32) for i in range(count)]
            self.assertTrue(any(blob[s[2]:s[2]+s[3]]==content for s in segments))


if __name__ == '__main__': unittest.main()
