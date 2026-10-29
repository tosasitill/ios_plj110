"""AES-XTS vs independent OpenSSL-backed cryptography; no device keys.
Synthetic effective keys test algorithms, NOT actual X9 decryption.
"""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest
from cryptography.hazmat.primitives.ciphers import Cipher,algorithms,modes

ROOT=Path(__file__).resolve().parents[2]
KEY=bytes(range(64))
OTHER=bytes(range(64,128))

def encrypted(key,dun,plain):
    op=Cipher(algorithms.AES(key),modes.XTS(dun.to_bytes(16,'little'))).encryptor()
    return op.update(plain)+op.finalize()

class DecryptionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='x9-xts-tests-');lib=Path(cls.tmp.name)/'xts.dylib';src=ROOT/'x9/shim/ufs'
        subprocess.run(['clang','-O2','-shared','-fPIC','-Wall','-Wextra','-Werror',str(src/'ufs_decrypt.c'),str(src/'crypto/aes.c'),'-o',str(lib)],check=True)
        cls.lib=C.CDLL(str(lib))
        cls.lib.x9d_xts_init.argtypes=[C.c_void_p,C.c_void_p,C.c_size_t]
        cls.lib.x9d_xts_decrypt.argtypes=[C.c_void_p,C.c_uint64,C.c_void_p,C.c_size_t]
        cls.lib.x9d_init.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.c_uint64]
        cls.lib.x9d_metadata.argtypes=[C.c_void_p,C.c_uint64,C.c_void_p]
        cls.lib.x9d_file.argtypes=[C.c_void_p,C.c_uint32,C.c_uint64,C.c_void_p]
        cls.lib.x9d_clear.argtypes=[C.c_void_p]
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def test_xts_independent_vectors_and_large_duns(self):
        ctx=C.create_string_buffer(2048);self.assertEqual(self.lib.x9d_xts_init(ctx,KEY,64),0)
        plain=bytes(range(256))*16
        for dun in [0,1,5143552,0x123456789abcdef0]:
            cipher=encrypted(KEY,dun,plain);b=C.create_string_buffer(cipher,4096)
            self.assertEqual(self.lib.x9d_xts_decrypt(ctx,dun,b,4096),0);self.assertEqual(b.raw,plain)
    def test_metadata_iv_is_partition_relative_and_crypto_sector_scaled(self):
        ctx=C.create_string_buffer(2048);self.assertEqual(self.lib.x9d_init(ctx,KEY,OTHER,16),0)
        plain=b'F2FS test metadata'+bytes(4096-18)
        block=295936;b=C.create_string_buffer(encrypted(KEY,block+2,plain),4096)
        self.assertEqual(self.lib.x9d_metadata(ctx,block,b),0);self.assertEqual(b.raw,plain)
    def test_complete_8MiB_file_uses_inode_logical_iv_and_no_metadata_layer(self):
        ctx=C.create_string_buffer(2048);self.assertEqual(self.lib.x9d_init(ctx,KEY,OTHER,0),0)
        zero=bytes(4096)
        for block in range(2048):
            b=C.create_string_buffer(encrypted(OTHER,(659739<<32)|block,zero),4096)
            self.assertEqual(self.lib.x9d_file(ctx,659739,block,b),0);self.assertEqual(b.raw,zero)
    def test_missing_keys_do_not_transform_ciphertext(self):
        ctx=C.create_string_buffer(2048);self.assertEqual(self.lib.x9d_init(ctx,None,None,0),-60)
        b=C.create_string_buffer(bytes([91])*4096,4096)
        self.assertEqual(self.lib.x9d_metadata(ctx,0,b),-60);self.assertEqual(self.lib.x9d_file(ctx,659739,0,b),-60)
        self.assertEqual(b.raw,bytes([91])*4096)
    def test_invalid_key_iv_and_sizes_fail_without_claiming_plaintext(self):
        ctx=C.create_string_buffer(2048);self.assertEqual(self.lib.x9d_xts_init(ctx,bytes(64),64),-61)
        self.assertEqual(self.lib.x9d_xts_init(ctx,KEY,32),-61)
        self.assertEqual(self.lib.x9d_init(ctx,KEY,OTHER,1),-61)
        self.assertEqual(self.lib.x9d_init(ctx,KEY,OTHER,0),0)
        b=C.create_string_buffer(bytes(4096),4096)
        self.assertEqual(self.lib.x9d_xts_decrypt(ctx,0,b,4095),-61)
        self.assertEqual(self.lib.x9d_file(ctx,659739,1<<32,b),-61)
        self.lib.x9d_clear(ctx);self.assertEqual(self.lib.x9d_file(ctx,659739,0,b),-60)
    def test_wrong_effective_key_or_iv_cannot_match_expected_file(self):
        ctx=C.create_string_buffer(2048);self.assertEqual(self.lib.x9d_init(ctx,KEY,OTHER,0),0)
        for key,dun in [(KEY,659739<<32),(OTHER,(659739<<32)|1)]:
            b=C.create_string_buffer(encrypted(key,dun,bytes(4096)),4096)
            self.assertEqual(self.lib.x9d_file(ctx,659739,0,b),0);self.assertNotEqual(b.raw,bytes(4096))

if __name__=='__main__':unittest.main()
