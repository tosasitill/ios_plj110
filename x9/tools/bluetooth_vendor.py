#!/usr/bin/env python3
"""Port the captured MT6653 Android preload constructors, without emulation.

Inputs are ORIGINAL vendor ELF, complete btinit, properties and preferred cfg.
No emulator transcripts, approved-packet JSON, default calibration or device
I/O. Offsets below are btinit-relative (NVRAM starts at +0x10), and are traced
to load/store instructions in this exact original ELF. Output contains private
address/calibration data and must never be committed.
"""
import argparse
import hashlib
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / '.local/x9/bluetooth/android-mt6653'
ELF_SHA256 = '48e11132d3057ba0ea97efd3ea01ecc0575f2b4f839570eee35c31fbc7d9655b'
CONSTRUCTORS = (0x1af00, 0x1ffb0, 0x202b0, 0x1c9d0, 0x1e8f0,
                0x20660, 0x1f740, 0x24700, 0x20c30, 0x24930, 0x24bd0,
                0x24e80, 0x25140, 0x25400, 0x25690, 0x258c0, 0x25ad0,
                0x25cd0, 0x25f90, 0x26250, 0x26510, 0x1fc40,
                0x267d0, 0x269a0)
INPUT_NAMES = ('libbluetooth_mtk.so', 'btinit.bin', 'properties.txt', 'BT_FW.cfg')


def elf_reader(blob):
    if (hashlib.sha256(blob).hexdigest() != ELF_SHA256 or
            blob[:6] != b'\x7fELF\x02\x01' or
            struct.unpack_from('<H', blob, 18)[0] != 183):
        raise ValueError('unreviewed Android vendor ELF; do not reuse offsets')
    phoff = struct.unpack_from('<Q', blob, 32)[0]
    size, count = struct.unpack_from('<HH', blob, 54)
    if size != 56 or phoff + size * count > len(blob):
        raise ValueError('invalid original ELF program headers')
    segments = []
    for i in range(count):
        kind, _, offset, va, _, filesz, _, _ = struct.unpack_from(
            '<IIQQQQQQ', blob, phoff + i * size)
        if offset + filesz > len(blob):
            raise ValueError('truncated original ELF segment')
        if kind == 1:
            segments.append((va, offset, filesz))

    def read(va, length):
        for base, offset, size in segments:
            if base <= va and va + length <= base + size:
                return blob[offset + va - base:offset + va - base + length]
        raise ValueError(f'original ELF address not file-backed: {va:x}')

    table = struct.unpack('<25Q', read(0x85968, 25 * 8))
    if table != (*CONSTRUCTORS, 0):
        raise ValueError('unexpected MT6653 preload table/order')
    return read


def properties(text):
    result = {}
    for line in text.splitlines():
        key, separator, value = line.partition('=')
        if not separator or not key or key in result:
            raise ValueError('invalid/duplicate captured property')
        result[key] = value
    for key in ('persist.bluetooth.countrycode',
                'persist.bluetooth.country.nv.switch',
                'persist.vendor.bt.sspdebug.enable',
                'persist.vendor.bt.pip.enable',
                'ro.vendor.mtk_protocol1_rat_config', 'ro.vendor.bt.connac3'):
        if key not in result:
            raise ValueError(f'missing captured property: {key}')
    if result['ro.vendor.bt.connac3'] != 'yes':
        raise ValueError('not the captured connac3 production path')
    # GORM_FW_Init_Thread 27ff0..28130: these are outer skips. Other
    # selections require their own reviewed constructors, never an empty op.
    for key in ('persist.bluetooth.country.nv.switch',
                'persist.vendor.bt.sspdebug.enable', 'persist.vendor.bt.pip.enable'):
        if result[key] != '':
            raise ValueError(f'unreviewed property branch: {key}')
    return result


def h4(opcode, parameters):
    if opcode >> 10 != 0x3f or opcode == 0xfc6f or len(parameters) > 255:
        raise ValueError('not a bounded non-WMT vendor command')
    return b'\x01' + struct.pack('<HB', opcode, len(parameters)) + parameters


