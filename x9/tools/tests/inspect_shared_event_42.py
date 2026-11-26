#!/usr/bin/env python3
"""Verify QuartzCore's optimized cache stub target; pass .04 cache path."""
import struct
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit('usage: inspect_shared_event_42.py <dyld_shared_cache_arm64.04>')
p = Path(sys.argv[1])
data = p.read_bytes()
offset, count = struct.unpack_from('<II', data, 16)
va = 0x18dbcc748
for i in range(count):
    address, size, fileoff, _, _ = struct.unpack_from('<QQQII', data, offset + i * 32)
    if address <= va and va + 12 <= address + size:
        actual = data[fileoff + va - address:fileoff + va - address + 12]
        expected = bytes.fromhex('f0ed0a9010c2289100021fd6')
        assert actual == expected, actual.hex()
        print('PASS: 0x18dbcc748 -> 0x1a3988a30 (_IOSurfaceSignalEvent)')
        break
else:
    raise SystemExit('cache mapping missing optimized stub')
source = (Path(__file__).resolve().parents[3] / 'x9/kernel/src/iokit/Kernel/arm/SweetSurface.cpp').read_text()
assert 'if ((selector >= 36 && selector <= 42) || selector == 45)' in source
print('PASS: selector 42 enters opt-in gate')
