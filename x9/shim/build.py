#!/usr/bin/env python3
"""Cross-compile the direct X9 shim and build LK boot.img v0 (legacy LZ4).

build.py [PAYLOAD] [--out DIR] [--console-base PA --console-size SIZE]
PAYLOAD is SWEETXNU v1, embedded in Image so no vendor initrd scan is needed.
No device access, flashing, UEFI source or proprietary input is needed.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
from bootimg import check_image, pack, unpack
from toolchain import CFLAGS, abs64_relocs, command, run, symbols

SOURCES = ['start.S', 'main.c', 'hw.c', 'plog.c', 'loader.c', 'validate.c', 'fdt.c', 'fdt_property.c', 'gpu_handoff.c', 'scp_handoff.c', 'gps_handoff.c', 'mic_dma_handoff.c', 'adsp_handoff.c', 'display_lk.c', 'fbconsole.c', 'fdt_wifi_dump.c']


def validate_payload(blob, out):
    # Reuse the same no-side-effect validation that runs on the phone.
    library = out / 'payload-check.dylib'
    run(command('cc') + ['-O2', '-shared', '-fPIC', HERE / 'validate.c', '-o', library])
    checker = ctypes.CDLL(str(library))
    checker.payload_validate.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                         ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
    checker.payload_validate.restype = ctypes.c_char_p
    buf = ctypes.create_string_buffer(blob)
    adt, args = ctypes.c_uint64(), ctypes.c_uint64()
    result = checker.payload_validate(buf, len(blob), ctypes.byref(adt), ctypes.byref(args))
    if result:
        raise ValueError(result.decode())
    return {'adt_pa': hex(adt.value), 'boot_args_pa': hex(args.value)}


def build(payload_path=None, out=None, console_base=0, console_size=0, diag_only=False,
          pstore_mirror=False, usb_receiver=False, ufs_probe=False, ufs_root_manifest=None, ufs_native=False):
    out = Path(out or HERE / 'out').resolve()
    out.mkdir(parents=True, exist_ok=True)
    if bool(console_base) != bool(console_size):
        raise ValueError('console-base and console-size must be given together')
    if console_base and (console_base & 3 or console_size < 16 or
                         console_base < 0xaec00000 or
                         console_base + console_size > 0xaef10000):
        raise ValueError('console zone outside the captured pstore carveout')
    if ufs_root_manifest and (usb_receiver or diag_only):
        raise ValueError('UFS plaintext root cannot combine with USB or diagnostic-only mode')
    ufs_probe=ufs_probe or bool(ufs_root_manifest)
    definitions = [f'-DX9_CONSOLE_BASE={console_base}UL', f'-DX9_CONSOLE_SIZE={console_size}UL',
                   '-DX9_DIAG_ONLY=' + ('1' if diag_only else '0'),
                   '-DX9_PSTORE_MIRROR=' + ('1' if pstore_mirror else '0'),
                   '-DX9_USB_RECEIVER=' + ('1' if usb_receiver else '0'),
                   '-DX9_UFS_PROBE=' + ('1' if ufs_probe else '0')]
    if ufs_root_manifest:
        from ufs_root_manifest import load, header
        manifest=load(ufs_root_manifest,native=ufs_native)
        (out/'ufs_root_config.h').write_text(header(manifest,native=ufs_native))
        (out/'ufs-root-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        definitions += ['-DX9_UFS_ROOT=1','-I'+str(out),'-I'+str(HERE/'ufs')]
        ufs_probe=True
    else:
        definitions += ['-DX9_UFS_ROOT=0']
    definitions += ['-DX9_UFS_NATIVE='+('1' if ufs_native else '0')]
    objects = []
    sources = SOURCES + ["usb_log_shim.c", "usb_phy_lk.c"] + (["ramrx.c", "resident.c", "usb_mtu3.c", "fdt_ram.c", "usb_phy_lk.c", "usb_repeater_probe.c"] if usb_receiver else [])
    if ufs_probe:
        sources += ["ufs/ufs_ro.c", "ufs/ufs_volume.c", "ufs/f2fs_ro.c", "ufs/ufs_decrypt.c", "ufs/crypto/aes.c", "ufs/ufs_smmu.c", "ufs/ufs_link.c", "ufs/ufs_hs.c", "ufs/ufs_platform.c", "ufs/ufs_probe.c", "ufs/ufs_runtime.c", ]
        if 'fdt_ram.c' not in sources:
            sources.append('fdt_ram.c')
    if ufs_root_manifest:
        sources += ['ufs/ufs_root.c','ufs/sha256.c','ufs/ufs_boot.c','resident.c']
    for src in sources:
        obj = out / (Path(src).stem + '.o')
        run(command('cc') + CFLAGS + definitions + ['-c', HERE / src, '-o', obj])
        if src.endswith('.c') and abs64_relocs(obj):
            raise ValueError(f'absolute relocation in position-independent shim C: {src}')
        objects.append(obj)
    info = {}
    if payload_path:
        blob = Path(payload_path).read_bytes()
        info.update(validate_payload(blob, out))
        # Copy the selected container into the build output, then embed exactly it.
        stored_payload = out / 'embedded-payload.bin'
        stored_payload.write_bytes(blob)
        assembly = out / 'payload.S'
        assembly.write_text('.section .payload,"a"\n.balign 16\n.incbin "' +
                            str(stored_payload).replace('\\', '\\\\').replace('"', '\\"') + '"\n')
        obj = out / 'payload.o'
        run(command('cc') + CFLAGS + ['-c', assembly, '-o', obj])
        objects.append(obj)
        info['payload_sha256'] = hashlib.sha256(blob).hexdigest()
    elf = out / 'shim.elf'
    run(command('ld.lld') + ['-T', HERE / 'link.ld', '--build-id=none', '-static'] + objects + ['-o', elf])
    image_path = out / 'Image'
    run(command('objcopy') + ['-O', 'binary', elf, image_path])
    sym = symbols(elf)
    image = image_path.read_bytes()
    image += bytes(sym['_image_end'] - len(image))
    image_path.write_bytes(image)
    if check_image(image) != sym['_start']:
        raise ValueError('entry instruction does not target the linked shim')
    boot = pack(image)
    recovered, ramdisk, report = unpack(boot)
    if recovered != image:
        raise ValueError('LZ4/boot image round-trip failed')
    report.update(info)
    report.update({
        'boot_flow': 'LK -> x9 shim -> SWEETXNU -> XNU', 'uefi': False,
        'payload_offset': hex(sym['_payload_start']),
        'payload_bytes': sym['_payload_end'] - sym['_payload_start'],
        'console_base': hex(console_base), 'console_size': hex(console_size),
        'diag_only': diag_only, 'pstore_mirror': pstore_mirror,
        'display_path': 'lk-preserve-dali', 'legacy_display_linked': False,
        'shim_screen_log': True, 'usb_ram_receiver': usb_receiver,
        'ufs_readonly_probe': ufs_probe, 'ufs_plaintext_ram_root': bool(ufs_root_manifest) and not ufs_native, 'ufs_native_root':ufs_native,
        'scope': 'host-verified candidate, not a hardware boot result',
    })
    (out / 'shim-boot.img').write_bytes(boot)
    (out / 'shim-boot.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Image {len(image)} bytes; shim-boot.img {len(boot)} bytes; entry {report["entry_offset"]}'
          + ('; DIAG ONLY (no copy, no jump)' if diag_only else '')
          + ('; PSTORE MIRROR' if pstore_mirror else ''))
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('payload', nargs='?', type=Path)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--console-base', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--console-size', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--diag-only', action='store_true',
                    help='validate and log, then halt without copying or jumping')
    ap.add_argument('--pstore-mirror', action='store_true',
                    help='also mirror the log into the ramoops console zone for adb readback')
    ap.add_argument('--usb-receiver', action='store_true')
    ap.add_argument('--ufs-probe', action='store_true')
    a = ap.parse_args()
    build(a.payload, a.out, a.console_base, a.console_size, a.diag_only, a.pstore_mirror, a.usb_receiver, a.ufs_probe)


if __name__ == '__main__':
    main()
