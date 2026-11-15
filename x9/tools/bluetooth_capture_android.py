#!/usr/bin/env python3
"""Read-only capture of the running MT6993 Android Bluetooth inputs.

Fresh gitignored output only. No radio toggle, HCI injection, debug-register
writes, reboot, partition writes or process instrumentation. Device identity,
calibration, logs and process addresses stay in the private output directory.
"""
import argparse
import json
import re
import shlex
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def elf_object(data, name):
    """Resolve an exported ELF64 LE object from this exact served library."""
    if (len(data) < 64 or data[:6] != b'\x7fELF\x02\x01' or
            struct.unpack_from('<H', data, 18)[0] != 183):
        raise ValueError('expected ELF64 little-endian AArch64 library')
    off = struct.unpack_from('<Q', data, 40)[0]
    width, count = struct.unpack_from('<HH', data, 58)
    if width != 64 or not count or off + width * count > len(data):
        raise ValueError('invalid section table')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, off + i * width)
                for i in range(count)]
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM
            continue
        if section[6] >= len(sections) or section[4] + section[5] > len(data):
            raise ValueError('out-of-bounds dynamic symbol table')
        strings = sections[section[6]]
        if strings[1] != 3 or strings[4] + strings[5] > len(data):
            raise ValueError('out-of-bounds symbol string table')
        table = data[strings[4]:strings[4] + strings[5]]
        if section[9] != 24 or section[5] % 24:
            raise ValueError('invalid dynamic symbol table')
        for at in range(section[4], section[4] + section[5], 24):
            index, info, _, shndx, address, size = struct.unpack_from('<IBBHQQ', data, at)
            if index >= len(table) or b'\0' not in table[index:]:
                raise ValueError('invalid dynamic symbol name')
            if table[index:].split(b'\0', 1)[0] == name.encode():
                if info & 15 != 1 or not shndx or not 16 <= size <= 65536:
                    raise ValueError('unexpected exported object type/size')
                return address, size
    raise ValueError(f'exported object not found: {name}')


