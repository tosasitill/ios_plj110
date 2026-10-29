"""Read-only UFS transport with modeled completions and real captured GPTs.
These tests do not qualify MT6993 MMIO, retained link, DMA or SMMU hardware.
"""
import ctypes as C
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
import time

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
CAPTURE=Path('/Users/tosasitill/Downloads/01-Android/内测/x9内测/final/x9/gpt/sdc-head.img')

class Partition(C.Structure):
    _fields_=[('first',C.c_uint64),('count',C.c_uint64),('guid',C.c_uint8*16)]
class Extent(C.Structure):
    _fields_=[('file',C.c_uint64),('part',C.c_uint64),('count',C.c_uint64)]
class RootManifest(C.Structure):
    _fields_=[('bytes',C.c_uint64),('first',C.c_uint64),('blocks',C.c_uint64),('lun',C.c_uint),
              ('guid',C.c_uint8*16),('sha',C.c_uint8*32),('extents',C.POINTER(Extent)),('count',C.c_size_t)]
class SMMU(C.Structure):
    _fields_=[('idr0',C.c_uint32),('cr0',C.c_uint32),('cr0ack',C.c_uint32),('gbpa',C.c_uint32),
              ('config',C.c_uint32),('base',C.c_uint64),('l1',C.c_uint64),('ste0',C.c_uint64),
              ('sid',C.c_uint),('identity',C.c_uint)]

class UFSTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='x9-ufs-tests-');cls.work=Path(cls.tmp.name)
        lib=cls.work/'ufs.dylib';src=ROOT/'x9/shim/ufs'
        subprocess.run([shutil.which('clang'),'-O2','-shared','-fPIC','-Wall','-Wextra','-Werror',
                        *getattr(cls,'extra_cflags',[]),'-I',str(src),str(HERE/'ufs_harness.c'),
                        *[str(src/n) for n in ['ufs_ro.c','ufs_volume.c','ufs_smmu.c','ufs_root.c','sha256.c']],'-o',str(lib)],check=True)
        cls.lib=C.CDLL(str(lib))
        cls.lib.test_reg.argtypes=[C.c_uint,C.c_uint32];cls.lib.test_get.argtypes=[C.c_uint];cls.lib.test_get.restype=C.c_uint32
        cls.lib.test_read.argtypes=[C.c_uint,C.c_uint64,C.c_uint32,C.c_void_p]
        cls.lib.test_disk.argtypes=[C.c_uint,C.c_void_p,C.c_size_t,C.c_uint64]
        cls.lib.test_gpt.argtypes=[C.c_uint,C.c_char_p,C.POINTER(Partition)]
        cls.lib.test_image.argtypes=[C.c_uint,C.POINTER(Partition),C.POINTER(Extent),C.c_size_t,C.c_uint64,C.c_uint64,C.c_void_p,C.c_size_t]
        cls.lib.test_image_mapped.argtypes=cls.lib.test_image.argtypes
        cls.lib.test_sync_bytes.restype=C.c_uint64
        cls.lib.x9u_extent_validate.argtypes=[C.POINTER(Extent),C.c_size_t,C.c_uint64,C.c_uint64]
        cls.lib.test_root.argtypes=[C.c_uint,C.POINTER(Partition),C.POINTER(RootManifest),C.c_void_p,C.c_size_t]
        cls.lib.test_sha256.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p]
        cls.R=C.CFUNCTYPE(C.c_uint32,C.c_void_p,C.c_uint)
        cls.T=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint64,C.POINTER(C.c_uint64))
        cls.lib.x9u_smmu_identity.argtypes=[C.c_void_p,C.c_uint,C.POINTER(SMMU),cls.R,cls.T]

    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def setUp(self):self.lib.test_reset()
    def put_disk(self,data,blocks=None,lun=0):
        b=C.create_string_buffer(bytes(data));self.lib.test_disk(lun,b,len(data),blocks or len(data)//4096)
    def opened(self):self.assertEqual(self.lib.test_open(1),0)

    def test_no_hci_writes_without_proven_dma_or_ready_idle_legacy_host(self):
        for register,value,want in [(None,0,-4),(0x58,1,-3),(0x78,1,-3),(0x34,0,-2),(0x30,1,-2),(8,0,-2)]:
            self.lib.test_reset()
            if register is not None:self.lib.test_reg(register,value)
            self.assertEqual(self.lib.test_open(0 if register is None else 1),want)
            self.assertEqual(self.lib.test_writes(),0)
        self.lib.test_reset();self.lib.test_reg(8,0x400);self.lib.test_reg(0x300,1)
        self.assertEqual(self.lib.test_open(1),-5);self.assertEqual(self.lib.test_writes(),0)

    def test_capacity_and_read_only_cdb_complete_and_queue_is_restored(self):
        disk=bytearray(0x200000);disk[7*4096:8*4096]=bytes(range(256))*16
        self.put_disk(disk);self.opened();self.assertEqual(self.lib.test_capacity(0),0)
        out=C.create_string_buffer(4096);self.assertEqual(self.lib.test_read(0,7,1,out),0)
        self.assertEqual(out.raw,disk[7*4096:8*4096]);self.assertEqual(self.lib.test_cdb(),0x28)
        self.assertEqual(self.lib.test_forbidden(),0);self.assertEqual(self.lib.test_close(),0)
        self.assertEqual(self.lib.test_get(0x50),0x90000000);self.assertEqual(self.lib.test_get(0x24),0x777)
        self.assertEqual(self.lib.test_get(0x60),1)

    def test_malformed_or_failed_responses_never_copy_output(self):
        self.put_disk(bytes(0x200000));self.opened();self.assertEqual(self.lib.test_capacity(0),0)
        for mode in [1,2,3,4,29]:
            self.lib.test_mode(mode);out=C.create_string_buffer(b'X'*4096,4096)
            self.assertEqual(self.lib.test_read(0,0,1,out),-7)
            self.assertEqual(out.raw,b'X'*4096)
        self.assertEqual(self.lib.test_forbidden(),0)

    def test_timeout_poisons_host_and_pending_dma_blocks_handoff(self):
        self.opened();self.lib.test_mode(6)
        self.assertEqual(self.lib.test_capacity(0),-11)
        writes=self.lib.test_writes();self.assertEqual(self.lib.test_close(),-11)
        self.assertEqual(self.lib.test_writes(),writes)
        self.assertEqual(self.lib.test_capacity(0),-1)
        self.lib.test_reg(0x58,0);self.assertEqual(self.lib.test_close(),0)

    @unittest.skipUnless(CAPTURE.exists(),'requires user-captured GPT')
    def test_real_x9_gpt_is_read_through_actual_ufs_descriptors(self):
        data=CAPTURE.read_bytes();last=struct.unpack_from('<Q',data,4096+32)[0]
        self.put_disk(data,last+1);self.opened();self.assertEqual(self.lib.test_capacity(0),0)
        result=Partition();self.assertEqual(self.lib.test_gpt(0,b'userdata',C.byref(result)),0)
        header=data[4096:8192];table,count,size=struct.unpack_from('<QII',header,72)
        entries=[data[table*4096+i*size:table*4096+(i+1)*size] for i in range(count)]
        entry=next(e for e in entries if e[56:128].decode('utf-16le').rstrip('\0')=='userdata')
        first,last=struct.unpack_from('<QQ',entry,32)
        self.assertEqual((result.first,result.count),(first,last-first+1));self.assertEqual(bytes(result.guid),entry[16:32])
        self.assertEqual(self.lib.test_forbidden(),0)

    @unittest.skipUnless(CAPTURE.exists(),'requires user-captured GPT')
    def test_corrupt_gpt_crc_is_refused_before_exposing_partition(self):
        data=bytearray(CAPTURE.read_bytes());last=struct.unpack_from('<Q',data,4128)[0]
        data[4096+60]^=1;self.put_disk(data,last+1);self.opened();self.assertEqual(self.lib.test_capacity(0),0)
        p=Partition();self.assertEqual(self.lib.test_gpt(0,b'userdata',C.byref(p)),-9)
        data=bytearray(CAPTURE.read_bytes());data[8192+100]^=1
        self.put_disk(data,last+1);self.assertEqual(self.lib.test_gpt(0,b'userdata',C.byref(p)),-9)

    def test_extent_image_reads_cross_nonadjacent_runs_with_bounds(self):
        disk=bytearray(0x200000)
        for block in range(512):disk[block*4096:(block+1)*4096]=bytes([block%251])*4096
        self.put_disk(disk);self.opened();self.assertEqual(self.lib.test_capacity(0),0)
        p=Partition(10,200);m=(Extent*2)(Extent(0,30,2),Extent(2,80,3))
        expected=disk[40*4096:42*4096]+disk[90*4096:93*4096]
        out=C.create_string_buffer(14000)
        self.assertEqual(self.lib.test_image(0,C.byref(p),m,2,5,3500,out,14000),0)
        self.assertEqual(out.raw,expected[3500:17500])
        commands=self.lib.test_commands();self.assertEqual(self.lib.test_image(0,C.byref(p),m,2,5,20480,out,1),-12)
        self.assertEqual(self.lib.test_commands(),commands)
        overlap=(Extent*2)(Extent(0,30,2),Extent(2,31,3))
        self.assertEqual(self.lib.x9u_extent_validate(overlap,2,5,200),-12)
        hole=(Extent*2)(Extent(0,30,2),Extent(3,80,3))
        self.assertEqual(self.lib.x9u_extent_validate(hole,2,6,200),-12)

    def test_partial_dma_sync_delivers_controller_output_without_syncing_unused_tail(self):
        disk=bytes(range(256))*8192;self.put_disk(disk);self.opened()
        self.assertEqual(self.lib.test_capacity(0),0)
        for count in [1,2,4,16]:
            before=self.lib.test_sync_bytes();out=C.create_string_buffer(4096*count)
            self.assertEqual(self.lib.test_read(0,7,count,out),0)
            self.assertEqual(out.raw,disk[7*4096:(7+count)*4096])
            self.assertEqual(self.lib.test_sync_bytes()-before,4096*count*2+1616)

    def test_validated_map_fast_path_matches_cross_extent_reads_and_refuses_ranges(self):
        disk=bytes(range(251))*8355;self.put_disk(disk[:0x200000]);self.opened()
        self.assertEqual(self.lib.test_capacity(0),0)
        p=Partition(10,200);m=(Extent*2)(Extent(0,30,2),Extent(2,80,3))
        self.assertEqual(self.lib.x9u_extent_validate(m,2,5,200),0)
        for offset,size in [(0,1),(4095,17),(7500,9000),(18000,2480)]:
            reference=C.create_string_buffer(size);fast=C.create_string_buffer(size)
            self.assertEqual(self.lib.test_image(0,C.byref(p),m,2,5,offset,reference,size),0)
            self.assertEqual(self.lib.test_image_mapped(0,C.byref(p),m,2,5,offset,fast,size),0)
            self.assertEqual(reference.raw,fast.raw)
        commands=self.lib.test_commands()
        self.assertEqual(self.lib.test_image_mapped(0,C.byref(p),m,2,5,20480,fast,1),-12)
        invalid=(Extent*1)(Extent(0,199,5))
        self.assertEqual(self.lib.test_image_mapped(0,C.byref(p),invalid,1,5,0,fast,1),-12)
        self.assertEqual(self.lib.test_commands(),commands)

    def test_nop_and_deviceinit_set_then_read_flag_are_only_management_commands(self):
        self.opened();self.assertEqual(self.lib.test_device_init(),0)
        self.assertEqual(self.lib.test_init_stage(),4);self.assertEqual(self.lib.test_init_reads(),3)
        self.assertEqual(self.lib.test_commands(),5);self.assertEqual(self.lib.test_forbidden(),0)
        self.assertEqual(self.lib.test_close(),0)
    def test_failed_nop_set_read_and_stuck_deviceinit_fail_at_exact_stage(self):
        for mode,want,stage in [(7,-7,1),(10,-7,2),(8,-7,2),(9,-6,3)]:
            self.lib.test_reset();self.opened();self.lib.test_mode(mode)
            self.assertEqual(self.lib.test_device_init(),want);self.assertEqual(self.lib.test_init_stage(),stage)
            self.assertEqual(self.lib.test_forbidden(),0);self.assertEqual(self.lib.test_close(),0)

    def test_lun_unit_attention_is_cleared_but_persistent_not_ready_is_bounded(self):
        for mode,want in [(0,0),(11,0),(12,-2),(13,-7)]:
            self.lib.test_reset();self.opened();self.lib.test_mode(mode)
            self.assertEqual(self.lib.test_ready(2),want);self.assertEqual(self.lib.test_forbidden(),0)
            self.assertLessEqual(self.lib.test_commands(),6);self.assertEqual(self.lib.test_close(),0)

    def test_upiu_autosense_fixed_or_descriptor_unit_attention_needs_no_request_sense(self):
        for mode in [14,15]:
            self.lib.test_reset();self.opened();self.lib.test_mode(mode)
            self.assertEqual(self.lib.test_ready(2),0)
            self.assertEqual(self.lib.test_commands(),2)
            self.assertEqual(self.lib.test_sense_calls(),0);self.assertEqual(self.lib.test_forbidden(),0)
    def test_fallback_no_sense_and_short_descriptor_still_require_subsequent_good_tur(self):
        for mode in [17,18]:
            self.lib.test_reset();self.opened();self.lib.test_mode(mode)
            self.assertEqual(self.lib.test_ready(2),0)
            self.assertEqual(self.lib.test_commands(),3);self.assertEqual(self.lib.test_sense_calls(),1)
            self.assertEqual(self.lib.test_forbidden(),0)
    def test_malformed_sense_and_transport_errors_never_trigger_request_sense(self):
        for mode,phase in [(16,3),(20,2),(21,3)]:
            self.lib.test_reset();self.opened();self.lib.test_mode(mode)
            self.assertEqual(self.lib.test_ready(2),-7)
            self.assertEqual(self.lib.test_failure_phase(),phase);self.assertEqual(self.lib.test_sense_calls(),0)
            self.assertEqual(self.lib.test_commands(),1)
    def test_persistent_not_ready_or_illegal_request_preserves_autosense_and_stops(self):
        for mode,want,commands,category in [(19,-2,3,2),(22,-7,1,5)]:
            self.lib.test_reset();self.opened();self.lib.test_mode(mode)
            self.assertEqual(self.lib.test_ready(2),want)
            self.assertEqual(self.lib.test_commands(),commands);self.assertEqual(self.lib.test_sense_calls(),0)
            self.assertEqual(self.lib.test_sense_valid(),1);self.assertEqual(self.lib.test_sense_category(),category)
    def test_busy_tur_has_bounded_retry_without_request_sense(self):
        self.opened();self.lib.test_mode(23);self.assertEqual(self.lib.test_ready(2),0)
        self.assertEqual(self.lib.test_sense_calls(),0);self.assertEqual(self.lib.test_commands(),2)

    def test_observed_target_failure_plus_check_condition_decodes_before_retry(self):
        self.opened();self.lib.test_mode(24)
        self.assertEqual(self.lib.test_ready(2),0)
        self.assertEqual(self.lib.test_commands(),2)
        self.assertEqual(self.lib.test_unit_attentions(),1)
        self.assertEqual(self.lib.test_sense_calls(),0);self.assertEqual(self.lib.test_forbidden(),0)
    def test_reserved_response_or_target_failure_with_good_status_is_not_accepted(self):
        for mode in [25,26]:
            self.lib.test_reset();self.opened();self.lib.test_mode(mode)
            self.assertEqual(self.lib.test_ready(2),-7)
            self.assertEqual(self.lib.test_failure_phase(),3);self.assertEqual(self.lib.test_commands(),1)
            self.assertEqual(self.lib.test_sense_calls(),0)
    def test_target_failure_check_condition_does_not_bypass_persistent_or_fatal_sense(self):
        for mode,want,commands in [(27,-2,3),(28,-7,1)]:
            self.lib.test_reset();self.opened();self.lib.test_mode(mode)
            self.assertEqual(self.lib.test_ready(2),want)
            self.assertEqual(self.lib.test_failure_phase(),4);self.assertEqual(self.lib.test_commands(),commands)
            self.assertEqual(self.lib.test_sense_calls(),0)

    def test_plaintext_root_digest_and_hfs_geometry_across_extents(self):
        root=bytearray((i*31)%256 for i in range(65536));root[1024:1028]=b'HX\0\5'
        struct.pack_into('>II',root,1064,4096,16)
        disk=bytearray(0x200000);disk[40*4096:48*4096]=root[:32768];disk[90*4096:98*4096]=root[32768:]
        self.put_disk(disk);self.opened();self.assertEqual(self.lib.test_capacity(0),0)
        part=Partition(10,200);runs=(Extent*2)(Extent(0,30,8),Extent(8,80,8))
        m=RootManifest();m.bytes=65536;m.first=10;m.blocks=200;m.lun=0;m.extents=runs;m.count=2
        m.sha[:]=hashlib.sha256(root).digest();out=C.create_string_buffer(65536)
        self.assertEqual(self.lib.test_root(0,C.byref(part),C.byref(m),out,65536),0)
        self.assertEqual(out.raw,root);self.assertEqual(self.lib.test_forbidden(),0)
        m.sha[0]^=1;self.assertEqual(self.lib.test_root(0,C.byref(part),C.byref(m),out,65536),-9)
        m.sha[0]^=1;m.guid[0]=1;commands=self.lib.test_commands()
        self.assertEqual(self.lib.test_root(0,C.byref(part),C.byref(m),out,65536),-8)
        self.assertEqual(self.lib.test_commands(),commands);m.guid[0]=0
        self.assertEqual(self.lib.test_root(0,C.byref(part),C.byref(m),out,65535),-12)
        struct.pack_into('>I',root,1068,15);disk[40*4096:48*4096]=root[:32768];self.put_disk(disk)
        m.sha[:]=hashlib.sha256(root).digest()
        self.assertEqual(self.lib.test_root(0,C.byref(part),C.byref(m),out,65536),-8)

    def test_sha256_matches_hashlib_for_padding_boundaries(self):
        for size in [0,1,55,56,63,64,65,4096,65536]:
            data=bytes(i%256 for i in range(size));out=C.create_string_buffer(32)
            self.lib.test_sha256(data,len(data),out);self.assertEqual(out.raw,hashlib.sha256(data).digest())

    def smmu(self,registers,tables):
        accesses=[]
        def read(_,offset):return registers.get(offset,0)
        def table(_,address,out):
            accesses.append(address)
            if address not in tables:return -1
            out[0]=tables[address];return 0
        result=SMMU();error=self.lib.x9u_smmu_identity(None,0x63,C.byref(result),self.R(read),self.T(table))
        return error,result,accesses

    def test_smmu_translation_abort_or_inaccessible_tables_never_allow_identity_dma(self):
        for gbpa,want in [(0,0),(1<<20,-3),(1<<31,-2)]:
            error,state,accesses=self.smmu({0:1,0x44:gbpa},{})
            self.assertEqual(error,want);self.assertEqual(state.identity,int(want==0));self.assertEqual(accesses,[])
        registers={0:1,0x20:1,0x24:1,0x80:0x100000,0x88:8}
        ste=0x100000+0x63*64
        for value,want in [(9,0),(1,-13),(0,-13),(11,-13)]:
            error,state,_=self.smmu(registers,{ste:value})
            self.assertEqual(error,want);self.assertEqual(state.identity,int(want==0))
        self.assertNotEqual(self.smmu(registers,{})[0],0)
        two={0:1,0x20:1,0x24:1,0x80:0x100000,0x88:(1<<16)|(8<<6)|16}
        self.assertEqual(self.smmu(two,{0x100000:0x200000|9,0x200000+0x63*64:9})[0],0)
        self.assertNotEqual(self.smmu(two,{0x100000:0x200000})[0],0)

if __name__=='__main__':unittest.main()
