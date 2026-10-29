"""Bounded F2FS parser tests: synthetic metadata and captured Android blocks.
No filesystem mount or device write. These do not emulate F2FS recovery.
"""
import ctypes as C
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
CAPTURE=ROOT/'work/x9-ufs/f2fs-capture-20261026-195833'

def put(b,offset,fmt,*values):struct.pack_into('<'+fmt,b,offset,*values)
def crc(data,c=0xf2f52010):
    for byte in data:
        c^=byte
        for _ in range(8):c=(c>>1)^(0xedb88320 if c&1 else 0)
    return c

def checksum(b):put(b,4092,'I',crc(b[:4092]))

READ=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint64,C.c_void_p)
class Reader(C.Structure):_fields_=[('context',C.c_void_p),('read',READ),('blocks',C.c_uint64)]
class Info(C.Structure):
    _fields_=[('blocks',C.c_uint64),('version',C.c_uint64),('size',C.c_uint64),
              *[(x,C.c_uint32) for x in ['cp','nat','main','ssa','nat_segments','payload','features','selected_cp','cp_flags','inode','node_block','checked_blocks']],
              ('uuid',C.c_uint8*16),('inline_flags',C.c_uint8),('super_copy',C.c_uint),('pinned',C.c_uint)]

class F2FSTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='x9-f2fs-')
        p=Path(cls.tmp.name)/'f2fs.dylib'
        subprocess.run(['clang','-O2','-shared','-fPIC','-Wall','-Wextra','-Werror',str(ROOT/'x9/shim/ufs/f2fs_ro.c'),'-o',str(p)],check=True)
        cls.lib=C.CDLL(str(p));cls.lib.x9f_super.argtypes=[C.POINTER(Reader),C.POINTER(Info)]
        cls.lib.x9f_check_pin.argtypes=[C.POINTER(Reader),C.POINTER(Info),C.c_uint32,C.c_uint64,C.c_uint32]
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def setUp(self):
        self.disk={};sb=bytearray(4096);o=1024
        put(sb,o,'IHH',0xf2f52010,1,0)
        for off,val in [(8,9),(12,3),(16,12),(20,9),(60,2),(76,512),(80,1536),(84,2560),(88,3584),(92,4096),(1664,0),(2180,8)]:put(sb,o+off,'I',val)
        put(sb,o+36,'Q',8192);self.disk[0]=sb;self.disk[1]=bytearray(sb)
        for start,version in [(512,100),(1024,99)]:
            cp=bytearray(4096);put(cp,0,'Q',version)
            for off,val in [(132,1),(136,8),(140,1),(156,64),(160,64),(164,4092)]:put(cp,off,'I',val)
            checksum(cp);self.disk[start]=cp;self.disk[start+7]=bytearray(cp);self.disk[start+1]=bytearray(4096)
        nat=bytearray(4096)
        for nid,address in [(10,4500),(11,4501),(12,4502)]:put(nat,nid*9,'BII',1,10,address)
        self.disk[2560]=nat
        inode=bytearray(4096);put(inode,0,'H',0o100600);inode[3]=0x60;put(inode,12,'I',1);put(inode,16,'Q',8388608);put(inode,360,'H',32)
        for i in range(915):put(inode,392+i*4,'I',5000+i)
        put(inode,4052,'II',11,12);put(inode,4072,'III',10,10,0);self.disk[4500]=inode
        for nid,address,base,length in [(11,4501,5915,1018),(12,4502,6933,115)]:
            page=bytearray(4096)
            for i in range(length):put(page,i*4,'I',base+i)
            put(page,4072,'III',nid,10,(nid-10)<<3);self.disk[address]=page
    def parse(self,check=True,blocks=8192,first=5000):
        reads=[]
        def read(_,n,out):
            reads.append(n)
            if n not in self.disk:return -1
            C.memmove(out,bytes(self.disk[n]),4096);return 0
        callback=READ(read);reader=Reader(None,callback,blocks);info=Info()
        result=self.lib.x9f_check_pin(C.byref(reader),C.byref(info),10,8388608,first) if check else self.lib.x9f_super(C.byref(reader),C.byref(info))
        self.assertTrue(all(n<blocks for n in reads));return result,info,reads
    def test_full_8MiB_inode_and_two_direct_nodes_are_verified(self):
        result,info,_=self.parse();self.assertEqual(result,0);self.assertEqual(info.checked_blocks,2048);self.assertEqual(info.pinned,1)
    def test_latest_unclean_checkpoint_cannot_fall_back_to_old_clean_state(self):
        for n in [512,519]:put(self.disk[n],132,'I',0x46);checksum(self.disk[n])
        result,info,reads=self.parse();self.assertEqual(result,-43);self.assertEqual(info.version,100);self.assertNotIn(2560,reads)
    def test_checkpoint_crc_failure_falls_back_to_valid_pack(self):
        self.disk[512][250]^=1
        result,info,_=self.parse();self.assertEqual(result,0);self.assertEqual(info.version,99)
        self.disk[1024][250]^=1;self.assertEqual(self.parse()[0],-41)
    def test_missing_pin_changed_inode_or_changed_data_block_are_refused(self):
        page=self.disk[4500];page[3]&=~0x40;self.assertEqual(self.parse()[0],-45);page[3]|=0x40
        put(page,4076,'I',99);self.assertEqual(self.parse()[0],-44);put(page,4076,'I',10)
        put(self.disk[4502],0,'I',7000);self.assertEqual(self.parse()[0],-46)
    def test_nat_journal_overrides_stale_nat_page(self):
        self.disk[2560]=bytearray(4096)
        for n in [513,1025]:
            j=self.disk[n];put(j,3584,'H',3)
            for i,(nid,address) in enumerate([(10,4500),(11,4501),(12,4502)]):put(j,3586+13*i,'IBII',nid,1,10,address)
        result,_,reads=self.parse();self.assertEqual(result,0);self.assertNotIn(2560,reads)
        put(self.disk[513],3584,'H',39);self.assertEqual(self.parse()[0],-41)
    def test_nat_version_bitmap_selects_second_nat_copy(self):
        self.disk[3072]=self.disk.pop(2560)
        for n in [512,519,1024,1031]:self.disk[n][256]=0x80;checksum(self.disk[n])
        result,_,reads=self.parse();self.assertEqual(result,0);self.assertIn(3072,reads)
    def test_partition_bounds_and_multidevice_fail_closed(self):
        put(self.disk[0],1024+36,'Q',9000);put(self.disk[1],1024+36,'Q',9000);self.assertEqual(self.parse(False)[0],-41)
        for n in [0,1]:put(self.disk[n],1024+36,'Q',8192);self.disk[n][1024+2201+68]=ord('x')
        self.assertEqual(self.parse(False)[0],-42)
    def test_inline_xattr_reservation_changes_inode_address_count(self):
        self.disk[4500][3]|=1
        # Without flexible-xattr feature, 50 address slots are reserved.
        # The old direct-node map must now fail; rebuild it at logical 865.
        self.assertEqual(self.parse()[0],-46)
        for address,base in [(4501,5865),(4502,6883)]:
            for i in range(1018 if address==4501 else 165):put(self.disk[address],i*4,'I',base+i)
        self.assertEqual(self.parse()[0],0)
    def test_compact_summary_nat_journal_and_duplicate_keys(self):
        for n in [512,519,1024,1031]:put(self.disk[n],132,'I',5);checksum(self.disk[n])
        self.disk[2560]=bytearray(4096)
        for n in [513,1025]:
            j=self.disk[n];put(j,0,'H',3)
            for i,(nid,address) in enumerate([(10,4500),(11,4501),(12,4502)]):put(j,2+13*i,'IBII',nid,1,10,address)
        self.assertEqual(self.parse()[0],0)
        put(self.disk[513],15,'I',10);self.assertEqual(self.parse()[0],-41)
    def test_bad_direct_node_owner_and_nat_owner_cannot_verify_pin(self):
        put(self.disk[4501],4076,'I',99);self.assertEqual(self.parse()[0],-44)
        put(self.disk[4501],4076,'I',10);put(self.disk[2560],10*9+1,'I',99)
        self.assertEqual(self.parse()[0],-44)
    @unittest.skipUnless(CAPTURE.exists(),'requires captured Android metadata')
    def test_real_encrypted_and_decrypted_superblocks_and_live_checkpoint(self):
        for mode,want in [('raw',-40),('decrypted',0)]:
            data=(CAPTURE/(mode+'-superblocks.bin')).read_bytes();self.disk={i:bytearray(data[i*4096:(i+1)*4096]) for i in range(2)}
            result,info,_=self.parse(False,119101432);self.assertEqual(result,want)
        self.assertEqual(info.cp,512);self.assertEqual(info.main,295936)
        data=(CAPTURE/'decrypted-checkpoint-packs.bin').read_bytes()
        for i in range(1024):self.disk[512+i]=bytearray(data[i*4096:(i+1)*4096])
        result,info,_=self.parse(True,119101432,5143552);self.assertEqual(result,-43);self.assertEqual(info.cp_flags,0x46)

if __name__=='__main__':unittest.main()
