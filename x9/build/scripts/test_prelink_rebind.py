"""prelink_rebind: only the listed imports move, to a unique native shim."""
import types
import unittest

import prelink_rebind as pr


class RebindTests(unittest.TestCase):
    def setUp(self):
        self.calls = []
        fake = types.SimpleNamespace()

        def original(address, original_exports, native_by_name):
            self.calls.append(address)
            return 0x1111
        fake.unique_kernel_binding = original
        self.used = {}
        pr.install(fake, self.used)
        self.bind = fake.unique_kernel_binding

    def test_listed_import_goes_to_shim(self):
        orig = {0x10: ['_register_and_init_prng'], 0x20: ['_mac_policy_register']}
        native = {'_sweet_prelink_decline_prng': {0xAAA}, '_sweet_prelink_decline_mac_policy': {0xBBB}}
        self.assertEqual(self.bind(0x10, orig, native), 0xAAA)
        self.assertEqual(self.bind(0x20, orig, native), 0xBBB)
        self.assertEqual(self.used, {'_register_and_init_prng': 1, '_mac_policy_register': 1})
        self.assertEqual(self.calls, [])

    def test_unlisted_import_uses_xnuprelink(self):
        self.assertEqual(self.bind(0x30, {0x30: ['_register_crypto_functions']}, {}), 0x1111)
        self.assertEqual(self.calls, [0x30])

    def test_missing_or_duplicate_shim_fails(self):
        orig = {0x10: ['_img4_interface_register']}
        for native in [{}, {'_sweet_prelink_decline_img4': {1, 2}}]:
            with self.assertRaises(ValueError):
                self.bind(0x10, orig, native)


if __name__ == '__main__':
    unittest.main()
