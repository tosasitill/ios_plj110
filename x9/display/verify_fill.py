#!/usr/bin/env python3
"""Verify boot -> decompressed Image -> real SWEETXNU header/ADT, offline."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from shared import adt, payload
import x9_boot_defaults


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(blob):
    return hashlib.sha256(blob).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('directory', type=Path)
    ap.add_argument('--kernel', required=True, type=Path)
    ap.add_argument('--fill', required=True, type=int, choices=(0, 1))
    ap.add_argument('--island', required=True, type=int, choices=(0, 1))
    a = ap.parse_args()
    directory = a.directory
    report = json.loads((directory / 'boot.json').read_text())
    boot = (directory / 'boot.img').read_bytes()
    image = (directory / 'shim/Image').read_bytes()
    blob = (directory / 'payload.bin').read_bytes()
    kernel = a.kernel.read_bytes()
    for field, data in [('boot', boot), ('image', image), ('payload', blob), ('kernel', kernel)]:
        require(sha(data) == report[field + '_sha256'], field + ' hash mismatch')
    require(boot[:8] == b'ANDROID!', 'bad Android boot magic')
    require(struct.unpack_from('<I', boot, 40)[0] == 4, 'not boot v4')
    compressed_size = struct.unpack_from('<I', boot, 8)[0]
    lz4 = shutil.which('lz4')
    require(lz4 is not None, 'lz4 tool required for actual boot decompression')
    decoded = subprocess.run([lz4, '-d', '-c'], input=boot[4096:4096 + compressed_size],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout
    require(decoded == image, 'boot kernel decompresses to a different Image')
    offset = int(report['payload_offset'], 0)
    require(image[offset:offset + len(blob)] == blob, 'payload not exactly embedded in Image')
    require(blob[:8] == b'SWEETXNU' and struct.unpack_from('<I', blob, 8)[0] == 1, 'bad payload header')
    adt_off, adt_size = struct.unpack_from('<QQ', blob, 0x20)
    require(adt_off + adt_size <= len(blob), 'ADT outside payload')
    tree_bytes = blob[adt_off:adt_off + adt_size]
    require(tree_bytes == (directory / 'x9.adt').read_bytes(), 'payload/generated ADT mismatch')
    tree, end = adt.parse(tree_bytes)
    require(end == len(tree_bytes), 'trailing ADT bytes')
    product = next(n for n in tree.children if n.name == 'product')
    io = next(n for n in tree.children if n.name == 'arm-io')
    display = next(n for n in io.children if n.name == 'disp0')
    dimensions = [struct.unpack('<I', display.prop(k))[0] for k in ('canvas-width', 'canvas-height')]
    require(dimensions == [1242, 2760], 'UIKit logical dimensions changed')
    subtype = struct.unpack('<I', product.prop('artwork-device-subtype'))[0]
    require(subtype == (2556 if a.island else 2436), 'wrong artwork subtype')
    args = blob[payload.CMDLINE:payload.CMDLINE + payload.BOOT_LINE_LENGTH].split(b'\0', 1)[0].decode()
    require(len(args.encode()) <= 607, 'boot args exceed bound')
    require(x9_boot_defaults.value(args.split(), 'x9_fill') == a.fill, 'wrong fill switch')
    require(x9_boot_defaults.value(args.split(), 'x9_ui') == a.island, 'wrong UI switch')
    require(b'X9 DISPLAY FILL:' in kernel and b'X9 HID FILL:' in kernel, 'missing compiled candidate paths')
    require(report['hardware_tested'] is False and report['device_written'] is False, 'unexpected verification scope')
    print(json.dumps(dict(name=directory.name, boot_sha256=sha(boot), boot_bytes=len(boot),
                         kernel_sha256=sha(kernel), payload_sha256=sha(blob), image_bytes=len(image),
                         canvas=dimensions, subtype=subtype, fill=a.fill, ui=a.island,
                         args_bytes=len(args.encode()), args=args,
                         verified='offline boot/Image/payload/ADT only; not panel, keyboard, HID or island runtime'), indent=2))


if __name__ == '__main__':
    main()
