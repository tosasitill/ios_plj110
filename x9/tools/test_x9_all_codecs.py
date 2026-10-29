"""All decmpfs reader families; real HFS fixtures + Apple encoder interop.
No VFS, kernel DMA or hardware boot is emulated by these checks.
"""
import ctypes as C
import hashlib, importlib.util, json, random, struct, subprocess, tempfile, unittest, zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];WORK=ROOT/'work/x9-decmpfs';SRC=WORK/'kernel-source/bsd/dev/x9_decmpfs'
spec=importlib.util.spec_from_file_location('zlib_tests',ROOT/'x9/tools/test_x9_decmpfs.py');old=importlib.util.module_from_spec(spec);spec.loader.exec_module(old)
IO,READ,INFLATE=old.IO,old.READ,old.INFLATE
class Resource(C.Structure):_fields_=[('old',old.Resource),('size',C.c_uint64),('bytes',C.c_uint64),('blocks',C.c_uint32),('type',C.c_uint32)]
SOURCES=['zlib_resource.c','all_resource.c','codec_decode.c','lzbitmap.c','vendor/lzfse_decode_base.c','vendor/lzfse_fse.c','vendor/lzvn_decode_base.c']
class AllCodecTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  if not SRC.exists():raise unittest.SkipTest('requires frozen kernel reader sources')
  cls.tmp=tempfile.TemporaryDirectory();lib=Path(cls.tmp.name)/'all.dylib'
  subprocess.run(['/opt/homebrew/opt/llvm/bin/clang','-O2','-DNDEBUG','-shared','-fPIC','-Wall','-Wextra','-Wno-sign-compare','-Werror',*[str(SRC/x) for x in SOURCES],str(WORK/'reference/lzfse/src/lzvn_encode_base.c'),'-o',str(lib)],check=True)
  cls.lib=C.CDLL(str(lib));cls.lib.x9d_open.argtypes=[C.POINTER(IO),C.c_uint,C.c_uint64,C.POINTER(Resource)]
  cls.lib.x9d_read.argtypes=[C.POINTER(IO),C.POINTER(Resource),C.c_uint64,C.c_void_p,C.c_size_t,C.c_void_p]
  cls.lib.x9d_decode.argtypes=[C.POINTER(IO),C.c_uint,C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t,C.c_void_p]
  cls.compression=C.CDLL('/usr/lib/libcompression.dylib');cls.encode=cls.compression.compression_encode_buffer
  cls.encode.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t,C.c_void_p,C.c_int];cls.encode.restype=C.c_size_t
  cls.lzvn_encode=cls.lib.lzvn_encode_buffer;cls.lzvn_encode.argtypes=cls.encode.argtypes[:-1];cls.lzvn_encode.restype=C.c_size_t
  cls.lib.lzvn_encode_scratch_size.restype=C.c_size_t
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def setup_io(self,blob):
  blob=bytes(blob);self.blob=blob;self.fail=False;self.reads=[]
  def read(_,off,dst,n):
   self.reads.append((off,n))
   if self.fail or off>len(blob) or n>len(blob)-off:return -1
   C.memmove(dst,blob[off:off+n],n);return 0
  def inflate(_,data,n,dst,expected):
   try:
    op=zlib.decompressobj();decoded=op.decompress(C.string_at(data,n),expected+1)
    if not op.eof or len(decoded)!=expected:return -1
    C.memmove(dst,decoded,expected);return 0
   except zlib.error:return -1
  self.reader=READ(read);self.inflater=INFLATE(inflate);self.io=IO(None,len(blob),self.reader,self.inflater);self.scratch=C.create_string_buffer(262144)
 def encoded(self,plain,algorithm):
  out=C.create_string_buffer(len(plain)+1024);n=self.lzvn_encode(out,len(out),plain,len(plain),C.create_string_buffer(self.lib.lzvn_encode_scratch_size())) if algorithm==0x800 else self.encode(out,len(out),plain,len(plain),None,algorithm)
  self.assertGreater(n,0);return out.raw[:n]
 def decode(self,kind,encoded,size):
  self.setup_io(b'');out=C.create_string_buffer(max(size,1));e=self.lib.x9d_decode(C.byref(self.io),kind,encoded,len(encoded),out,size,self.scratch)
  return e,out.raw[:size]
 def open_resource(self,kind,blob,size):
  self.setup_io(blob);self.resource=Resource();return self.lib.x9d_open(C.byref(self.io),kind,size,C.byref(self.resource))
 def read(self,off,n):
  out=C.create_string_buffer(max(n,1));e=self.lib.x9d_read(C.byref(self.io),C.byref(self.resource),off,out,n,self.scratch)
  self.assertTrue(all(a<=len(self.blob) and b<=len(self.blob)-a for a,b in self.reads[-4:]));return e,out.raw[:n]
 def modern_resource(self,blocks):
  pos=(len(blocks)+1)*4;offsets=[pos]
  for b in blocks:pos+=len(b);offsets.append(pos)
  return struct.pack('<'+'I'*len(offsets),*offsets)+b''.join(blocks)
 def test_inline_all_codecs_and_raw_markers(self):
  plain=(b'Apple original compressed data '+bytes(range(251)))*170
  vectors=[(3,zlib.compress(plain)),(7,self.encoded(plain,0x800)),(9,b'\xcc'+plain),(11,self.encoded(plain,0x801))]
  # Valid raw-frame ZBM stream, plus decmpfs literal alternatives.
  size=len(plain);zbm=b'ZBM\x09'+(size+6).to_bytes(3,'little')+size.to_bytes(3,'little')+plain+b'\x06\0\0\0\0\0'
  vectors += [(13,zbm),(7,b'\x06'+plain),(13,b'\xff'+plain)]
  for kind,encoded in vectors:
   with self.subTest(type=kind):self.assertEqual(self.decode(kind,encoded,size),(0,plain))
  self.assertEqual(self.decode(9,b'\xcc',0),(0,b''))
 def test_apple_bitmap_encoder_partial_tail_groups(self):
  rnd=random.Random(200)
  for size in [1,7,8,9,47,100,759,2594,7000,65520]:
   for pattern in [0,1,2]:
    plain=(bytes(rnd.randrange(256) for _ in range(size)) if pattern==0 else
           (b'a'*size if pattern==1 else (bytes(range(251))*300)[:size]))
    encoded=self.encoded(plain,0x702)
    if len(encoded)>65537:
     self.assertNotEqual(self.decode(13,encoded,size)[0],0)
     encoded=b'\xff'+plain
    error,decoded=self.decode(13,encoded,size)
    self.assertEqual(error,0);self.assertEqual(decoded,plain)
 def test_legacy_zlib_dispatch_and_lzfse_raw_frame(self):
  plain=(b'original zlib reader '+bytes(range(251)))*300
  self.assertEqual(self.open_resource(4,old.make_resource(plain),len(plain)),0)
  self.assertEqual(self.read(65530,20),(0,plain[65530:65550]))
  raw=b'bvx-'+struct.pack('<I',1234)+plain[:1234]+b'bvx$'
  self.assertEqual(self.decode(11,raw,1234),(0,plain[:1234]))
 def test_resource_all_modern_families_cross_blocks(self):
  plain=(b'0123456789abcdefghijklmnopqrstuv'*5000)[:150001]
  for kind in [8,10,12,14]:
   blocks=[]
   for off in range(0,len(plain),65536):
    p=plain[off:off+65536]
    blocks.append(self.encoded(p,0x800 if kind==8 else 0x801) if kind in [8,12] else bytes([0xcc if kind==10 else 0xff])+p)
   self.assertEqual(self.open_resource(kind,self.modern_resource(blocks),len(plain)),0)
   for off,n in [(0,len(plain)),(65530,20),(131065,100)]:self.assertEqual(self.read(off,n),(0,plain[off:off+n]))
 def test_real_type9_and_type14_full_hashes_and_partial_vectors(self):
  fixtures=WORK/'all-codecs/fixtures.json'
  if not fixtures.exists():self.skipTest('requires read-only HFS fixtures')
  for item in json.loads(fixtures.read_text()):
   folder=WORK/'all-codecs/fixtures'/item['folder'];kind=item['type'];size=item['uncompressed_bytes'];whole=hashlib.sha256()
   with self.subTest(file=item['path']):
    if kind==9:
     attr=(folder/'decmpfs.xattr').read_bytes();e,decoded=self.decode(kind,attr[16:],size);self.assertEqual(e,0);whole.update(decoded)
    else:
     self.assertEqual(self.open_resource(kind,(folder/'resource-fork.bin').read_bytes(),size),0)
     for block,digest in enumerate(item['block_sha256']):
      e,decoded=self.read(block*65536,min(65536,size-block*65536));self.assertEqual(e,0,(item['folder'],block));self.assertEqual(hashlib.sha256(decoded).hexdigest(),digest);whole.update(decoded)
     for sample in item['samples']:self.assertEqual(self.read(sample['offset'],sample['bytes']),(0,(folder/sample['file']).read_bytes()))
    self.assertEqual(whole.hexdigest(),item['sha256'])
 def test_real_bitmap_payload_also_works_as_inline_type13(self):
  files=WORK/'all-codecs/fixtures.json'
  if not files.exists():self.skipTest('requires real type14 fixtures')
  for item in json.loads(files.read_text()):
   if item['type']!=14:continue
   blob=(WORK/'all-codecs/fixtures'/item['folder']/'resource-fork.bin').read_bytes();a,b=struct.unpack_from('<II',blob)
   e,decoded=self.decode(13,blob[a:b],min(65536,item['uncompressed_bytes']));self.assertEqual(e,0);self.assertEqual(hashlib.sha256(decoded).hexdigest(),item['block_sha256'][0])
 def test_short_streams_wrong_sizes_invalid_backrefs_and_headers_fail(self):
  plain=b'abcdefgh'*1000
  for kind,enc in [(7,self.encoded(plain,0x800)),(11,self.encoded(plain,0x801))]:
   self.assertNotEqual(self.decode(kind,enc[:-1],len(plain))[0],0)
   self.assertNotEqual(self.decode(kind,enc,len(plain)-1)[0],0)
  for data in [b'ZBM\x09',b'ZBM\x08'+b'\x06\0\0\0\0\0',b'ZBM\x09'+b'\x06\0\0\x01\0\0']:
   self.assertNotEqual(self.decode(13,data,10)[0],0)
  self.assertNotEqual(self.decode(9,b'\0'+plain,len(plain))[0],0)
  self.assertNotEqual(self.decode(9,b'\xcc'+plain,len(plain)-1)[0],0)
  self.assertNotEqual(self.decode(5,b'\xcc'+plain,len(plain))[0],0)
 def test_offset_tables_corruption_and_io_failure(self):
  blob=self.modern_resource([b'\xff'+b'a'*65536,b'\xff'+b'b'*20]);size=65556
  for at,value in [(0,8),(8,len(blob)-1)]:
   bad=bytearray(blob);struct.pack_into('<I',bad,at,value);self.assertNotEqual(self.open_resource(14,bad,size),0)
  bad=bytearray(blob);struct.pack_into('<I',bad,4,4);self.assertEqual(self.open_resource(14,bad,size),0);self.assertNotEqual(self.read(0,1)[0],0)
  self.assertEqual(self.open_resource(14,blob,size),0);self.fail=True;self.assertNotEqual(self.read(0,1)[0],0)
  self.fail=False;self.assertNotEqual(self.read(size,1)[0],0)
if __name__=='__main__':unittest.main()
