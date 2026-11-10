#!/usr/bin/env python3
"""Inspect the exact D22/20H392 USB ABI and extract private prelinked inputs.

Read-only analysis, not a kext loader, relocation emitter, or test suite.
The prelink geometry/OSUnserializeXML approach follows UniXNU XnuPrelink
f876e3ee. No 24A446 ABI, Apple executable bytes, or generated ABI dumps belong
in source control. Output must be a fresh directory under .local or x9/out.
"""
import argparse
import base64
from collections import Counter
import hashlib
import json
from pathlib import Path
import plistlib
import struct
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
SHA256 = '2ec297bac95bcba01046fd0148e97e7e5885c05a6c18827f39f534ba375c1221'
ROOTS = ('com.apple.iokit.IOUSBDeviceFamily', 'com.apple.driver.AppleUSBDeviceMux')
# Reviewed against the pinned cache's mod_init_func and instance constructor.
CONTROLLER_VPTR = 0xfffffff006e11210
CONTROLLER_SIZE = 0x1f8
CONTROLLER_SLOTS = 232
IOSERVICE_SLOTS = 168


class MachO:
    def __init__(self, data, at=0):
        self.data = data
        if at < 0 or at + 32 > len(data):
            raise ValueError('Mach-O header outside input')
        magic, cpu, subtype, self.kind, count, size = struct.unpack_from('<6I', data, at)
        if (magic, cpu, subtype) != (0xfeedfacf, 0x100000c, 0):
            raise ValueError('Requires ordinary arm64; not arm64e/24A446')
        end = at + 32 + size
        if end > len(data) or count > size // 8:
            raise ValueError('Invalid load-command extent')
        self.segments, self.sections, self.symtab = [], [], None
        offset = at + 32
        for _ in range(count):
            if offset + 8 > end:
                raise ValueError('Truncated load command')
            command, length = struct.unpack_from('<II', data, offset)
            if length < 8 or length % 8 or offset + length > end:
                raise ValueError('Invalid load command')
            if command == 0x19:
                if length < 72:
                    raise ValueError('Truncated segment')
                name, va, vs, fo, fs, mp, ip, ns, _ = struct.unpack_from('<16s4Q4I', data, offset + 8)
                if length != 72 + ns * 80 or fs > vs or va + vs > 1 << 64:
                    raise ValueError('Invalid segment')
                segment = dict(name=name.rstrip(b'\0').decode(), va=va, size=vs,
                               fileoff=fo, filesize=fs, maxprot=mp, initprot=ip, sections=[])
                for i in range(ns):
                    sn, _, a, n, f, align, ro, nr = struct.unpack_from('<16s16sQQIIII', data, offset + 72 + 80 * i)
                    if n and not va <= a <= a + n <= va + vs:
                        raise ValueError('Section outside segment')
                    section = dict(name=sn.rstrip(b'\0').decode(), va=a, size=n,
                                   fileoff=f, align=align, reloff=ro, nreloc=nr)
                    segment['sections'].append(section)
                    self.sections.append(section)
                self.segments.append(segment)
            elif command == 2:
                if length != 24 or self.symtab is not None:
                    raise ValueError('Invalid/duplicate symbol table')
                self.symtab = struct.unpack_from('<4I', data, offset + 8)
            offset += length
        if offset != end:
            raise ValueError('Load-command size mismatch')

    def read(self, va, size):
        matches = [s['fileoff'] + va - s['va'] for s in self.segments
                   if s['va'] <= va and va + size <= s['va'] + s['filesize']]
        if va < 0 or size < 0 or len(matches) != 1 or matches[0] + size > len(self.data):
            raise ValueError(f'Ambiguous/unbacked VA {va:#x}+{size:#x}')
        return self.data[matches[0]:matches[0] + size]

    def file_offset(self, va):
        matches = [s['fileoff'] + va - s['va'] for s in self.segments
                   if s['va'] <= va < s['va'] + s['filesize']]
        if len(matches) != 1:
            raise ValueError(f'Ambiguous header address {va:#x}')
        return matches[0]

    def symbols(self):
        if self.symtab is None:
            return []
        so, count, st, size = self.symtab
        if so + 16 * count > len(self.data) or st + size > len(self.data):
            raise ValueError('Symbol table outside input')
        strings = self.data[st:st + size]
        result = []
        for i in range(count):
            si, kind, section, _, address = struct.unpack_from('<IBBHQ', self.data, so + 16 * i)
            end = strings.find(b'\0', si)
            if si >= size or end < 0:
                raise ValueError('Invalid symbol string')
            result.append((strings[si:end].decode(), address, kind, section))
        return result

    def exports(self):
        result = {}
        for name, address, kind, _ in self.symbols():
            if not kind & 0xe0 and kind & 1 and kind & 0x0e:
                result.setdefault(address, []).append(name)
        return result


