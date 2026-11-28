"""Source assertions for APFS P0: IOKit storage publication of the
extent root disk (IOBlockStorageDevice -> IOMedia), gated off by
default so the HFS boot path is unchanged."""
from pathlib import Path
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
UFS = (ROOT / 'x9/kernel/src/bsd/dev/sweet_x9_ufs.c').read_text()
CPP = (ROOT / 'x9/kernel/src/iokit/Kernel/arm/SweetX9Storage.cpp').read_text()
BSD_HDR = (ROOT / 'x9/kernel/src/iokit/Kernel/arm/IOKit/storage/IOBlockStorageDevice.h').read_text()
STORAGE_HDR = (ROOT / 'x9/kernel/src/iokit/Kernel/arm/IOKit/storage/IOStorage.h').read_text()
PRELINK = (ROOT / 'x9/build/scripts/build-usb-prelink.py').read_text()


class GateAndBSDPathTests(unittest.TestCase):
    def test_publication_is_gated_default_off(self):
        self.assertIn('sweet_x9_arg("x9_iomedia",0)', UFS)
        # The publish call sits inside the gate, after log_start(), in
        # sweet_x9_ufs_rootdev -- i.e. after the BSD root initialized.
        gate = UFS.index('sweet_x9_arg("x9_iomedia",0)')
        call = UFS.index('sweet_x9_storage_publish();')
        self.assertLess(gate, call)
        self.assertEqual(UFS.count('sweet_x9_storage_publish();'), 1)
        rootdev = UFS[UFS.index('int sweet_x9_ufs_rootdev'):]
        self.assertLess(rootdev.index('log_start();'), rootdev.index('sweet_x9_arg("x9_iomedia",0)'))
        self.assertTrue(rootdev.rstrip().endswith('*device=xu.device;return 0;\n}') or
                        '*device=xu.device;return 0;' in rootdev)

    def test_media_accessors_share_strategy_transport_and_gating(self):
        for name in ('sweet_x9_media_info', 'sweet_x9_media_read',
                     'sweet_x9_media_write', 'sweet_x9_media_sync'):
            self.assertIn('int %s(' % name, UFS)
        read = UFS[UFS.index('int sweet_x9_media_read'):UFS.index('int sweet_x9_media_write')]
        self.assertIn('x9u_image_read_mapped(&xu.host,2,&xu.partition,xu.extents,xu.extent_count,xu.bytes/4096', read)
        self.assertIn('lck_mtx_lock(&x9ufs_lock)', read)
        self.assertIn('length>xu.bytes-offset', read)
        write = UFS[UFS.index('int sweet_x9_media_write'):UFS.index('int sweet_x9_media_sync')]
        self.assertIn('if(!xu.writable)return EROFS;', write)
        self.assertIn('x9u_image_write(&xu.host,offset,data,length)', write)
        sync = UFS[UFS.index('int sweet_x9_media_sync'):UFS.index('int sweet_x9_ufs_rootdev')]
        self.assertIn('x9u_sync_image(&xu.host)', sync)


class DeviceClassTests(unittest.TestCase):
    def test_every_pure_virtual_is_implemented(self):
        pure = re.findall(r'virtual\s+.+?\b(\w+)\s*\([^;{}]*?\)\s*(?:const\s*)?=\s*0\s*;', BSD_HDR, re.S)
        self.assertGreaterEqual(len(pure), 14)
        for name in pure:
            self.assertIn('SweetX9BlockStorageDevice::%s(' % name, CPP,
                          'missing implementation for %s' % name)

    def test_block_size_published_is_4096(self):
        self.assertIn('#define X9_MEDIA_BLOCK_SIZE 4096ULL', CPP)
        body = CPP[CPP.index('reportBlockSize'):]
        self.assertIn('*blockSize = X9_MEDIA_BLOCK_SIZE;', body)

    def test_fixed_media_answers(self):
        self.assertIn('*isRemovable = false;', CPP)
        self.assertIn('*isEjectable = false;', CPP)
        self.assertIn('return kIOReturnUnsupported;', CPP)  # eject/format
        # Write protection mirrors the BSD write contract snapshot.
        self.assertIn('*isWriteProtected = !writable;', CPP)

    def test_completion_exactly_once_and_direction_from_descriptor(self):
        # IOStorage::complete is a family symbol; its forwarder body is
        # open-coded and must be the single completion site.
        self.assertEqual(CPP.count('sweet_storage_complete(completion, status, done);'), 1)
        self.assertIn('completion->action(completion->target, completion->parameter,', CPP)
        self.assertNotIn('IOStorage::complete', CPP)
        self.assertIn('buffer->getDirection() == kIODirectionIn', CPP)

    def test_discrimination_markers(self):
        self.assertIn('device->setName("X9ExtentRoot");', CPP)
        self.assertIn('device->setProperty("x9-extent-media", true);', CPP)
        self.assertIn('X9 IOMEDIA: published IOBlockStorageDevice X9ExtentRoot', CPP)
        self.assertIn('registerService(kIOServiceSynchronous);', CPP)

    def test_no_macos_layout_slots_implemented(self):
        for name in ('doLockUnlockMedia', 'reportLockability',
                     'reportPollRequirements', 'doDiscard'):
            self.assertNotIn(name, CPP)


