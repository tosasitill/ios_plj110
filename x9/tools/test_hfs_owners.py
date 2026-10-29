"""Catalog ownership edits must never change executable data or tree structure."""
import struct
import unittest
from hfs_owners import set_catalog_owners


def volume():
    data=bytearray(16384)
    data[1024:1026]=b'HX'
    struct.pack_into('>I',data,1064,4096)
    struct.pack_into('>Q',data,1024+272,2048)
    struct.pack_into('>II',data,1024+288,1,1)
    catalog=bytearray(2048)
    struct.pack_into('>I',catalog,24,1)
    struct.pack_into('>H',catalog,32,1024)
    struct.pack_into('>IIbBHH',catalog,1024,0,0,-1,1,5,0)
    offset=14
    for i,(cnid,parent,name,kind) in enumerate([(2,1,'Root',1),(16,2,'private',1),
            (17,16,'var',1),(18,17,'mobile',1),(19,18,'original',2)]):
        key=struct.pack('>HIH',6+len(name)*2,parent,len(name))+name.encode('utf-16be')
        body=bytearray(88 if kind==1 else 248)
        struct.pack_into('>H',body,0,kind)
        struct.pack_into('>I',body,8,cnid)
        struct.pack_into('>II',body,32,123,456)
        record=key+body
        struct.pack_into('>H',catalog,2048-2*(i+1),offset)
        catalog[1024+offset:1024+offset+len(record)]=record
        offset+=len(record)
    struct.pack_into('>H',catalog,2048-12,offset)
    data[4096:6144]=catalog
    data[8192:8256]=bytes(range(64))
    return bytes(data)


class HFSOwnersTests(unittest.TestCase):
    def test_root_and_mobile_ownership_only_touch_bsd_fields(self):
        original=volume()
        updated,entries=set_catalog_owners(original)
        by_path={e['path']:e for e in entries}
        self.assertEqual((by_path['/']['uid'],by_path['/']['gid']),(0,0))
        self.assertEqual(by_path['/private/var/mobile']['uid'],501)
        self.assertEqual(by_path['/private/var/mobile/original']['uid'],501)
        self.assertEqual(updated[8192:],original[8192:])
        # Running twice changes no additional bytes.
        again,_=set_catalog_owners(updated)
        self.assertEqual(again,updated)
        # Three root-owned records and two mobile records, 8 bytes each max.
        self.assertLessEqual(sum(a!=b for a,b in zip(updated,original)),40)

    def test_explicit_owner_applies_without_altering_file_content(self):
        updated,entries=set_catalog_owners(volume(),{'/private/var/mobile/original':(0,80)})
        entry=next(e for e in entries if e['path'].endswith('/original'))
        self.assertEqual((entry['uid'],entry['gid']),(0,80))
        self.assertEqual(updated[8192:8256],bytes(range(64)))

    def test_cycles_and_truncated_extents_are_rejected(self):
        broken=bytearray(volume());struct.pack_into('>I',broken,4096+1024,1)
        with self.assertRaisesRegex(ValueError,'cycle'):set_catalog_owners(broken)
        broken=bytearray(volume());struct.pack_into('>Q',broken,1024+272,8192)
        with self.assertRaises(ValueError):set_catalog_owners(broken)
        with self.assertRaises(ValueError):set_catalog_owners(b'not a volume')


if __name__=='__main__':unittest.main()
