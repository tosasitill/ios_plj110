#!/usr/bin/env python3
"""IOUSBDeviceController ABI fixture for X9 (iOS 16 20H392, iOS kext C++ ABI).

    python3 -I x9/usb/xdc/gen_usbdc_abi.py            # writes x9/usb/xdc/usbdc-20H392-abi.json

Names and signatures come from the KDK 13.5 (22G74) IOUSBDeviceFamily, which has
symbols: its IOUSBDeviceController vtable minus the macOS-only _RESERVED padding
slots is the iOS layout. The 20H392 kernelcache (stripped, plain pointers) holds
the matching 232-slot vtable in IOUSBDeviceFamily's __DATA_CONST; the check is
that slot count and all 18 __cxa_pure_virtual positions agree. Addresses are
given unslid (kernelcache) and as placed by build-usb-prelink.py (PRELINK_BASE).
Read-only; no device operations.
"""
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
from kext_vtable import vtable  # noqa: E402

KDK = Path('/Library/Developer/KDKs/KDK_13.5_22G74.kdk/System/Library/Extensions/IOUSBDeviceFamily.kext/Contents/MacOS/IOUSBDeviceFamily')
KC = ROOT / '.local/x9/usb/native-20H392/kernelcache.macho'
KC_VTABLE = 0xfffffff006e11200          # IOUSBDeviceController vtable (two zero words, then slots)
PRELINK_BASE, KC_BASE = 0xfffffe000a400000, 0xfffffff004dc8000
SLOTS, FIRST = 232, 168                 # IOService ends at slot 167 in the iOS ABI


def kc_slots():
    d = KC.read_bytes()
    n, = struct.unpack_from('<I', d, 16); o = 32; segs = []
    for _ in range(n):
        c, sz = struct.unpack_from('<II', d, o)
        if c == 0x19:
            va, vs, fo, fs = struct.unpack_from('<QQQQ', d, o + 24); segs.append((va, fo, fs))
        o += sz
    fo = next(f + KC_VTABLE - va for va, f, fs in segs if va <= KC_VTABLE < va + fs)
    assert d[fo:fo + 16] == bytes(16)
    out = [struct.unpack_from('<Q', d, fo + 16 + 8 * i)[0] for i in range(SLOTS + 1)]
    return out[:SLOTS], out[SLOTS]


def main():
    mac = [s for s in vtable(str(KDK), '__ZTV21IOUSBDeviceController') if '_RESERVED' not in s['mangled']]
    assert len(mac) == SLOTS, len(mac)
    kc, after = kc_slots()
    pv = max(set(kc[FIRST:]), key=kc[FIRST:].count)
    mac_pure = [i for i, s in enumerate(mac) if 'pure_virtual' in s['mangled']]
    kc_pure = [i for i, v in enumerate(kc) if v == pv]
    assert mac_pure == kc_pure, (mac_pure, kc_pure)
    slide = PRELINK_BASE - KC_BASE
    rows = []
    for i in range(FIRST, SLOTS):
        s = mac[i]
        pure = i in mac_pure
        rows.append({'slot': i, 'mangled': None if pure else s['mangled'], 'name': None if pure else s['name'],
                     'pure': pure, 'kc': hex(kc[i]), 'prelinked': hex(kc[i] + slide)})
    # Pure slots: the name comes from a concrete subclass in the same KDK.
    otg = Path(str(KDK).replace('MacOS/IOUSBDeviceFamily', 'PlugIns/AppleT7000USBOTGDevice.kext/Contents/MacOS/AppleT7000USBOTGDevice'))
    sub = [s for s in vtable(str(otg), '__ZTV23AppleT8103USBOTG3Device') if '_RESERVED' not in s['mangled']]
    for r in rows:
        if r['pure']:
            m = sub[r['slot']]['name']
            r['name'] = 'IOUSBDeviceController::' + m.split('::', 1)[1]
    out = {'source': 'KDK_13.5_22G74 IOUSBDeviceFamily + 20H392 kernelcache', 'kc_vtable': hex(KC_VTABLE),
           'prelinked_vtable': hex(KC_VTABLE + slide), 'pure_virtual_kc': hex(pv), 'slots': SLOTS, 'first': FIRST,
           'instance_size': 504, 'rows': rows}
    (HERE / 'usbdc-20H392-abi.json').write_text(json.dumps(out, indent=1) + '\n')
    print('ok: %d slots, %d pure, vtable %s' % (SLOTS, len(mac_pure), hex(KC_VTABLE + slide)))
    for r in rows:
        print('%3d %s %s' % (r['slot'], 'P' if r['pure'] else ' ', r['name']))


if __name__ == '__main__':
    main()
