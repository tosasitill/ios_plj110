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

    def apfs_manifest(self):
        m=self.manifest();m['write_policy']={'version':1,'kind':'dedicated-mutable-copy','initial_content_verified':True,
            'protected_extents':[{'partition_block':100,'blocks':200}]}
        m['apfs_policy']={'version':1,'kind':'dedicated-apfs-container','pinned':True,'immutable':True,
            'initial_content_verified':True,'image_sha256':'ab'*32,'bytes':(64+4)*1024**2,
            'extents':[{'file_block':0,'partition_block':9000000,'blocks':16384},
                       {'file_block':16384,'partition_block':9100000,'blocks':1024}]}
        return m
    def test_apfs_window_adt_appends_exact_protected_runs(self):
        m=self.apfs_manifest();self.assertEqual(validate(m,True),m)
        tree=add_native_adt(mkadt.build(seed=bytes(64)),m)
        obj,_=adt.parse(adt.serialize(tree));chosen=next(n for n in obj.children if n.name=='chosen')
        self.assertEqual(struct.unpack('<Q',chosen.prop('x9-ufs-apfs-bytes')),(68*1024**2,))
        self.assertEqual(struct.unpack('<6Q',chosen.prop('x9-ufs-apfs-extents')),(0,9000000,16384,16384,9100000,1024))
        self.assertEqual(chosen.prop('x9-ufs-protected-extents'),struct.pack('<6Q',100,200,9000000,16384,9100000,1024))
    def test_apfs_window_rejects_overlap_gap_size_and_weak_contract(self):
        cases=[]
        for target,value in [((0,'partition_block'),1000000),((0,'partition_block'),150),((1,'file_block'),16385),
                             ((1,'partition_block'),9000100)]:
            bad=self.apfs_manifest();bad['apfs_policy']['extents'][target[0]][target[1]]=value;cases.append(bad)
        for key,value in [('bytes',32*1024**2),('bytes',68*1024**2+512),('version',2),('kind','x'),
                          ('pinned',False),('immutable',None),('initial_content_verified',False),('image_sha256','')]:
            bad=self.apfs_manifest();bad['apfs_policy'][key]=value;cases.append(bad)
        bad=self.apfs_manifest();del bad['write_policy'];cases.append(bad)
        for bad in cases:
            with self.assertRaises(ValueError):validate(bad,True)

if __name__=='__main__':unittest.main()