def prelink_xml(blob):
    refs = {}

    def decode(e):
        if 'IDREF' in e.attrib:
            return refs[e.attrib['IDREF']]
        if e.tag == 'dict':
            children = list(e)
            if len(children) % 2:
                raise ValueError('Odd prelink dictionary')
            value = {}
            for i in range(0, len(children), 2):
                key = decode(children[i])
                if not isinstance(key, str) or key in value:
                    raise ValueError('Invalid/duplicate prelink key')
                value[key] = decode(children[i + 1])
        elif e.tag == 'array':
            value = [decode(c) for c in e]
        elif e.tag in ('key', 'string'):
            value = e.text or ''
        elif e.tag == 'integer':
            value = int(e.text or '0', 0)
        elif e.tag == 'data':
            value = base64.b64decode(''.join((e.text or '').split()), validate=True)
        elif e.tag in ('true', 'false'):
            value = e.tag == 'true'
        else:
            raise ValueError(f'Unsupported prelink element {e.tag}')
        if 'ID' in e.attrib:
            if e.attrib['ID'] in refs:
                raise ValueError('Duplicate prelink ID')
            refs[e.attrib['ID']] = value
        return value

    return decode(ET.fromstring(blob.split(b'\0', 1)[0]))


def object_vtable(path, symbol):
    obj = MachO(path.read_bytes())
    if obj.kind != 1:
        raise ValueError('ABI comparison requires a frozen MH_OBJECT, not a built boot/kernel image')
    symbols = obj.symbols()
    starts = [address + 16 for name, address, _, _ in symbols if name == symbol]
    if len(starts) != 1:
        raise ValueError('Missing/ambiguous frozen vtable')
    relocations = {}
    for s in obj.sections:
        if s['reloff'] + 8 * s['nreloc'] > len(obj.data):
            raise ValueError('Relocations outside object')
        for i in range(s['nreloc']):
            offset, flags = struct.unpack_from('<iI', obj.data, s['reloff'] + 8 * i)
            if flags >> 27 & 1 and not flags >> 24 & 1 and flags >> 25 & 3 == 3 and flags >> 28 == 0:
                index = flags & 0xffffff
                if index >= len(symbols) or offset < 0 or offset + 8 > s['size']:
                    raise ValueError('Invalid pointer relocation')
                relocations[s['va'] + offset] = symbols[index][0]
    result = []
    while starts[0] + 8 * len(result) in relocations:
        result.append(relocations[starts[0] + 8 * len(result)])
    if not result:
        raise ValueError('Cannot recover frozen vtable relocation names')
    return result


