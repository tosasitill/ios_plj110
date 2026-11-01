#!/usr/bin/env python3
"""X9 Apple Device Tree for the SWEET xnu-8796 kernel; no UEFI sidecar.

Only cpu0 is enabled initially. UFS, DSC/DSI, GPU and input are deliberately
not described as supported drivers: their MT6789 sequences do not apply here.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from shared import adt

PHYS_BASE = 0x140000000
MEM_SIZE = 0x20000000
SOC_BASE = 0x00400000
GICD, GICD_SIZE = 0x06800000, 0x40000
GICR, GICR_SIZE = 0x06880000, 0x200000
FB, FB_SIZE = 0xfc910000, 0x3660000
FB_W, FB_H, FB_STRIDE = 1256, 2760, 5120
TIMEBASE = 1000000000  # captured arm,armv8-timer clock-frequency; override if needed


def u32(*v):
    return struct.pack('<%dI' % len(v), *v)


def u64(*v):
    return struct.pack('<%dQ' % len(v), *v)


def cstr(s):
    return s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, timebase=TIMEBASE, console_base=0, console_size=0):
    if not 0 < timebase <= 0xffffffff:
        raise ValueError('timebase must fit an ADT u32')
    if bool(console_base) != bool(console_size):
        raise ValueError('console-base and console-size must be given together')
    if console_base and (console_base & 3 or console_size < 16 or
                         console_base < 0xaec00000 or
                         console_base + console_size > 0xaef10000):
        raise ValueError('console zone must be aligned and inside the captured pstore carveout')
    seed = seed if seed is not None else os.urandom(64)
    if len(seed) != 64:
        raise ValueError('random seed must contain 64 bytes')
    handles = iter(range(1, 100))
    ph = lambda: ('AAPL,phandle', u32(next(handles)))
    board_props = [
        ('x9-gicd', u64(GICD)), ('x9-gicr', u64(GICR)),
        ('x9-wdt-restart', u64(0x1c010008)),
    ]
    if console_base:
        board_props += [('x9-console-base', u64(console_base)),
                        ('x9-console-size', u32(console_size))]
    chosen = node('chosen', [ph(),
        ('dram-base', u64(PHYS_BASE)), ('dram-size', u64(MEM_SIZE)),
        ('random-seed', seed), ('debug-enabled', u32(1)),
        ('firmware-version', cstr('x9-direct-shim')),
        ('system-firmware-version', cstr('x9-direct-shim')),
        ('boot-manifest-hash', hashlib.sha384(b'x9 direct shim manifest').digest()),
        ('crypto-hash-method', cstr('sha2-384')),
        ('display-scale', u32(3)),
        ('unique-chip-id', u64(0x5839000000000001)),  # synthetic, not a hardware ECID
        ('chip-id', u32(0x8015)),
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ] + board_props, [node('memory-map', [ph()]),
                     node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], [
        node('cpu0', [ph(), ('device_type', cstr('cpu')),
            # The qcom string is the existing CPU class match key, not an SoC claim.
            ('compatible', cstr('ARM,v8') + cstr('qcom,kryo470')),
            ('reg', u32(0)), ('cpu-id', u32(0)), ('cluster-id', u32(0)),
            ('cluster-type', u32(0)), ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(0)), ('l2-cache-id', u32(0)), ('l3-cache-id', u32(0)),
            ('state', cstr('running')),
            ('timebase-frequency', u32(timebase)), ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ])])
    gic = node('gic', [ph(), ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''), ('virtual-timer-intid', u32(27)),
        ('reg', u64(GICD - SOC_BASE, GICD_SIZE, GICR - SOC_BASE, GICR_SIZE))])
    io = node('arm-io', [ph(), ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)), ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x40000000 - SOC_BASE))], [gic,
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        node('disp0', [ph(), ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(FB_W)), ('canvas-height', u32(FB_H))])])
    fb = node('sweet-fbcon', [ph(), ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)), ('height', u32(FB_H)), ('stride', u32(FB_STRIDE))])
    return node('device-tree', [ph(),
        ('compatible', cstr('mediatek,MT6993') + cstr('oppo,x9') +
         cstr('xiaomi,sweet') + cstr('AppleARM')),
        ('model', cstr('OPPO Find X9')), ('target-type', cstr('X9')),
        ('target-sub-type', cstr('PLJ110')), ('manufacturer', cstr('OPPO')),
        ('#address-cells', u32(2)), ('#size-cells', u32(2))],
        [chosen, node('defaults', [ph(), ('no-effaceable-storage', u32(1))]),
         cpus, io, fb])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out', type=Path)
    ap.add_argument('--timebase', type=lambda s: int(s, 0), default=TIMEBASE)
    ap.add_argument('--console-base', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--console-size', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--dump', action='store_true')
    a = ap.parse_args()
    root = build(timebase=a.timebase, console_base=a.console_base, console_size=a.console_size)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    blob = adt.serialize(root)
    a.out.write_bytes(blob)
    if a.dump:
        adt.dump(root)
    print(f'{a.out}: {len(blob)} bytes')


if __name__ == '__main__':
    main()
