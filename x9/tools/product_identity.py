"""User-visible X9 identity with an iOS 16-compatible product/artwork profile.

The compatibility product ID is from the locally captured D22AP identity
(work/legacy-import/hackios-x9/xnuloader/identity.h). It selects an existing
MobileGestalt product table entry; DISPLAY_NAME is an independent custom label.
The compatible list also retains MT6993 for hardware isolation. Display labels
are separate from the internal iPhone10,3 identity used for DeviceClass.
"""
import struct

DISPLAY_NAME = 'iPhone20'
SERIAL_NUMBER = 'ST0RMYN0E'
COMPATIBILITY_PRODUCT = 'iPhone10,3'
COMPATIBILITY_BOARD = 'D22AP'
MODEL_NUMBER = 'PLJ110'
REGION_INFO = 'CN/A'  # software display suffix, not Apple factory data
PRODUCT_ID = bytes.fromhex('fd079b298144a7deb4d7c87fef3eb474d8f0e7eb')


def _cstr(value):
    if not value or '\0' in value:
        raise ValueError('identity strings must be nonempty and contain no NUL')
    return value.encode('utf-8') + b'\0'


def serial_property():
    serial = SERIAL_NUMBER.encode('ascii')
    if not 1 <= len(serial) <= 31 or any(c < 0x21 or c > 0x7e for c in serial):
        raise ValueError('serial must be 1..31 printable ASCII characters')
    return serial.ljust(32, b'\0')


def product_properties():
    if len(PRODUCT_ID) != 20:
        raise ValueError('MobileGestalt product-id must contain 20 bytes')
    return [
        ('product-name', _cstr(DISPLAY_NAME)),
        ('product-description', _cstr(DISPLAY_NAME)),
        ('product-id', PRODUCT_ID),
        ('compatible-device-fallback', _cstr('iPhone9,2')),
        ('artwork-device-idiom', _cstr('phone')),
        ('artwork-device-subtype', struct.pack('<I', 2436)),
        ('artwork-scale-factor', struct.pack('<I', 3)),
        # Current framebuffer supports BGRA8/sRGB, not the D22AP P3 path.
        ('artwork-display-gamut', _cstr('sRGB')),
    ]


def model_properties():
    def padded(value):
        data = value.encode('ascii')
        if not 1 <= len(data) < 32 or b'\0' in data:
            raise ValueError('model fields must fit a 32-byte ASCII property')
        return data.ljust(32, b'\0')
    return [
        ('model-number', padded(MODEL_NUMBER)),
        ('region-info', padded(REGION_INFO)),
        ('regulatory-model-number', padded(MODEL_NUMBER)),
    ]
