#!/usr/bin/env python3
"""Host-side validation for the CoreBrightness auto-brightness seed.

Checks, without a device:
  * the XML plist body embedded in sweet_x9_seed_corebrightness
    (kern_exec.c) parses and carries the expected state + curve;
  * the display container UUID is byte-identical in kern_exec.c and in
    SweetFramebuffer.cpp (X9_DISPLAY_CONTAINER_ID), in canonical form;
  * the framebuffer publishes VendorID/ProductID/kUSBContainerID;
  * the MG capabilities epoch marker is the 20261127 one;
  * the seeded two-segment curve approximates the OPPO factory
    lux -> level table (Android DisplayDeviceConfig, captured
    2026-11-27) within tolerance;
  * the seed is create-only (existing CB plist is probed, never renamed
    over).
"""
import plistlib
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KERN = (ROOT / "x9/kernel/src/bsd/kern/kern_exec.c").read_text()
FB = (ROOT / "x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp").read_text()

UUID = "7C9E6993-1102-4A5B-8C7D-202611270001"

# Android factory curve for this panel (dumpsys DisplayDeviceConfig):
# lux -> normalized brightness level.
FACTORY = {
    0: 0.011747006, 128: 0.23580436, 256: 0.37183917, 384: 0.3958453,
    512: 0.41985148, 640: 0.4438576, 768: 0.46786374, 896: 0.49186987,
    1024: 0.51587605, 2048: 0.7079252, 4096: 1.0,
}


def seed_function():
    start = KERN.index("sweet_x9_seed_corebrightness(proc_t p")
    end = KERN.index("\n}\n", start)
    return KERN[start:end]


def seed_plist():
    fn = seed_function()
    m = re.search(r'static const char body\[\]="(.*?)";', fn, re.S)
    assert m, "body string not found"
    raw = m.group(1).encode().decode("unicode_escape")
    return plistlib.loads(raw.encode())


def two_segment(lux, lux1, bl1, lux2, bl2, floor):
    if lux <= 0:
        return floor
    if lux < lux1:
        return floor + (bl1 - floor) * (lux / lux1)
    if lux < lux2:
        return bl1 + (bl2 - bl1) * ((lux - lux1) / (lux2 - lux1))
    return bl2


class TestCoreBrightnessSeed(unittest.TestCase):
    def test_plist_structure(self):
        p = seed_plist()
        self.assertIs(p["DisplayBrightnessAuto"], True)
        prefs = p["DisplayPreferences"]
        self.assertEqual(list(prefs.keys()), [UUID])
        curve = prefs[UUID]["AutoBrightnessCurve"]
        self.assertEqual(curve["Version"], 1)
        self.assertEqual(curve["Lux1"], 256.0)
        self.assertEqual(curve["Backlight1"], 0.3718)
        self.assertEqual(curve["Lux2"], 4096.0)
        self.assertEqual(curve["Backlight2"], 1.0)

    def test_uuid_canonical_and_shared(self):
        self.assertRegex(UUID, r"^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$")
        m = re.search(r'#define X9_DISPLAY_CONTAINER_ID\s+"([^"]+)"', FB)
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1), UUID)
        self.assertIn(UUID, seed_function())

    def test_framebuffer_identity(self):
        self.assertIn("#define X9_DISPLAY_VENDOR_ID", FB)
        self.assertIn("#define X9_DISPLAY_PRODUCT_ID", FB)
        for key in ("VendorID", "ProductID", "kUSBContainerID"):
            self.assertIn(f'setProperty("{key}"', FB)
        # backlight-child identity rides the X9 publishLevel branch
        i = FB.index("publishLevel(fBlLevel, 0, X9_BL_DBV_MAX)")
        j = FB.index('bl->setProperty("kUSBContainerID"')
        self.assertLess(i, j)

    def test_epoch_bumped(self):
        self.assertIn("refresh-capabilities-20261127", KERN)
        self.assertNotIn("refresh-capabilities-20261126", KERN)

    def test_gate_and_call_site(self):
        fn = seed_function()
        self.assertIn('sweet_x9_arg("x9_cb_seed",1)', fn)
        self.assertIn("sweet_x9_seed_corebrightness(p,scratch_addr", KERN)

    def test_create_only(self):
        fn = seed_function()
        kept = fn.index("not modified")
        rename = fn.index("rename_args")
        self.assertLess(kept, rename)

    def test_curve_fit(self):
        curve = seed_plist()["DisplayPreferences"][UUID]["AutoBrightnessCurve"]
        worst = 0.0
        for lux, want in FACTORY.items():
            got = two_segment(lux, curve["Lux1"], curve["Backlight1"],
                              curve["Lux2"], curve["Backlight2"], FACTORY[0])
            worst = max(worst, abs(got - want))
        self.assertLessEqual(worst, 0.05, f"worst fit error {worst}")


if __name__ == "__main__":
    unittest.main()
