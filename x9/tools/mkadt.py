#!/usr/bin/env python3
"""X9 Apple Device Tree for the SWEET xnu-8796 kernel; no UEFI sidecar.

Experimental eight-CPU topology. UFS, DSC/DSI, GPU and input are deliberately
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
from nvram_proxy import chosen_properties as nvram_chosen_properties
from product_identity import (product_properties, serial_property, model_properties,
                              COMPATIBILITY_PRODUCT, COMPATIBILITY_BOARD)

PHYS_BASE = 0x140000000
MEM_SIZE = 0x20000000
# Both ranges appear in the supplied and live X9 iomem/FDT captures.
# large4g is experimental: the active hardware baseline remains legacy512.
RAM_WINDOWS = {"legacy512": (PHYS_BASE, MEM_SIZE),
               "large4g": (0x200000000, 0x100000000),
               "large5g": (0x200000000, 0x13e000000),
               "segmented8g": (0x100000000, 0x27fa00000)}
SOC_BASE = 0x00400000
GICD, GICD_SIZE = 0x06800000, 0x40000
GICR, GICR_SIZE = 0x06880000, 0x200000
FB, FB_SIZE = 0xfc910000, 0x3660000
FB_W, FB_H, FB_STRIDE = 1256, 2760, 5120
# Display presented to iOS (IOMobileFramebuffer reads disp0 canvas-*): a real iPhone
# size, 414x896 pt @3x (iPhone XS Max / 11 Pro Max), centred on the 1256x2760 panel.
# 1256 px = 418.67 pt matched no iPhone keyboard layout: the keyboard frame (414)
# and its planes (418.67) disagreed, so every 123 switch reloaded the keyboard.
# 109.txt/USER: the 36 px top/bottom bars were ugly; the keyboard bug was the width
# (414 pt layout vs 418.67 pt screen), so keep 414 pt and use the full panel height
# (414x920 pt). 7 px stay black left/right.
CANVAS_W, CANVAS_H = 1242, 2760
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


def build(seed=None, timebase=TIMEBASE, console_base=0, console_size=0, ram_window="legacy512", wlan=False,
          baseband=False, torch=False):
    if ram_window not in RAM_WINDOWS:
        raise ValueError("unknown captured X9 RAM window")
    phys_base, mem_size = RAM_WINDOWS[ram_window]
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
        ('dram-base', u64(phys_base)), ('dram-size', u64(mem_size)),
        ('x9-ram-map', bytes(2064)),
        ('x9-gpu-handoff', bytes(64)),  # populated only after live LK FDT verification
        ('x9-scp-handoff', bytes(384)),  # layout only; never a firmware READY declaration
        ('x9-gps-handoff', bytes(64)),  # consys/HIF layout only, not MCU ownership/READY
        ('x9-adsp-handoff', bytes(256)),  # live shared-pool layout only, not EMI/firmware READY
        ('x9-lk-wifi-dt', bytes(16384)),  # shim fdt_wifi_dump.c: read-only Wi-Fi DT facts as text
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
        # Bootstrap policy metadata from dali's matching iOS16 profile.
        # These are software boot inputs, not device fuses or attestation.
        ('mix-n-match-prevention-status', u32(0)),
        ('effective-security-mode-ap', u32(1)),
        ('amfi-only-platform-code', u32(0)),
        ('protected-data-access', u32(1)),
        ('amfi-allows-trust-cache-load', u32(0)),
    ] + board_props + nvram_chosen_properties(), [node('memory-map', [ph()]),
                     node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))]),
                     node('manifest-properties', [ph(), ('BNCH', bytes(48))])])
    # Flatten hardware clusters into SWEET's one scheduler cluster. Physical
    # MPIDRs come from the captured running device FDT, not the node names.
    cpu_nodes = []
    for i, mpidr in enumerate((0x000,0x100,0x200,0x300,0x400,0x500,0x600,0x700)):
        cpu_nodes.append(node(f'cpu{i}', [ph(), ('device_type', cstr('cpu')),
            ('compatible', cstr('ARM,v8') + cstr('qcom,kryo470')),
            ('reg', u32(mpidr)), ('cpu-id', u32(i)), ('cluster-id', u32(0)),
            ('cluster-type', u32(0)), ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(i)), ('l2-cache-id', u32(0)), ('l3-cache-id', u32(0)),
            ('state', cstr('running' if i == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)), ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)
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
            ('canvas-width', u32(CANVAS_W)), ('canvas-height', u32(CANVAS_H))])] + ([
        # launchd loads wifid only if io_device_tree_entry_exists_with_name("wlan")
        # (com.apple.wifid.plist _LimitLoadToDeviceTree = [wlan, marconi-wifi]).
        # Shape of the iPhone X /arm-io/uart4/wlan node; MAC = the fake radio's.
        node('wlan', [ph(), ('device_type', cstr('wlan')),
            ('compatible', cstr('wlan-pcie,mt6653') + cstr('wlan-pcie')),
            ('module-instance', cstr('x9')),
            ('local-mac-address', bytes([0x02, 0x58, 0x39, 0x57, 0x46, 0x01]))])] if wlan else []))
    fb = node('sweet-fbcon', [ph(), ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)), ('height', u32(FB_H)), ('stride', u32(FB_STRIDE))])
    product = node('product', [ph()] + product_properties())
    if torch:
        # D22AP camera-flash discovery, as in hackios. Only advertise a flash,
        # not a supported ISP/capture camera or any Apple MMIO/GPIO routing.
        product.children.append(node('camera', [ph(), ('flash', u32(1))]))
    root = node('device-tree', [ph(),
        ('compatible', cstr(COMPATIBILITY_BOARD) + cstr(COMPATIBILITY_PRODUCT) +
         cstr('mediatek,MT6993') + cstr('oppo,x9') + cstr('xiaomi,sweet') + cstr('AppleARM')),
        ('model', cstr(COMPATIBILITY_PRODUCT)), ('target-type', cstr('D22')),
        ('target-sub-type', cstr(COMPATIBILITY_BOARD)), ('manufacturer', cstr('OPPO')),
        ('x9-hardware-model', cstr('OPPO Find X9')), ('x9-hardware-board', cstr('PLJ110')),
        ('serial-number', serial_property()),
        ('#address-cells', u32(2)), ('#size-cells', u32(2))] + model_properties(),
        [chosen, node('defaults', [ph(), ('no-effaceable-storage', u32(1))]),
         node('options', [ph()]),
         node('buttons', [ph(), ('home-button-type', u32(2))]), cpus, io, fb, product])
    if baseband:
        # Cellular discovery metadata from the original 20H392 D22AP tree.
        # MobileGestalt reads /baseband/class; the Coachz reference supplies
        # the same node but its J121AP/mav16/eUICC profile is not ours.
        # No Apple GPIO, SPMI, interrupt or MMIO descriptions: the X9 bridge
        # publishes its own services, and the physical MD still boots via CCCI.
        root.children.append(node('baseband', [ph(), ('class', u32(3)),
            ('compatible', cstr('baseband,n41')), ('device_type', cstr('baseband'))]))
        product.props.append(('baseband-chipset', 0, cstr('mav17')))
    return root


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out', type=Path)
    ap.add_argument('--timebase', type=lambda s: int(s, 0), default=TIMEBASE)
    ap.add_argument('--console-base', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--console-size', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--ram-window', choices=RAM_WINDOWS, default='legacy512')
    ap.add_argument('--baseband', action='store_true', help='include D22AP cellular discovery metadata (no hardware driver)')
    ap.add_argument('--torch', action='store_true', help='advertise flash capability for x9_torch=3 integration')
    ap.add_argument('--dump', action='store_true')
    a = ap.parse_args()
    root = build(timebase=a.timebase, console_base=a.console_base, console_size=a.console_size, ram_window=a.ram_window,
                 baseband=a.baseband, torch=a.torch)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    blob = adt.serialize(root)
    a.out.write_bytes(blob)
    if a.dump:
        adt.dump(root)
    print(f'{a.out}: {len(blob)} bytes')


if __name__ == '__main__':
    main()