def analyze(data, native_object, objects):
    cache = MachO(data)
    if cache.kind != 2:
        raise ValueError('Expected an MH_EXECUTE kernelcache')
    info = next(s for s in cache.segments if s['name'] == '__PRELINK_INFO')
    metadata = prelink_xml(cache.read(info['va'], info['filesize']))
    entries = {}
    for e in metadata['_PrelinkInfoDictionary']:
        name = e['CFBundleIdentifier']
        if name in entries:
            raise ValueError('Duplicate bundle identifier')
        entries[name] = e
    pending, selected, interfaces = list(ROOTS), set(), set()
    while pending:
        name = pending.pop()
        if name in selected or name in interfaces:
            continue
        e = entries[name]
        if e.get('OSKernelResource') is True:
            interfaces.add(name)
        else:
            selected.add(name)
            pending.extend(e.get('OSBundleLibraries', {}))
    modules, ownership = {}, []
    for name, e in entries.items():
        if e.get('OSKernelResource') or not e.get('_PrelinkExecutableSize'):
            continue
        module = MachO(data, cache.file_offset(e['_PrelinkExecutableLoadAddr']))
        if module.kind != 11:
            raise ValueError('Expected prelinked MH_KEXT_BUNDLE')
        modules[name] = module
        for s in module.segments:
            if s['size'] and s['name'] != '__LINKEDIT':
                ownership.append((s['va'], s['va'] + s['size'], name))
    ownership.sort()
    if any(a[1] > b[0] for a, b in zip(ownership, ownership[1:])):
        raise ValueError('Overlapping component ownership')

    def owner(a):
        # Kept independent from assumed segment/file offsets of embedded kexts.
        return next((name for low, high, name in ownership if low <= a < high), None)

    exports = cache.exports()
    packed = metadata['_PrelinkLinkKASLROffsets']
    count, = struct.unpack_from('<I', packed)
    if len(packed) != 4 + 4 * count:
        raise ValueError('Invalid prelink pointer table')
    offsets = struct.unpack_from(f'<{count}I', packed, 4)
    if len(set(offsets)) != count:
        raise ValueError('Duplicate prelink pointer site')
    base = next(s['va'] for s in cache.segments if s['name'] == '__PRELINK_TEXT')
    bindings, counts, unresolved = [], Counter(), []
    for offset in offsets:
        site = base + offset
        source = owner(site)
        if source not in selected:
            continue
        if owner(site + 7) != source:
            raise ValueError('Pointer spans module ownership')
        target, = struct.unpack('<Q', cache.read(site, 8))
        destination = owner(target)
        names = exports.get(target, [])
        if destination in selected:
            kind = 'component'
        elif destination or not names:
            kind = 'unresolved'
            unresolved.append(dict(site=site, source=source, target=target, destination=destination))
        else:
            kind = 'kernel'
        counts[kind] += 1
        bindings.append(dict(site=site, source=source, target=target, kind=kind, kernel_symbols=names))
    kernel_targets = {b['target']: b['kernel_symbols'] for b in bindings if b['kind'] == 'kernel'}
    available = set()
    for path in objects:
        obj = MachO(path.read_bytes())
        if obj.kind != 1:
            raise ValueError('Import comparison accepts frozen objects only')
        available.update(n for n, _, t, _ in obj.symbols() if not t & 0xe0 and t & 1 and t & 0x0e)
    missing = [{'address': a, 'aliases': names} for a, names in kernel_targets.items()
               if objects and not set(names) & available]
    native_slots = object_vtable(native_object, '__ZTV9IOService')
    stock_vptr = next(a for a, names in exports.items() if '__ZTV9IOService' in names) + 16
    differences = []
    for i in range(max(IOSERVICE_SLOTS, len(native_slots))):
        names = exports.get(struct.unpack('<Q', cache.read(stock_vptr + 8 * i, 8))[0], []) if i < IOSERVICE_SLOTS else []
        native = native_slots[i] if i < len(native_slots) else None
        if native not in names:
            differences.append(dict(slot=i, stock_symbols=names, frozen_symbol=native))
    vtable = [dict(slot=i, target=struct.unpack('<Q', cache.read(CONTROLLER_VPTR + i * 8, 8))[0])
              for i in range(CONTROLLER_SLOTS)]
    pure = [row['slot'] for row in vtable if '___cxa_pure_virtual' in exports.get(row['target'], [])]
    result = dict(kernelcache_sha256=SHA256, build='20H392', identity='d22ap',
                  components=sorted(selected), kernel_interfaces=sorted(interfaces),
                  pointer_counts=dict(counts), kernel_targets=len(kernel_targets),
                  kernel_imports=kernel_targets, unresolved=unresolved, missing_frozen_imports=missing,
                  original_controller=dict(instance_size=CONTROLLER_SIZE, vptr=CONTROLLER_VPTR,
                                           slots=CONTROLLER_SLOTS, pure_virtual_slots=pure, vtable=vtable),
                  ioservice=dict(stock_slots=IOSERVICE_SLOTS, frozen_slots=len(native_slots),
                                 differences=differences),
                  abi_compatible=False, relocated=False, runtime_loadable=False)
    return result, cache, entries, modules


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernelcache', type=Path, required=True)
    ap.add_argument('--build-manifest', type=Path, required=True)
    ap.add_argument('--io-service-object', type=Path, required=True)
    ap.add_argument('--objects-file', type=Path, help='one frozen MH_OBJECT path per line; optional import-name comparison')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--extract', action='store_true', help='also save selected prelinked segments (NOT standalone kexts)')
    ap.add_argument('--require-compatible', action='store_true', help='exit 2 if runtime ABI/load readiness is not established')
    a = ap.parse_args()
    output = a.out.resolve()
    if not any(output.is_relative_to(p.resolve()) and output != p.resolve() for p in (ROOT / '.local', ROOT / 'x9/out')):
        ap.error('Private Apple/ABI outputs must be under .local or x9/out')
    if output.exists():
        ap.error('Use a fresh output directory; previous evidence is not overwritten')
    data = a.kernelcache.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        ap.error('Wrong cache: requires the reviewed ordinary arm64 D22/20H392 Mach-O')
    manifest = plistlib.loads(a.build_manifest.read_bytes())
    if manifest.get('ProductBuildVersion') != '20H392' or manifest.get('ProductVersion') != '16.7.16':
        ap.error('Wrong BuildManifest')
    identity = next(i for i in manifest['BuildIdentities']
                    if i['Info']['DeviceClass'] == 'd22ap' and i['Info']['RestoreBehavior'] == 'Erase')
    if identity['Manifest']['KernelCache']['Info']['Path'] != 'kernelcache.release.iphone10b':
        ap.error('Unexpected D22 kernel member')
    objects = [Path(line) for line in a.objects_file.read_text().splitlines() if line] if a.objects_file else []
    result, cache, entries, modules = analyze(data, a.io_service_object, objects)
    output.mkdir(parents=True)
    (output / 'abi.json').write_text(json.dumps(result, indent=2) + '\n')
    (output / 'bindings.json').write_text(json.dumps(result['kernel_imports'], indent=2) + '\n')
    for name in result['components']:
        folder = output / name
        folder.mkdir()
        (folder / 'metadata.json').write_text(json.dumps(entries[name], indent=2) + '\n')
        module = modules.get(name)
        if module:
            (folder / 'segments.json').write_text(json.dumps(module.segments, indent=2) + '\n')
            if a.extract:
                for s in module.segments:
                    if s['filesize']:
                        (folder / (s['name'] + '.bin')).write_bytes(cache.read(s['va'], s['filesize']))
    print(f"20H392: {len(result['components'])} USB components, {result['kernel_targets']} kernel target addresses; unresolved={len(result['unresolved'])}")
    if objects:
        print(f"Frozen import-name comparison: missing={len(result['missing_frozen_imports'])}; aliases alone do not qualify ABI")
    print(f"IOService slots: stock={IOSERVICE_SLOTS}, frozen={result['ioservice']['frozen_slots']}; NOT runtime-loadable")
    return 2 if a.require_compatible else 0


if __name__ == '__main__':
    raise SystemExit(main())
