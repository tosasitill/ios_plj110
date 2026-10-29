"""Real IPSW launchd/dyld/xpcproxy resource forks + hostile decmpfs corpus.
Calls the same bounded C parser linked into XNU. zlib callback checks stream
completion and exact output independently via host zlib; bounded Apple
resource-entry tails are intentionally ignored after a valid stream end.
"""
import ctypes as C
from pathlib import Path
import hashlib
import random
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'work/x9-decmpfs'
READ=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint64,C.c_void_p,C.c_size_t)
INFLATE=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t)
class IO(C.Structure):_fields_=[('context',C.c_void_p),('size',C.c_uint64),('read',READ),('inflate',INFLATE)]
class Resource(C.Structure):_fields_=[('base',C.c_uint64),('length',C.c_uint64),('size',C.c_uint64),('blocks',C.c_uint32)]


def make_resource(plain,raw_blocks=False):
    chunks=[plain[i:i+65536] for i in range(0,len(plain),65536)]
    encoded=[b'\xff'+p if raw_blocks else zlib.compress(p) for p in chunks]
    table=bytearray(struct.pack('<I',len(chunks)));offset=4+len(chunks)*8
    for block in encoded:table+=struct.pack('<II',offset,len(block));offset+=len(block)
    record=bytes(table)+b''.join(encoded);data=struct.pack('>I',len(record))+record
    header=struct.pack('>IIII',256,256+len(data),len(data),0)
    return header+bytes(256-16)+data

class DecmpfsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='x9-decmpfs-tests-');lib=Path(cls.tmp.name)/'decoder.dylib'
        subprocess.run(['clang','-O2','-shared','-fPIC','-Wall','-Wextra','-Werror',str(WORK/'kernel-source/bsd/dev/x9_decmpfs/zlib_resource.c'),'-o',str(lib)],check=True)
        cls.lib=C.CDLL(str(lib));cls.lib.x9dc_open.argtypes=[C.POINTER(IO),C.c_uint64,C.POINTER(Resource)]
        cls.lib.x9dc_read.argtypes=[C.POINTER(IO),C.POINTER(Resource),C.c_uint64,C.c_void_p,C.c_size_t,C.c_void_p]
        cls.lib.x9dc_decode.argtypes=[C.POINTER(IO),C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t]
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def setup_io(self,blob):
        self.blob=bytes(blob);self.reads=[];self.decodes=0
        def read(_,offset,out,n):
            self.reads.append((offset,n))
            if offset>len(self.blob) or n>len(self.blob)-offset:return -1
            C.memmove(out,self.blob[offset:offset+n],n);return 0
        def inflate(_,data,n,out,expected):
            self.decodes+=1
            try:
                op=zlib.decompressobj();plain=op.decompress(C.string_at(data,n),expected+1)
                if not op.eof or op.unconsumed_tail or len(plain)!=expected:return -1
                C.memmove(out,plain,expected);return 0
            except zlib.error:return -1
        self.reader=READ(read);self.inflater=INFLATE(inflate);self.io=IO(None,len(self.blob),self.reader,self.inflater)
        return self.io
    def opened(self,blob,size):
        self.setup_io(blob);self.resource=Resource()
        self.assertEqual(self.lib.x9dc_open(C.byref(self.io),size,C.byref(self.resource)),0)
    def read(self,offset,length):
        out=C.create_string_buffer(max(1,length));scratch=C.create_string_buffer(131073)
        result=self.lib.x9dc_read(C.byref(self.io),C.byref(self.resource),offset,out,length,scratch)
        self.assertTrue(all(a<=len(self.blob) and n<=len(self.blob)-a for a,n in self.reads))
        return result,out.raw[:length]
    def test_real_ios_executables_full_bytes_and_random_page_ranges(self):
        randomizer=random.Random(197)
        for name in ['launchd','dyld','xpcproxy']:
            folder=WORK/'fixtures'/name
            if not folder.exists():self.skipTest('requires captured IPSW compressed fixtures')
            plain=(folder/'original.bin').read_bytes();attr=(folder/'decmpfs.xattr').read_bytes()
            self.assertEqual(struct.unpack_from('<IIQ',attr),(0x636d7066,4,len(plain)))
            self.opened((folder/'resource-fork.bin').read_bytes(),len(plain))
            error,decoded=self.read(0,len(plain));self.assertEqual(error,0);self.assertEqual(decoded,plain)
            self.assertEqual(hashlib.sha256(decoded).digest(),hashlib.sha256(plain).digest())
            for _ in range(20):
                offset=randomizer.randrange(len(plain));length=min(randomizer.randrange(1,131073),len(plain)-offset)
                error,part=self.read(offset,length);self.assertEqual(error,0);self.assertEqual(part,plain[offset:offset+length])
    def test_raw_blocks_final_partial_block_and_boundary_crossings(self):
        plain=bytes(range(256))*600+bytes(93)
        self.opened(make_resource(plain,True),len(plain))
        for offset,length in [(0,len(plain)),(65535,3),(65536,16384),(len(plain)-1,1),(len(plain),0)]:
            result,data=self.read(offset,length);self.assertEqual(result,0);self.assertEqual(data,plain[offset:offset+length])
        self.assertEqual(self.decodes,0)
    def test_bad_envelope_count_and_table_cannot_escape_resource(self):
        plain=b'hello'*20000;original=make_resource(plain)
        for offset,value,fmt in [(0,0,'>I'),(4,1,'>I'),(8,0xffffffff,'>I'),(256,2,'>I'),(260,99999,'<I')]:
            bad=bytearray(original);struct.pack_into(fmt,bad,offset,value);self.setup_io(bad);resource=Resource()
            self.assertNotEqual(self.lib.x9dc_open(C.byref(self.io),len(plain),C.byref(resource)),0)
    def test_block_table_pointer_length_and_overlaps_are_refused(self):
        plain=b'hello'*20000;original=make_resource(plain)
        for offset,value in [(264,0),(264,0xffffffff),(268,0),(268,65538),(272,4)]:
            bad=bytearray(original);struct.pack_into('<I',bad,offset,value)
            self.opened(bad,len(plain));self.assertNotEqual(self.read(0,4096)[0],0)
    def test_checksum_corruption_and_wrong_uncompressed_size_are_refused(self):
        plain=b'hello'*20000;bad=bytearray(make_resource(plain));bad[-1]^=1
        self.opened(bad,len(plain));self.assertNotEqual(self.read(65536,len(plain)-65536)[0],0)
        self.opened(make_resource(plain),len(plain)+1);self.assertNotEqual(self.read(65536,len(plain)+1-65536)[0],0)
    def test_inline_type3_stream_raw_marker_and_trailing_data_rules(self):
        self.setup_io(b'');plain=b'inline type3 payload'*200
        for encoded,want in [(zlib.compress(plain),0),(b'\xff'+plain,0),(zlib.compress(plain)+b'garbage',0),(b'\xff'+plain+b'x',-1)]:
            out=C.create_string_buffer(len(plain));result=self.lib.x9dc_decode(C.byref(self.io),encoded,len(encoded),out,len(plain))
            self.assertEqual(result,want)
            if not want:self.assertEqual(out.raw,plain)
    def test_out_of_range_reads_issue_no_callback(self):
        self.opened(make_resource(b'a'*65536),65536);n=len(self.reads)
        self.assertEqual(self.read(65536,1)[0],-4);self.assertEqual(self.read(2**64-1,1)[0],-4)
        self.assertEqual(len(self.reads),n)
    def test_read_failure_is_propagated_without_decode(self):
        self.opened(make_resource(b'a'*65536),65536)
        fail=READ(lambda *args:-1);self.io.read=fail
        self.assertEqual(self.read(0,4096)[0],-2);self.assertEqual(self.decodes,0)

if __name__=='__main__':unittest.main()