class BindingDesignTests(unittest.TestCase):
    def test_no_family_symbols_and_no_registration(self):
        # The class must not C++-derive from the family class (its
        # metaclass/ctor/virtuals are unresolvable at kernel link),
        # must not register any class, and must bind by name at
        # publish time instead.
        self.assertIn('class SweetX9BlockStorageDevice : public IOService', CPP)
        self.assertNotIn('OSDefineMetaClassAndStructors', CPP)
        self.assertNotIn('OSDeclareDefaultStructors', CPP)
        self.assertIn('OSMetaClass::getMetaClassWithName(name)', CPP)
        self.assertIn('sweet_storage_family_meta = family;', CPP)
        self.assertIn('return sweet_storage_family_meta;', CPP)

    def test_vtable_mirrors_family_declaration_order(self):
        def virtuals(text, start_marker):
            body = text[text.index(start_marker):]
            body = body[:body.index('\n};')]
            # Drop the forced-off (macOS-layout) blocks in the header.
            out, skip = [], False
            for line in body.split('\n'):
                if line.startswith('#if 0 /* X9:'):
                    skip = True
                elif skip and line.startswith('#endif'):
                    skip = False
                elif not skip:
                    out.append(line)
            body = '\n'.join(out)
            return re.findall(r'virtual\s+[^;{}]*?\b(\w+)\s*\(', body)
        family = virtuals(BSD_HDR, 'class __exported IOBlockStorageDevice')
        mine = virtuals(CPP, 'class SweetX9BlockStorageDevice')
        self.assertGreaterEqual(len(family), 25)
        self.assertEqual([n for n in mine if n in family], family)

    def test_reserved_slots_mirrored(self):
        for i in range(4):
            self.assertIn('OSMetaClassDeclareReservedUsed(SweetX9BlockStorageDevice, %d);' % i, CPP)
            self.assertIn('OSMetaClassDefineReservedUsed(SweetX9BlockStorageDevice, %d);' % i, CPP)
        for i in range(4, 32):
            self.assertIn('OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, %d);' % i, CPP)
            self.assertIn('OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, %d);' % i, CPP)


class VendoredHeaderTests(unittest.TestCase):
    def test_osx_blocks_forced_off(self):
        marker = '#if 0 /* X9: TARGET_OS_OSX forced off -- iOS layout */'
        self.assertEqual(BSD_HDR.count(marker), 4)
        self.assertEqual(STORAGE_HDR.count(marker), 6)
        self.assertIsNone(re.search(r'(?m)^#if TARGET_OS_OSX', BSD_HDR))
        self.assertIsNone(re.search(r'(?m)^#if TARGET_OS_OSX', STORAGE_HDR))

    def test_vendored_set_complete(self):
        d = ROOT / 'x9/kernel/src/iokit/Kernel/arm/IOKit/storage'
        for f in ('IOStorage.h', 'IOMedia.h', 'IOBlockStorageDevice.h',
                  'IOStorageDeviceCharacteristics.h',
                  'IOStorageProtocolCharacteristics.h',
                  'IOStorageControllerCharacteristics.h'):
            self.assertTrue((d / f).is_file(), f)
            self.assertIn('X9 VENDORED HEADER', (d / f).read_text())


class BuildWiringTests(unittest.TestCase):
    def test_recipe_and_link_order(self):
        recipe = json.loads((ROOT / 'x9/kernel/recipe.json').read_text())
        entries = [o for o in recipe['objects'] if o['name'] == 'SweetX9Storage.o']
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]['source'], 'iokit/Kernel/arm/SweetX9Storage.cpp')
        self.assertEqual(entries[0]['template'], 'iokit-cxx-3')
        order = (ROOT / 'x9/kernel/link-order.txt').read_text().split('\n')
        self.assertEqual(order.count('x9:SweetX9Storage.o'), 1)

    def test_prelink_root_includes_storage_family(self):
        # IOStorageFamily is the default (and only default) prelink root;
        # the historical USB roots are behind --usb-roots.
        self.assertIn("DEFAULT_ROOTS = ['com.apple.iokit.IOStorageFamily']", PRELINK)
        self.assertIn("root_args = [x for r in roots for x in ('--root', r)]", PRELINK)

    def test_prelink_base_derived_above_kernel_end(self):
        # The fixed 284-era anchor (0xfffffe000a400000) is only a floor: the
        # live base must clear the grown native kernel end + guard gap.
        self.assertIn('PRELINK_BASE_FLOOR = 0xfffffe000a400000', PRELINK)
        self.assertIn('prelink_base = max(PRELINK_BASE_FLOOR, (end + 0x40000 + PAGE - 1) & ~(PAGE - 1))', PRELINK)


if __name__ == '__main__':
    unittest.main()
