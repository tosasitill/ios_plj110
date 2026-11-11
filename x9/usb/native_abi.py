#!/usr/bin/env python3
"""Recover bidirectional, symbol-exact IOService ABI mappings for stock USB.

Private analysis only. A mapping is NOT a relocation or callable adapter:
receiver types, class geometry and native override thunks still need proof.
Unknown/ambiguous slots stay unmapped; never assume a universal +104 delta.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_native import (ROOT, SHA256, IOSERVICE_SLOTS, CONTROLLER_VPTR,
                            CONTROLLER_SLOTS, analyze, object_vtable)


def recover(data, object_path):
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
    from capstone.arm64 import ARM64_OP_REG, ARM64_OP_MEM
    abi, cache, entries, modules = analyze(data, object_path, [])
    exports = cache.exports()
    current = object_vtable(object_path, '__ZTV9IOService')
    stock_vptr = next(a for a, names in exports.items() if '__ZTV9IOService' in names) + 16
    mappings = []
    for slot in range(IOSERVICE_SLOTS):
        target, = struct.unpack('<Q', cache.read(stock_vptr + slot * 8, 8))
        aliases = exports.get(target, [])
        matches = [i for i, name in enumerate(current) if name in aliases]
        mappings.append(dict(stock_slot=slot, stock_target=target, aliases=aliases,
                             current_candidates=matches,
                             current_slot=matches[0] if len(matches) == 1 else None,
                             status='exact-symbol' if len(matches) == 1 else 'unmapped-or-ambiguous'))
    reverse = []
    for slot, symbol in enumerate(current):
        old = [m['stock_slot'] for m in mappings if symbol in m['aliases']]
        reverse.append(dict(current_slot=slot, symbol=symbol, stock_candidates=old,
                            stock_slot=old[0] if len(old) == 1 else None))
    cs = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    cs.detail = True
    callsites = []
    # Only an adjacent ldr Xfunc,[Xvtable,#offset]; blr Xfunc pair is recorded.
    # This cannot identify receiver type and is never emitted as a patch.
    for name in abi['components']:
        for seg in modules[name].segments:
            if not seg['initprot'] & 4 or not seg['filesize']:
                continue
            previous = None
            for ins in cs.disasm(cache.read(seg['va'], seg['filesize']), seg['va']):
                if (previous is not None and previous.address + 4 == ins.address
                        and previous.mnemonic == 'ldr' and ins.mnemonic == 'blr'
                        and len(previous.operands) == 2 and len(ins.operands) == 1):
                    dst, src = previous.operands
                    if (dst.type == ARM64_OP_REG and src.type == ARM64_OP_MEM
                            and ins.operands[0].type == ARM64_OP_REG
                            and dst.reg == ins.operands[0].reg
                            and previous.reg_name(dst.reg).startswith('x')
                            and src.mem.base and not src.mem.index
                            and src.mem.disp >= 0 and src.mem.disp % 8 == 0):
                        slot = src.mem.disp // 8
                        callsites.append(dict(component=name, load_pc=previous.address,
                            call_pc=ins.address, slot_candidate=slot,
                            receiver_type='unqualified', patchable=False,
                            ioservice_candidate=mappings[slot]['current_slot']
                            if slot < IOSERVICE_SLOTS else None))
                previous = ins
    mux_personalities = entries['com.apple.driver.AppleUSBDeviceMux']['IOKitPersonalities']
    match = mux_personalities['Apple USB Device Multiplexor']
    if (match.get('IOProviderClass') != 'IOUSBDeviceInterface'
            or match.get('IOPropertyMatch', {}).get('USBDeviceFunction') != 'AppleUSBMux'):
        raise ValueError('unexpected native mux binding contract')
    tail = []
    for slot in range(IOSERVICE_SLOTS, CONTROLLER_SLOTS):
        target, = struct.unpack('<Q', cache.read(CONTROLLER_VPTR + 8 * slot, 8))
        tail.append(dict(stock_slot=slot, proposed_current_slot=len(current)+slot-IOSERVICE_SLOTS,
                         target=target, pure_virtual='___cxa_pure_virtual' in exports.get(target, []),
                         callable=False, reason='class-specific layout/signature/overrides not qualified'))
    return dict(build='20H392', identity='d22ap', source_sha256=SHA256,
        ioservice=dict(stock_slots=IOSERVICE_SLOTS, current_slots=len(current),
                       forward=mappings, reverse=reverse),
        native_controller_tail=tail, virtual_call_candidates=callsites,
        native_mux_binding=match,
        native_bringup=entries['com.apple.iokit.IOUSBDeviceFamily']['IOKitPersonalities']['Configuration-standardBringup'],
        exact_forward_slots=sum(m['status']=='exact-symbol' for m in mappings),
        abi_compatible=False, runtime_loadable=False, patches_emitted=False,
        blockers=['unmapped/ambiguous base slots', 'per-call receiver types',
                  'current kernel -> native object reverse virtual dispatch',
                  'class instance geometry and OSMetaClass allocation',
                  'request timeout/cancel/DMA ownership', 'MTU3 native pure-virtual backend',
                  'native module relocation and bootstrap registration',
                  'wire DMA, native mux binding, original lockdown Pair/SSL session'])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernelcache', type=Path, required=True)
    ap.add_argument('--io-service-object', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    out = a.out.resolve()
    if out.exists() or not any(out != p.resolve() and out.is_relative_to(p.resolve())
                              for p in (ROOT/'.local', ROOT/'x9/out')):
        ap.error('use a fresh private output directory')
    data = a.kernelcache.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        ap.error('requires the reviewed stock D22/20H392 cache, not a built candidate')
    r = recover(data, a.io_service_object)
    out.mkdir(parents=True)
    (out/'native-abi.json').write_text(json.dumps(r, indent=2)+'\n')
    print(f"Exact base mappings: {r['exact_forward_slots']}/{IOSERVICE_SLOTS}; "
          f"virtual call candidates: {len(r['virtual_call_candidates'])}")
    print('Native bind: IOUSBDeviceInterface[USBDeviceFunction=AppleUSBMux] -> AppleUSBDeviceMux')
    print('No callable adapter, patches or load-readiness claim emitted')


if __name__ == '__main__':
    main()