def cfg_commands(text, original):
    """Strict preferred-file parsing; no fallback/partial/default config."""
    commands, upper, fields = [], {}, {}
    for line in text.splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        key, separator, value = line.partition(':')
        if not separator:
            raise ValueError('malformed preferred BT_FW.cfg line')
        if key == 'VENDOR_CMD':
            tokens = value.split()
            if not all(re.fullmatch(r'0x[0-9a-fA-F]{2}', x) for x in tokens):
                raise ValueError('invalid preferred VENDOR_CMD byte')
            packet = bytes(int(x, 16) for x in tokens)
            if len(packet) < 3 or len(packet) != packet[2] + 3:
                raise ValueError('preferred VENDOR_CMD length mismatch')
            commands.append(h4(int.from_bytes(packet[:2], 'little'), packet[3:]))
        elif key.startswith('TX_power_upper_lmt '):
            index = int(key.removeprefix('TX_power_upper_lmt '))
            if not 0 <= index < 16 or index in upper:
                raise ValueError('invalid/duplicate TX upper-limit index')
            upper[index] = 0x7f if value.strip() == 'reserved' else int(value)
            if not -128 <= upper[index] <= 127:
                raise ValueError('TX upper limit outside signed byte')
        else:
            if key in fields:
                raise ValueError('duplicate preferred configuration field')
            fields[key] = value.strip()
    # These reviewed non-command fields are the current cfg path. Reject a
    # future setting rather than silently ignoring coex/antenna changes.
    expected = {key: '0x00' for key in (
        'coex_bt_ctrl', 'coex_bt_ctrl_mode', 'coex_bt_ctrl_rw',
        'coex_opp_time_ratio', 'coex_opp_time_ratio_bt_slot',
        'coex_opp_time_ratio_wifi_slot', 'coex_lescan_time_ratio',
        'coex_lescan_time_ratio_bt_slot', 'coex_lescan_time_ratio_wifi_slot')}
    expected.update({'bt_antswap': '0x00 0x00', 'wifi_antswap': '0x81 0x03',
                     'bt_tssi_from_wifi': '3', 'bt_tssi_target': '3776',
                     'region_power': '0x14 0x14 0x14 0x16 0x16 0x16 '
                                     '0x15 0x15 0x15 0x13 0x13 0x13'})
    if fields != expected or set(upper) != set(range(16)):
        raise ValueError('unreviewed/incomplete preferred configuration')
    limits = bytes(upper[i] & 255 for i in range(16))
    if limits != original[0x1819:0x1829]:
        raise ValueError('preferred cfg and captured parsed TX limits differ')
    # Production requires this last append. Do not mistake the examples in
    # comments for effective commands, or drop the last non-newline line.
    if commands != [h4(0xfc37, bytes.fromhex('0d 00 01'))]:
        raise ValueError('unreviewed preferred append sequence')
    return commands


