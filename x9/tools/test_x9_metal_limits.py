"""Guard the PhotosUICore zero-limits fix: the Metal frontend must fill the base
class's inline limits struct with the measured texture dimensions.

PhotosUICore reads maxTextureWidth2D/Height2D directly from the struct
(limits+0x84/0x88) as its texture-redraw size; a struct left at zero makes it
request a 0x0 context and abort on its own assertion (PXGTextureUtilities.m:290).
The stock filler (an AGX hardware profile) does not exist on this port, so
NBDeviceMethods.initLimits writes the Vulkan-measured dimensions instead.
These assertions pin the three pieces together so the fix cannot silently
revert to an empty initLimits.
"""
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
GENERAL=ROOT/'x9/metal/General'
MM=(GENERAL/'NativeBridgeMetalFrontend.mm').read_text()
RS=(GENERAL/'bridge/NativeMetalObjects.rs').read_text()
H=(GENERAL/'bridge/NativeMetalObjects.h').read_text()
class MetalLimitsTests(unittest.TestCase):
 def test_bridge_declares_and_implements_texture_limits(self):
  self.assertIn('bool nmb_device_texture_limits(const NmbDevice *, uint32_t out[8]);',H)
  self.assertIn('pub extern "C" fn nmb_device_texture_limits',RS)
  for field in ('max_image_dimension_1d','max_image_dimension_2d','max_image_dimension_3d',
                'max_image_dimension_cube','max_image_array_layers'):
   self.assertIn(field,RS)
 def test_init_limits_fills_struct_from_measurements(self):
  self.assertNotIn('- (void)initLimits {}',MM)
  body=MM[MM.index('- (void)initLimits {'):]
  body=body[:body.index('\n}',0)+2]
  self.assertIn('nmb_device_texture_limits(gDevice,dims)',body)
  self.assertIn('class_getInstanceVariable(object_getClass(self),"_limits")',body)
  self.assertIn('ivar_getOffset(ivar)',body)
  # The two fields PhotosUICore reads, plus the rest of the dimension block.
  for off in ('0x80','0x84','0x88','0x8c','0x90','0x94','0x98','0x9c'):
   self.assertIn(off,body)
if __name__=='__main__':
 unittest.main()
