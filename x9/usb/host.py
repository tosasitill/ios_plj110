#!/usr/bin/env python3
"""Inspect X9 composite USB or verify an explicitly running iOS raw echo client.

Requires pyusb + libusb. Never detaches ECM, resets the device, selects a new
configuration, or retries a possibly transmitted write. No serials printed.
"""
import argparse
import sys
import time

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--echo', help='send this UTF-8 text once to usbctl --echo (max4096 bytes)')
    ap.add_argument('--product', action='store_true', help='read product string via EP0, never read/print serial')
    a = ap.parse_args()
    import usb.core
    import usb.util
    dev = usb.core.find(idVendor=0x1209, idProduct=1)
    if dev is None:
        ap.error('1209:0001 absent')
    cfg = dev.get_active_configuration()
    print(f'VID={dev.idVendor:04x} PID={dev.idProduct:04x} configuration={cfg.bConfigurationValue}')
    if a.product:
        print(f'EP0 product={usb.util.get_string(dev, dev.iProduct)!r}; descriptor read, not mux/pairing')
    private = None
    for interface in cfg:
        print(f'interface={interface.bInterfaceNumber} alt={interface.bAlternateSetting} '
              f'class={interface.bInterfaceClass:02x}/{interface.bInterfaceSubClass:02x}/{interface.bInterfaceProtocol:02x} '
              f'endpoints={[(hex(e.bEndpointAddress), e.wMaxPacketSize) for e in interface]}')
        if (interface.bInterfaceClass, interface.bInterfaceSubClass, interface.bInterfaceProtocol) == (255, 0x58, 1):
            private = interface
    if a.echo is None:
        return
    data = a.echo.encode()
    if not 1 <= len(data) <= 4096:
        ap.error('echo must be 1..4096 UTF-8 bytes')
    if private is None or private.bInterfaceNumber != 2:
        ap.error('private interface absent: requires x9_usb=2, not just the EP0 logger')
    if dev.is_kernel_driver_active(2):
        ap.error('private interface is owned; will not detach any driver')
    usb.util.claim_interface(dev, 2)
    try:
        wrote = dev.write(3, data, timeout=5000)
        if wrote != len(data):
            raise RuntimeError(f'partial write {wrote}/{len(data)}; do not replay automatically')
        reply = bytearray()
        deadline = time.monotonic() + 5
        while len(reply) < len(data) and time.monotonic() < deadline:
            piece = dev.read(0x83, 4096, timeout=max(1, int((deadline-time.monotonic())*1000)))
            reply.extend(piece)
        if bytes(reply) != data:
            raise RuntimeError(f'echo mismatch ({len(reply)} received bytes)')
        print(f'PASS: real host OUT -> iOS receive/send -> host IN, {len(data)} identical bytes')
    finally:
        usb.util.release_interface(dev, 2)
        usb.util.dispose_resources(dev)

if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        print(f'USB: {e}', file=sys.stderr)
        sys.exit(1)