def construct(source):
    read = elf_reader((source / 'libbluetooth_mtk.so').read_bytes())
    b = (source / 'btinit.bin').read_bytes()
    if len(b) != 6448 or struct.unpack_from('<III', b, 4) != (0x6993, 0x6653, 4):
        raise ValueError('complete production MT6993/MT6653/type4 btinit required')
    p = properties((source / 'properties.txt').read_text())
    address = b[0x10:0x16]
    if address in (b'\0' * 6, b'\xff' * 6):
        raise ValueError('random/default address path is not supported')
    if b[0x1c5] & 0x80 or b[0xc15]:
        raise ValueError('unreviewed optional power-table branch; refuse before power')

    packets = []

    def add(function, opcode, data):
        packets.append((function, h4(opcode, data)))

    # 1b190..1b1c0: six NVRAM BDADDR bytes are stored in reverse order.
    add(0x1af00, 0xfc1a, address[::-1])
    # 1ffb0: literal header word + halfword0x8000 + byte0.
    voice = read(0x145a8, 4) + struct.pack('<H', 0x8000) + b'\0'
    add(0x1ffb0, int.from_bytes(voice[:2], 'little'), voice[3:])
    # 20304 signed-byte branch, 2032c..34 short / 2045c..88 long.
    length = 52 if b[0x1c6] & 0x80 else 22
    add(0x202b0, 0xfc2d, b'\x03' + b[0x1c6:0x1c6 + length])
    add(0x1c9d0, 0xfc7a, b[0x22:0x29])
    # SSPdebug outer skip, proven by captured property and GORM branch.
    # 20660 MT6653: first32 literal bytes, then a word at packet+31.
    sleep = bytearray(read(0x1480c, 32) + b'\0' * 3)
    struct.pack_into('<I', sleep, 31, 0x028005e0)
    add(0x20660, int.from_bytes(sleep[:2], 'little'), bytes(sleep[3:]))
    add(0x1f740, 0xfc2d, read(0x145e0, 4)[3:] + b[0xc8:0x1c6])
    add(0x24700, 0xfc2d, read(0x145f0, 4)[3:] + b[0x97e:0xa7c])
    if p['ro.vendor.mtk_protocol1_rat_config'] != 'no':
        add(0x20c30, 0xfcfb, read(0x14608, 4)[3:] + b'\x01')
    # 24964/24c04: low two-bit and next two-bit selections, not bit0 only.
    for function, mask, index, start in ((0x24930, 3, 0, 0x1fa),
                                         (0x24bd0, 12, 1, 0x2ea)):
        if b[0x1c4] & mask:
            add(function, 0xfc2d, bytes([4, index]) + b[start:start + 240])
    # 24f18..64 / 251dc..25228: INTERLEAVE 96+6+96+6, not192+12.
    for function, mask, index, start, tail in ((0x24e80, 3, 0, 0x3da, 0x67a),
                                               (0x25140, 0x30, 1, 0x55a, 0x692)):
        if b[0x1c5] & mask:
            data = (bytes([5, index]) + b[start:start + 96] + b[tail:tail + 6] +
                    b[start + 96:start + 192] + b[tail + 6:tail + 12])
            add(function, 0xfc2d, data)
    # 25434 bit7 clear: no command. Optional FD72 tables 25cd0..26510
    # have c15=0: no command. Those unported selections were rejected above.
    add(0x25690, 0xfd72, b'\x14' + b[0xb18:0xc16])
    add(0x258c0, 0xfd72, b'\x15' + b[0xa7c:0xb18])
    add(0x25ad0, 0xfd72, b'\x16\x00' + b[0xd14:0xd32] + b[0xd50:0xd6e] + b[0xd8c:0xdaa])
    # country-NV outer skip. TX limits are the actual cfg parser output,
    # independently checked against the preferred cfg, never guessed.
    add(0x267d0, 0xfc2d, b'\x25' + b[0x1819:0x1829])
    add(0x269a0, 0xfc31, b'\x94\x04' + b[0x141c:0x1424])
    packets.extend((0, packet) for packet in cfg_commands((source / 'BT_FW.cfg').read_text(), b))
    return packets


def emit(packets):
    text = ['/* PRIVATE address/calibration. Generated by bluetooth_vendor.py; never commit. */',
            f'#define X9_BT_VENDOR_COMMANDS {len(packets)}u',
            'static const struct x9_bt_vendor_packet {',
            '    uint32_t constructor; uint16_t length; uint8_t bytes[259];',
            '} x9_bt_vendor_packets[] = {']
    for function, packet in packets:
        text.append(f'    {{0x{function:x}u, {len(packet)}u, {{')
        for i in range(0, len(packet), 16):
            text.append('        ' + ','.join(f'0x{x:02x}' for x in packet[i:i + 16]) + ',')
        text.append('    }},')
    if not packets:
        text.append('    {0, 0, {0}},')
    text.append('};')
    return '\n'.join(text) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--src', type=Path, default=SOURCE)
    ap.add_argument('--out', type=Path, default=ROOT / 'x9/kernel/generated/x9_bt_vendor.inc')
    ap.add_argument('--required', action='store_true')
    a = ap.parse_args()
    # Builds must not acquire silent dependencies on mounted Android roots
    # or another checkout. Private input and output both stay repo-local.
    if not a.src.resolve().is_relative_to(ROOT):
        # Documented worktrees share the main checkout's .local symlink.
        a.src.resolve().relative_to((ROOT / '.local').resolve())
    a.out.resolve().relative_to(ROOT)
    present = [(a.src / name).is_file() for name in INPUT_NAMES]
    if all(present):
        packets = construct(a.src)
    elif any(present) or a.required:
        ap.error('complete original vendor ELF/btinit/properties/preferred cfg required')
    else:
        packets = []  # Passive builds only; runtime preflight returns ENOENT.
    text = emit(packets)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    if not a.out.is_file() or a.out.read_text() != text:
        a.out.write_text(text)
    print(f'bluetooth vendor: {len(packets)} ordered commands; original-input port, no device I/O')


if __name__ == '__main__':
    main()
