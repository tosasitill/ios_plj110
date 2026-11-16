#!/usr/bin/env python3
"""Generate private MT6653 board data from the captured production connfem DT.

BT linux_v2/proj/btmtk_proj_sp.c:btmtk_send_connfem_new copies the 3-byte
connfem_sku_pinmap verbatim, unlike gen4m's swapped/padded Wi-Fi PHY tag.
No default SKU, calibration, or device identity is supplied by this tool.
"""
import argparse
import importlib.util
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('wifi_connfem', Path(__file__).with_name('wifi_connfem.py'))
dt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dt)


def connfem_command(text, hw=0):
    root = dt._parse(text)
    by_ph = {}
    for node in dt._walk(root):
        if 'phandle' in node['props']:
            ph = dt._cells(node['props']['phandle'])[0]
            if ph in by_ph:
                raise ValueError('duplicate connfem phandle')
            by_ph[ph] = node
    hwn = dt._child(dt._child(root, 'sku'), f'hw-{hw}')
    props = hwn['props']
    fems = [by_ph[x] for x in dt._cells(props['using-fems'])]
    layouts = [by_ph[x] for x in dt._cells(props['layout'])]
    spdt = [by_ph[x] for x in dt._cells(props.get('layout-spdt', []))]
    if not 1 <= len(fems) <= 8 or not 1 <= len(layouts) <= 8:
        raise ValueError('connfem FEM/layout count exceeds API bounds')

    def cell(p, key, default=None):
        if key not in p and default is not None:
            return default
        v = dt._cells(p[key])
        if len(v) != 1 or not 0 <= v[0] <= 0xffffffff:
            raise ValueError(f'invalid connfem {key}')
        return v[0]

    def flag(p, key):
        # connfem_sku_flag_u8 leaves value unchanged on a missing key. Require
        # the actual seven production fields instead of inheriting that bug.
        v = dt._bytes(p[key])
        if len(v) != 1:
            raise ValueError(f'invalid connfem flag {key}')
        return v[0]

    def pinmap(node):
        m = dt._cells(node['props']['mapping'])
        if len(m) % 3 or len(m) // 3 > 32 or any(not 0 <= x <= 255 for x in m):
            raise ValueError('invalid connfem pin mapping')
        if any(x > 1 for x in m[2::3]):
            raise ValueError('invalid connfem polarity')
        return bytes(m)  # Antsel, FEM pin, polarity; sizeof(pinmap) == 3.

    bt = dt._child(dt._child(hwn, 'bt'), 'flags')['props']
    common = dt._child(dt._child(hwn, 'common'), 'flags')['props']
    out = bytearray.fromhex('02 6f fc 00 00 01 55 00 00 01 00')
    out += bytes([flag(bt, 'efem-mode'), flag(bt, 'rx-mode')])
    out += bytes(flag(common, k) for k in ('fe-ant-cnt', 'fe-conn-dpdt-sp3t',
                     'fe-conn-spdt', 'fe-bt-wf-usage', 'fe-conn-spdt-2'))
    sp = b''.join(pinmap(s) for s in spdt)
    if len(sp) // 3 > 32:
        raise ValueError('too many SPDT pins')
    out += bytes([len(sp) // 3]) + sp + bytes([len(fems)])
    records = []
    for f in fems:
        p = f['props']
        vid, pid = cell(p, 'vid'), cell(p, 'pid')
        pins, logic = dt._cells(p['pins']), dt._cells(p['truth-table'])
        if vid > 0xffff or pid > 0xffff or len(pins) > 6 or len(logic) % 2 or len(logic) // 2 > 32:
            raise ValueError('invalid FEM record')
        if any(not 0 <= x <= 255 for x in pins + logic):
            raise ValueError('invalid FEM pin/logic byte')
        out += struct.pack('<IIBB', (vid << 16) | pid, cell(p, 'flag', 0), len(pins), len(logic) // 2)
        records.append((bytes(pins), bytes(logic)))
    out += b''.join(p for p, _ in records)
    out += b''.join(t for _, t in records)
    out += struct.pack('<IB', cell(props, 'layout-flag', 0), len(layouts))
    mappings = []
    for lay in layouts:
        p = lay['props']
        fem = by_ph[cell(p, 'fem')]
        if fem not in fems:
            raise ValueError('layout references unselected FEM')
        wifi, btpath = cell(p, 'band-path-wifi'), cell(p, 'band-path-bt')
        mapping = pinmap(lay)
        if wifi > 255 or btpath > 255:
            raise ValueError('invalid layout bandpath')
        out += bytes([wifi, btpath, fems.index(fem), len(mapping) // 3])
        mappings.append(mapping)
    out += b''.join(mappings)
    if len(out) > 4096:
        raise ValueError('BT connfem command exceeds H4 buffer')
    struct.pack_into('<H', out, 3, len(out) - 5)
    struct.pack_into('<H', out, 7, len(out) - 9)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--connfem', type=Path, default=ROOT / '.local/x9/wifi-nvram/connfem.dts')
    ap.add_argument('--out', type=Path, default=ROOT / 'x9/kernel/generated/x9_bt_board.inc')
    ap.add_argument('--hw', type=int, default=0)
    ap.add_argument('--required', action='store_true')
    a = ap.parse_args()
    if a.connfem.is_file():
        command = connfem_command(a.connfem.read_text(), a.hw)
    elif a.required:
        ap.error('captured production connfem DT is required')
    else:
        command = b''
    text = ['/* Private captured board data. Generated; never commit. */',
            f'#define X9_BT_CONNFEM_BYTES {len(command)}u',
            'static const uint8_t x9_bt_connfem_command[] = {']
    for at in range(0, len(command), 16):
        text.append('    ' + ','.join(f'0x{x:02x}' for x in command[at:at+16]) + ',')
    text += ['};' if command else '    0,\n};']
    text = '\n'.join(text) + '\n'
    a.out.parent.mkdir(parents=True, exist_ok=True)
    if not a.out.is_file() or a.out.read_text() != text:
        a.out.write_text(text)
    print(f'bluetooth board: {len(command)} B production SKU command; no hardware writes')


if __name__ == '__main__':
    main()
