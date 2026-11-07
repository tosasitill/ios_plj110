#!/usr/bin/env python3
"""Encode the device's connfem SKU (external Wi-Fi front-end modules) as gen4m's PHY_ACTION COM_FEM tag.

    from wifi_connfem import sku_tag; tag = sku_tag(Path('.local/x9/wifi-nvram/connfem.dts').read_text())

Input: the decompiled live-DT `connfem` node (captured from the device, kept in .local, never committed).
Production connfem (connfem_dt_parser.c cfm_dt_sku_parse, not connfem_is_internal) reads the `sku` node,
not `sku-mtk`; it has one hw entry here. Output bytes follow gen4m chips/common/pre_cal.c _AddConnfemSkuTag
(CONNFEM_API_VERSION 2): FEM header + layout id + PHYACT_FEM_INFOS_T (count word + 80 B per FEM) +
PHYACT_LAYOUT_USAGES_T (8 B + 28 B per layout), WF header + PHYACT_STATE_USAGES_T (4 B + 100 B per FEM),
SPDT V2 (106 B). For 2 FEMs and 4 layouts that is 606 B, the size Android sent (TX CMD 0x08 LEN 682).
"""
import re
import struct


def _parse(text):
    """Tiny DTS parser: returns the root node {name, label, props, children}."""
    tokens = re.findall(r'"[^"]*"|<[^>]*>|\[[^\]]*\]|[{};=,]|[^\s{};=,]+', text)
    pos = 0

    def node(name, label):
        nonlocal pos
        n = {'name': name, 'label': label, 'props': {}, 'children': []}
        while pos < len(tokens):
            t = tokens[pos]
            if t == '}':
                pos += 2  # '}' ';'
                return n
            if t.endswith(':'):          # label for the next node
                lab = t[:-1]
                nm = tokens[pos + 1]
                pos += 3                 # label name {
                n['children'].append(node(nm, lab))
                continue
            if tokens[pos + 1] == '{':
                pos += 2
                n['children'].append(node(t, None))
                continue
            if tokens[pos + 1] == ';':   # boolean property
                n['props'][t] = True
                pos += 2
                continue
            assert tokens[pos + 1] == '=', (t, tokens[pos + 1])
            pos += 2
            vals = []
            while tokens[pos] != ';':
                if tokens[pos] != ',':
                    vals.append(tokens[pos])
                pos += 1
            pos += 1
            n['props'][t] = vals
        return n

    # first token sequence: "connfem: connfem {"
    if tokens[0].endswith(':'):
        pos = 3
        return node(tokens[1], tokens[0][:-1])
    pos = 2
    return node(tokens[0], None)


def _cells(v):
    out = []
    for x in v:
        if x.startswith('<'):
            out += [int(c, 0) for c in x[1:-1].split()]
    return out


def _bytes(v):
    return [int(c, 16) for c in v[0][1:-1].split()]


def _strings(v):
    return [x[1:-1] for x in v]


def _walk(n):
    yield n
    for c in n['children']:
        yield from _walk(c)


def _child(n, name):
    for c in n['children']:
        if c['name'] == name:
            return c
    raise KeyError(name)


def sku_tag(text, sku_node='sku', hw=0):
    root = _parse(text)
    by_ph = {}
    for n in _walk(root):
        if 'phandle' in n['props']:
            by_ph[_cells(n['props']['phandle'])[0]] = n
    hwn = _child(_child(root, sku_node), f'hw-{hw}')
    p = hwn['props']
    fems = [by_ph[x] for x in _cells(p['using-fems'])]
    layouts = [by_ph[x] for x in _cells(p['layout'])]
    spdt = [by_ph[x] for x in _cells(p.get('layout-spdt', []))]
    flags = _child(_child(hwn, 'common'), 'flags')['props']
    states = [by_ph[x] for x in _cells(_child(hwn, 'wifi')['props']['using-states'])]
    out = bytearray()
    # ---- FEM section
    hdr = len(out); out += struct.pack('<BBH', 1, 2, 0)
    start = len(out)
    out += b'\0' * 4                                  # layout id (unused)
    out += struct.pack('<B3x', len(fems))
    for f in fems:
        fp = f['props']
        vid, pid = _cells(fp['vid'])[0], _cells(fp['pid'])[0]
        flag = _cells(fp['flag'])[0] if 'flag' in fp else 0
        pins = _cells(fp['pins'])
        tt = _cells(fp['truth-table'])
        assert len(pins) <= 6 and len(tt) % 2 == 0 and len(tt) // 2 <= 32
        rec = struct.pack('<IIB', (vid << 16) | pid, flag, len(pins)) + bytes(pins).ljust(6, b'\0')
        rec += struct.pack('<B', len(tt) // 2) + bytes(tt).ljust(64, b'\0')
        assert len(rec) == 80
        out += rec
    out += struct.pack('<B3xI', len(layouts), _cells(p.get('layout-flag', ['<0>']))[0])
    names = [f['name'] for f in fems]
    for lay in layouts:
        lp = lay['props']
        fem = by_ph[_cells(lp['fem'])[0]]['name']
        m = _cells(lp['mapping'])
        assert len(m) % 3 == 0 and len(m) // 3 <= 6
        rec = struct.pack('<BBBB', _cells(lp['band-path-wifi'])[0], _cells(lp['band-path-bt'])[0],
                          names.index(fem), len(m) // 3)
        for i in range(0, len(m), 3):                 # mapping = antsel(pin1), fem pin(pin2), polarity
            rec += struct.pack('<BBBx', m[i + 1], m[i], m[i + 2])
        out += rec.ljust(28, b'\0')
    struct.pack_into('<H', out, hdr + 2, len(out) - start)
    # ---- WF state usage section
    hdr = len(out); out += struct.pack('<BBH', 3, 2, 0)
    start = len(out)
    out += struct.pack('<B3x', len(fems))
    for i, f in enumerate(fems):
        mine = [s for s in states if by_ph[_cells(s['props']['fem'])[0]] is f]
        cats = []
        for s in mine:
            for cn in _strings(s['props']['cat-names']):
                c = _child(s, cn)
                cats.append((_cells(c['props']['cat-id'])[0], _cells(c['props']['states'])))
        assert len(cats) <= 8
        rec = struct.pack('<BB2x', i, len(cats))
        for cid, ops in cats:
            assert len(ops) <= 10
            rec += struct.pack('<BB', cid, len(ops)) + bytes(ops).ljust(10, b'\0')
        out += rec.ljust(100, b'\0')
    struct.pack_into('<H', out, hdr + 2, len(out) - start)
    # ---- SPDT V2
    def flag(name):
        return _bytes(flags[name])[0] if name in flags else 0
    sp = struct.pack('<BBH', 2, 2, 106 - 4)
    sp += bytes([flag('fe-ant-cnt'), flag('fe-conn-dpdt-sp3t'), flag('fe-conn-spdt'), flag('fe-bt-wf-usage'),
                 flag('fe-conn-spdt-2')])
    pins = []
    for s in spdt:
        m = _cells(s['props']['mapping'])
        pins += [(m[i], m[i + 1], m[i + 2]) for i in range(0, len(m), 3)]
    assert len(pins) <= 32
    sp += bytes([len(pins)])
    for a, b, c in pins:                              # u1AntSelNo = pin1, u1FemPin = pin2, polarity
        sp += bytes([a, b, c])
    out += sp.ljust(106, b'\0')
    return bytes(out)


if __name__ == '__main__':
    import sys
    from pathlib import Path
    t = sku_tag(Path(sys.argv[1]).read_text())
    print(len(t), t.hex())
