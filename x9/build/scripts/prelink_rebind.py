#!/usr/bin/env python3
"""XnuPrelink emit with a fixed kernel-import rebinding table (APFS closure).

    python3 -I x9/build/scripts/prelink_rebind.py <emit_prelink.py arguments>

The 20H392 APFS closure (com.apple.filesystems.apfs) starts kec.corecrypto,
AppleImage4 and CoreTrust.  Their start routines install kernel-global
interfaces this kernel owns (SWEET PRNG) or deliberately leaves empty
(img4if, coretrust, no late MAC policy).  Every kext->kernel call in a
prelinked component goes through a pointer binding (emit_prelink rejects
external PC-relative encodings), so rebinding exactly those imports at the
pointer level is complete.  The shims live in
x9/kernel/src/bsd/kern/sweet_prelink_kpi.c.

Rebinding is by original export name, with the native target required to be
unique.  The importers were enumerated from the plan's binding table
(2026-11-28): each name below is imported only by the module listed, so a
global by-name rebind is equivalent to a per-module one.  Any other binding
goes through XnuPrelink unchanged.  No device operations.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
XNUPRELINK = ROOT / '.local/references/XnuPrelink'

# original kernel export -> native shim (importer, for the record)
REBIND = {
    '_register_and_init_prng': '_sweet_prelink_decline_prng',           # kec.corecrypto
    '_img4_interface_register': '_sweet_prelink_decline_img4',          # AppleImage4
    '_mac_policy_register': '_sweet_prelink_decline_mac_policy',        # AppleImage4
    '_coretrust_interface_register': '_sweet_prelink_decline_coretrust',  # CoreTrust
}


def install(native_prelink, used):
    original = native_prelink.unique_kernel_binding

    def rebinding(address, original_exports, native_by_name):
        names = original_exports.get(address, [])
        hits = sorted({REBIND[n] for n in names if n in REBIND})
        if not hits:
            return original(address, original_exports, native_by_name)
        if len(hits) != 1:
            raise ValueError(f'Ambiguous rebinding at {address:#x}: {names}')
        choices = native_by_name.get(hits[0], set())
        if len(choices) != 1:
            raise ValueError(f'Rebind target {hits[0]} has {len(choices)} native candidates')
        for n in names:
            if n in REBIND:
                used[n] = used.get(n, 0) + 1
        return next(iter(choices))

    native_prelink.unique_kernel_binding = rebinding


def main():
    sys.path.insert(0, str(XNUPRELINK))
    import native_prelink
    import emit_prelink
    used = {}
    install(native_prelink, used)
    sys.argv = [str(XNUPRELINK / 'emit_prelink.py')] + sys.argv[1:]
    try:
        emit_prelink.main()
    finally:
        for name in sorted(REBIND):
            print(f'rebind {name} -> {REBIND[name]}: {used.get(name, 0)} site(s)', file=sys.stderr)


if __name__ == '__main__':
    main()
