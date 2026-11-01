"""Native-root ADT contract and bounds, independent of device writes."""
import copy
import struct
import unittest
from ufs_root_manifest import validate,header,add_native_adt
from shared import adt
import mkadt

class NativeManifestTests(unittest.TestCase):
    def manifest(self):
        return {'schema':'x9-plaintext-root-v1','lun':2,'bytes':16*1024**3,
                'partition_first':5783552,'partition_blocks':119101432,
                'partition_guid_hex':'63000000c23988449bb000cb43c9ccd4','sha256':'12'*32,
                'header_bytes':65536,'header_sha256':'34'*32,'volume_uuid_hex':'56'*8,
                'extents':[{'file_block':0,'partition_block':1000000,'blocks':4*1024**2}]}
    def test_full_image_allowed_only_for_native_not_old_ram_path(self):
        m=self.manifest();self.assertEqual(validate(m,True),m)
        with self.assertRaises(ValueError):validate(m)
        self.assertIn('X9_ROOT_BYTES 17179869184UL',header(m,True))
    def test_native_adt_roundtrip_has_extent_and_identity_without_ramdisk(self):
        m=self.manifest();tree=add_native_adt(mkadt.build(seed=bytes(64)),m)
        obj,_=adt.parse(adt.serialize(tree));chosen=next(n for n in obj.children if n.name=='chosen')
        self.assertEqual(struct.unpack('<Q',chosen.prop('x9-ufs-root-bytes')),(16*1024**3,))
        self.assertEqual(chosen.prop('x9-ufs-root-header-sha256'),bytes.fromhex(m['header_sha256']))
        self.assertEqual(struct.unpack('<3Q',chosen.prop('x9-ufs-root-extents')),(0,1000000,4*1024**2))
        mmap=next(n for n in chosen.children if n.name=='memory-map')
        self.assertFalse(any(name=='RAMDisk' for name,_,_ in mmap.props))
    def test_native_root_needs_header_identity_and_bounded_disjoint_extents(self):
        for key,value in [('header_bytes',4096),('header_sha256','12'),('volume_uuid_hex',''),('bytes',65*1024**3)]:
            m=self.manifest();m[key]=value
            with self.assertRaises(ValueError):validate(m,True)
        m=self.manifest();m['extents']=[{'file_block':0,'partition_block':1000,'blocks':2097152},{'file_block':2097152,'partition_block':2000,'blocks':2097152}]
        with self.assertRaises(ValueError):validate(m,True)

    def test_writable_copy_contract_is_explicit_and_protected_extents_never_overlap(self):
        m=self.manifest();m['write_policy']={'version':1,'kind':'dedicated-mutable-copy','initial_content_verified':True,
            'protected_extents':[{'partition_block':100,'blocks':200}]}
        self.assertEqual(validate(m,True),m)
        tree=add_native_adt(mkadt.build(seed=bytes(64)),m)
        obj,_=adt.parse(adt.serialize(tree));chosen=next(n for n in obj.children if n.name=='chosen')
        self.assertEqual(chosen.prop('x9-ufs-root-write-contract'),struct.pack('<2Q',1,m['bytes']))
        self.assertEqual(chosen.prop('x9-ufs-protected-extents'),struct.pack('<2Q',100,200))
        for protected in [[],[{'partition_block':1000000,'blocks':1}],[{'partition_block':119101432,'blocks':1}]]:
            bad=copy.deepcopy(m);bad['write_policy']['protected_extents']=protected
            with self.assertRaises(ValueError):validate(bad,True)
        for key,value in [('version',2),('kind','old-root'),('initial_content_verified',False)]:
            bad=copy.deepcopy(m);bad['write_policy'][key]=value
            with self.assertRaises(ValueError):validate(bad,True)

if __name__=='__main__':unittest.main()