def live_object_address(data, maps, address, size):
    """Use this ELF's PT_LOAD and current readable mappings, not a stale base."""
    phoff = struct.unpack_from('<Q', data, 32)[0]
    width, count = struct.unpack_from('<HH', data, 54)
    if width != 56 or not count or phoff + width * count > len(data):
        raise ValueError('invalid ELF program headers')
    loads = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * width)
             for i in range(count)]
    first = [p for p in loads if p[0] == 1 and p[2] == 0 and p[5] >= 64]
    if len(first) != 1:
        raise ValueError('ambiguous ELF file-offset-zero PT_LOAD')
    mapped = []
    for line in maps.splitlines():
        match = re.fullmatch(rb'([0-9a-f]+)-([0-9a-f]+)\s+(r[-w][-x]p)\s+'
                             rb'([0-9a-f]+)\s+\S+\s+\d+(?:\s+(.*))?', line)
        if match:
            mapped.append((int(match[1], 16), int(match[2], 16),
                           int(match[4], 16), match[5] or b''))
    library = b'/vendor/lib64/libbluetooth_mtk.so'
    bases = [start - first[0][3] for start, _, off, path in mapped
             if off == 0 and path == library]
    if len(bases) != 1:
        raise ValueError('current library load bias is unavailable or ambiguous')
    live = bases[0] + address
    if not any(p[0] == 1 and p[3] <= address and address + size <= p[3] + p[6]
               for p in loads):
        raise ValueError('exported object is outside ELF PT_LOAD')
    # Android's last PT_LOAD has filesz < memsz. btinit crosses the end of
    # its file-backed mapping into bionic's anonymous .bss. Inspect mappings
    # of the exact symbol range, never require all bytes to be file-backed,
    # and never extend the read into a heap or another library.
    covered = live
    for start, end, _, path in sorted(mapped):
        if start <= covered < end and path in (library, b'', b'[anon:.bss]'):
            covered = min(end, live + size)
        if covered == live + size:
            return live
    raise ValueError('complete exported object is outside readable library/.bss mappings')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='/opt/homebrew/bin/adb')
    parser.add_argument('--serial', help='otherwise require exactly one online device; never print it')
    parser.add_argument('--out', type=Path, required=True, help='new directory under x9/out/')
    parser.add_argument('--hci', help='explicit original .cfa/.btsnoop path under /data/misc/bluetooth/logs/')
    args = parser.parse_args()
    out = args.out.resolve()
    out.relative_to((ROOT / 'x9/out').resolve())
    if out.exists():
        parser.error('output already exists; original captures must not be overwritten')
    devices = subprocess.run([args.adb, 'devices'], capture_output=True, check=True)
    online = [line.split(b'\t')[0].decode() for line in devices.stdout.splitlines()
              if line.endswith(b'\tdevice')]
    if args.serial:
        if args.serial not in online:
            parser.error('selected device is not online')
        serial = args.serial
    elif len(online) == 1:
        serial = online[0]
    else:
        parser.error('exactly one online device is required')
    out.mkdir(parents=True)
    originals = out / 'originals'
    originals.mkdir()
    (out / 'adb-devices-private.txt').write_bytes(devices.stdout + devices.stderr)
    (out / 'device-private.txt').write_text(serial + '\n')
    results = []

    def capture(name, command, binary=False, magic=None, exact_size=None):
        destination = originals / name if binary else out / name
        try:
            result = subprocess.run([args.adb, '-s', serial, 'exec-out',
                                     'su -c ' + shlex.quote(command)],
                                    capture_output=True, timeout=90)
        except subprocess.TimeoutExpired as error:
            destination.with_name(name + '.failed').write_bytes(error.stdout or b'')
            destination.with_name(name + '.stderr').write_bytes(error.stderr or b'')
            results.append({'name': name, 'exit': None,
                            'bytes': len(error.stdout or b''), 'valid': False, 'timeout': True})
            (out / 'capture-status.json').write_text(json.dumps(results, indent=2) + '\n')
            raise RuntimeError('ADB capture timed out; preserved previous captures') from None
        data = result.stdout
        valid = (result.returncode == 0 and bool(data) and
                 not data.startswith((b'cat:', b'dd:', b'error:', b'su:',
                                      b'/system/bin/sh:', b'sh:')) and
                 (magic is None or data.startswith(magic)) and
                 (exact_size is None or len(data) == exact_size))
        # exec-out can return 0 with a cat error in stdout: retain failures,
        # but never give them a successful .so/.ko/.bin input filename.
        (destination if valid else destination.with_name(name + '.failed')).write_bytes(data)
        if result.stderr:
            destination.with_name(name + '.stderr').write_bytes(result.stderr)
        results.append({'name': name, 'exit': result.returncode,
                        'bytes': len(data), 'valid': valid})
        (out / 'capture-status.json').write_text(json.dumps(results, indent=2) + '\n')
        print(f'{name}: {len(data)} B, ' + ('captured' if valid else 'FAILED'))
        if result.returncode == 255:
            raise RuntimeError('ADB connection lost; preserved previous captures')
        return data if valid else None

    capture('identity-original.txt', 'getprop ro.build.version.release; '
            'getprop ro.build.version.incremental; getprop ro.boot.slot_suffix; id')
    processes = capture('bluetooth-processes-original.txt',
                        "ps -A -o PID,ARGS | grep -E 'bluetooth-service-mediatek|uart_launcher'")
    if processes is None:
        raise RuntimeError('running Bluetooth HAL/launcher unavailable')
    hal = re.search(rb'^\s*(\d+)\s+/vendor/bin/hw/android\.hardware\.bluetooth-service-mediatek(?:\s|$)',
                    processes, re.M)
    launcher = re.search(rb'^\s*(\d+)\s+/vendor/bin/uart_launcher(?:\s|$)', processes, re.M)
    if not hal or not launcher:
        raise RuntimeError('expected live HAL and uart_launcher not found')
    pid, uart_pid = int(hal[1]), int(launcher[1])
    capture('launcher-command-original.bin', f'cat /proc/{uart_pid}/cmdline', binary=True)
    maps = capture('hal-library-maps-original.txt',
                   f'cat /proc/{pid}/maps')
    properties = ('persist.bluetooth.countrycode', 'persist.bluetooth.country.nv.switch',
                  'persist.vendor.bt.sspdebug.enable', 'persist.vendor.bt.pip.enable',
                  'ro.vendor.mtk_protocol1_rat_config', 'vendor.bluetooth.state',
                  'ro.vendor.bt.connac3')
    capture('vendor-properties-original.txt',
            'for k in ' + ' '.join(properties) + '; do echo "$k=$(getprop $k)"; done')
    capture('kernel-bluetooth-original.txt',
            "dmesg | grep -Ei 'btmtk|uarthub|SSPM|connv3|CONNXO'")
    capture('modules-original.txt', "cat /proc/modules | grep -Ei 'btmtk|uarthub|sspm|8250_mtk'")
    # stty -a only reads the existing tty's settings. Its encoded speed may
    # be the launcher startup value, NOT the hardware's later 24M baud.
    capture('launcher-termios-original.txt', 'stty -F /dev/ttyS3 -a')
    capture('bt-fw-version-original.txt', 'cat /proc/stpbt/bt_fw_version')
    library = None
    for path in ('/vendor/bin/hw/android.hardware.bluetooth-service-mediatek',
                 '/vendor/bin/uart_launcher', '/vendor/lib64/libbluetooth_mtk.so',
                 '/vendor/lib64/libbt-vendor.so', '/vendor/lib/modules/btmtk_uart_unify.ko',
                 '/vendor/lib/modules/uarthub_drv.ko'):
        value = capture(Path(path).name, 'cat ' + shlex.quote(path), binary=True, magic=b'\x7fELF')
        if path.endswith('/libbluetooth_mtk.so'):
            library = value
    # Both names come from GORM_FW_Init_Thread/parse_fw_cfg_para. The first
    # is preferred; absence/failure must not be mistaken for empty commands.
    capture('BT_FW-preferred-original.cfg', 'cat /vendor/odm/etc/bt/BT_FW.cfg', binary=True)
    capture('BT_FW-fallback-original.cfg', 'cat /vendor/firmware/BT_FW.cfg', binary=True)
    capture('init.btmtk_uart-original.rc', 'cat /vendor/etc/init/init.btmtk_uart.rc', binary=True)
    capture('BT_RAM_CODE_MT6653_dx5_2g2a_1_hdr.bin',
            'cat /vendor/firmware/BT_RAM_CODE_MT6653_dx5_2g2a_1_hdr.bin', binary=True)
    slot = capture('active-slot-original.txt', 'getprop ro.boot.slot_suffix')
    if slot and slot.strip() in (b'_a', b'_b'):
        path = '/dev/block/by-name/sspm' + slot.strip().decode()
        size = capture('sspm-size-original.txt', 'blockdev --getsize64 ' + path)
        if size and size.strip() == b'2097152':
            capture('sspm-active-original.bin', 'cat ' + path, binary=True,
                    magic=b'\x88\x16\x88\x58', exact_size=2097152)
    if library is not None and maps is not None:
        address, size = elf_object(library, 'btinit')
        # This object's complete symbol size is 6448, not just its
        # 6136-byte NVRAM member. The tail includes parsed cfg inputs.
        live_address = live_object_address(library, maps, address, size)
        capture('btinit-active-private.bin',
                f'dd if=/proc/{pid}/mem bs=1 skip={live_address} count={size}',
                binary=True, exact_size=size)
    if args.hci:
        path = Path(args.hci)
        if '..' in path.parts or not str(path).startswith('/data/misc/bluetooth/logs/'):
            parser.error('HCI original must be under the Bluetooth logs directory')
        capture('android-hci-original.cfa', 'cat ' + shlex.quote(str(path)),
                binary=True, magic=b'btsnoop\0')
    print('Read-only capture finished; no controller commands or device writes performed.')


if __name__ == '__main__':
    main()
