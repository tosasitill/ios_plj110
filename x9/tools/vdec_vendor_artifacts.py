#!/usr/bin/env python3
"""Inspect an explicit local MT6993 C2 decoder ELF, without loading it.

Extract bounded .gnu_debugdata and capture the observed conversion tables only
when build-id AND instruction bytes match. ELF VA, Android format codes and
RELR addends are not kernel DMA addresses, loaded VSI ABI or runtime capability.
This does not implement an Android ELF loader or decode/convert any pixels.
Writes a NEW private directory. Python stdlib only; never follows input symlinks.
"""
import argparse
import hashlib
import json
import lzma
import os
import stat
import struct
from pathlib import Path

MAX_INPUT = 16 << 20
MAX_DEBUG = 32 << 20


def span(blob, at, size):
    if at < 0 or size < 0 or at > len(blob) or size > len(blob) - at:
        raise ValueError('range outside artifact')
    return blob[at:at + size]


def cstring(blob, at):
    if not 0 <= at < len(blob):
        raise ValueError('string offset outside artifact')
    end = blob.find(b'\0', at)
    if end < 0:
        raise ValueError('unterminated string')
    return blob[at:end].decode('utf-8', errors='strict')


def read_input(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as f:
        info = os.fstat(f.fileno())
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= MAX_INPUT:
            raise ValueError('input must be a regular file <=16 MiB')
        blob = f.read(MAX_INPUT + 1)
        if len(blob) != info.st_size:
            raise ValueError('input size changed during read')
    return blob


class Library:
    def __init__(self, blob):
        self.blob = blob
        head = span(blob, 0, 64)
        if head[:7] != b'\x7fELF\x02\x01\x01':
            raise ValueError('expected ELF64 little-endian library')
        fields = struct.unpack_from('<HHIQQQIHHHHHH', head, 16)
        typ, machine, version, _, _, shoff, _, ehsize, _, _, shsize, count, names = fields
        if (typ, machine, version, ehsize, shsize) != (3, 183, 1, 64, 64):
            raise ValueError('expected AArch64 ET_DYN library')
        if not 0 < names < count <= 512:
            raise ValueError('unsupported ELF section numbering')
        raw = span(blob, shoff, count * 64)
        rows = [struct.unpack_from('<IIQQQQIIQQ', raw, i * 64) for i in range(count)]
        names_row = rows[names]
        if names_row[1] != 3:
            raise ValueError('invalid section name table')
        strings = span(blob, names_row[4], names_row[5])
        self.sections = []
        for row in rows:
            n, kind, flags, va, off, size, link, _, _, entry = row
            if kind != 8:
                span(blob, off, size)
            self.sections.append(dict(name=cstring(strings, n), kind=kind,
                flags=flags, va=va, offset=off, bytes=size, link=link, entry=entry))

    def section(self, name):
        matches = [s for s in self.sections if s['name'] == name]
        if len(matches) > 1:
            raise ValueError('ambiguous section name')
        return matches[0] if matches else None

    def data(self, sec):
        if sec['kind'] == 8:
            raise ValueError('NOBITS is not captured runtime memory')
        return span(self.blob, sec['offset'], sec['bytes'])

    def va_data(self, va, size):
        matches = [s for s in self.sections if s['kind'] != 8 and
                   s['flags'] & 2 and s['va'] <= va and
                   0 <= size <= s['bytes'] - (va - s['va'])]
        if len(matches) != 1:
            raise ValueError('ELF VA is not uniquely file-backed')
        sec = matches[0]
        return span(self.blob, sec['offset'] + va - sec['va'], size)

    def build_id(self):
        sec = self.section('.note.gnu.build-id')
        if sec is None:
            return None
        data, at, ids = self.data(sec), 0, []
        while at < len(data):
            namesz, descsz, kind = struct.unpack('<III', span(data, at, 12))
            at += 12
            name = span(data, at, namesz)
            at += (namesz + 3) & ~3
            desc = span(data, at, descsz)
            at += (descsz + 3) & ~3
            if at > len(data):
                raise ValueError('truncated note padding')
            if name == b'GNU\0' and kind == 3:
                ids.append(desc.hex())
        if len(ids) != 1:
            raise ValueError('missing/ambiguous GNU build-id note')
        return ids[0]

    def relr_locations(self):
        sec = self.section('.relr.dyn')
        if sec is None:
            return set()
        if sec['kind'] != 19 or sec['entry'] != 8 or sec['bytes'] % 8:
            raise ValueError('unsupported RELR encoding')
        locations, base = set(), None
        data = self.data(sec)
        for at in range(0, len(data), 8):
            value = struct.unpack_from('<Q', data, at)[0]
            if value & 1:
                if base is None:
                    raise ValueError('RELR bitmap precedes base')
                targets = [base + i * 8 for i in range(63) if value & (1 << (i + 1))]
                base += 63 * 8
            else:
                if value % 8:
                    raise ValueError('unaligned RELR location')
                targets, base = [value], value + 8
            for target in targets:
                if target >= 1 << 64 or target in locations:
                    raise ValueError('overflow/duplicate RELR location')
                locations.add(target)
            if len(locations) > 262144:
                raise ValueError('too many RELR locations')
        return locations


def conversion_evidence(lib):
    result = dict(profile_match=False, loaded_module_pairing_proven=False,
                  runtime_formats_proven=False, linear_pixels_proven=False,
                  full_elf_relocation_implemented=False)
    if (len(lib.blob) != 1779056 or
            lib.build_id() != '72c92143216bb71c06d8222be4eba204'):
        return result
    # These constructor copies and consumer offsets bind records to executable
    # code. A library name, XML capability or build-id alone is not enough.
    checks = (
        (0xc5f0c, '1f2003d501a66e1002bc8052e08e01f8d51600f9a0550394'),
        (0xc5f34, '1f2003d5c1d36e1002b88052c01a00f9db2200f996550394'),
        (0xc5f54, '77da13a9'),
        (0xc7904, '560540b9'),
        (0xc7b58, '090d40f9a8c300d1e103182a20013fd6'),
        (0xcacc0,
         '093c00112a7c0153296d1c124c0080522b7d011b2a7d0a1b1f250129093102290b290029c0035fd6'),
        (0xcaea0,
         '097c00114c00805229691b122a791f53297d011b2b791f531f2901290a3102290b250029c0035fd6'),
        (0xcaf10,
         '09fc00112afc001129651a124a4d1a124a7d091b2905090b297d01534b7d0653'
         '4c7d07536bfd3f118cfd00116b3914128c4d1a126b010a0b1f2501298b010b0'
         'b6b013f116b4d14126a054a0b0bfaffb0600d44fd0a7d0029000900fdc0035fd6'),
        (0xcaf70,
         '09fc00112afc001129651a124a4d1a124a7d091b290d090b297d02534b7d0653'
         '4c7d07534a050a0b6bfd3f114a791f538cfd00116b3914128c4d1a126b094a0b'
         '1f2501296b010c0b6bfd3f116b4914126a0d4a0b0bfaffb0600d44fd0a7d0029'
         '000900fdc0035fd6'),
    )
    instructions = []
    for va, raw in checks:
        expected = bytes.fromhex(raw)
        if lib.va_data(va, len(expected)) != expected:
            return result
        instructions.append(dict(elf_va=va, raw_hex=raw))
    relr = lib.relr_locations()
    tables = []
    text, rodata = lib.section('.text'), lib.section('.rodata')
    if text is None or rodata is None:
        return result
    for base, count in ((0x1a33d0, 47), (0x1a39b0, 46)):
        records = []
        for i in range(count):
            va = base + i * 32
            data = lib.va_data(va, 32)
            pixel, backend, word8, word12, name, callback = struct.unpack('<4I2Q', data)
            if (va + 16 not in relr or va + 24 not in relr or
                    not rodata['va'] <= name < rodata['va'] + rodata['bytes'] or
                    not text['va'] <= callback < text['va'] + text['bytes']):
                return result
            # Only RELR-covered in-file addends, no runtime pointer arithmetic.
            text_name = cstring(lib.va_data(name, min(256,
                rodata['va'] + rodata['bytes'] - name)), 0)
            records.append(dict(elf_va=va, raw_hex=data.hex(), pixel_format_code=pixel,
                conversion_format_word=backend, opaque_words_8_12=[word8, word12],
                name=text_name, name_elf_va=name, layout_callback_elf_va=callback,
                pointer_slots_have_relr=True))
        required = {
            'GM_PIXEL_FORMAT_NV12_HYFBC': (0x7f000010, 0xcaf10),
            'GM_PIXEL_FORMAT_P010_HYFBC': (0x7f000030, 0xcaf70),
            'V4L2_PIX_FMT_NV12': (0x3231564e, 0xcacc0),
        }
        if base == 0x1a33d0:
            required['V4L2_PIX_FMT_P010S'] = (0x53313050, 0xcaea0)
        for name, expected in required.items():
            matches = [r for r in records if r['name'] == name]
            if len(matches) != 1 or (matches[0]['pixel_format_code'],
                    matches[0]['layout_callback_elf_va']) != expected:
                return result
        tables.append(dict(elf_va=base, record_bytes=32, records=records))
    result.update(profile_match=True, profile='observed MT6993 C2 conversion tables',
        instruction_evidence=instructions, tables=tables,
        layout_callbacks=dict(nv12=0xcacc0, ycbcr_p010=0xcaea0,
                              nv12_hyfbc=0xcaf10, p010_hyfbc=0xcaf70),
        callback_semantics='buffer layout calculation, NOT decompression',
        other_relocations='Android packed RELA and PLT remain uninterpreted')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--c2', type=Path, required=True)
    parser.add_argument('--provenance', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    blob = read_input(args.c2)
    lib = Library(blob)
    result = dict(schema=1, provenance=args.provenance,
        source=str(args.c2.absolute()), bytes=len(blob),
        sha256=hashlib.sha256(blob).hexdigest(), build_id=lib.build_id(),
        conversion_evidence=conversion_evidence(lib),
        sections=[dict(name=s['name'], elf_va=s['va'], bytes=s['bytes'], kind=s['kind'])
                  for s in lib.sections])
    debug_sec = lib.section('.gnu_debugdata')
    mini = None
    if debug_sec is not None:
        decoder = lzma.LZMADecompressor(memlimit=64 << 20)
        mini = decoder.decompress(lib.data(debug_sec), max_length=MAX_DEBUG + 1)
        if not mini or len(mini) > MAX_DEBUG or not decoder.eof or decoder.unused_data:
            raise ValueError('invalid/oversized minidebug')
        if span(mini, 0, 7) != b'\x7fELF\x02\x01\x01':
            raise ValueError('minidebug is not ELF64 LE')
        result['minidebug'] = dict(local='c2.minidebug.elf', bytes=len(mini))
    # Parse fully before creating output; never overwrite a previous capture.
    args.out.mkdir(mode=0o700, exist_ok=False)
    if mini is not None:
        with (args.out / 'c2.minidebug.elf').open('xb') as f:
            f.write(mini)
    with (args.out / 'artifacts.json').open('x', encoding='utf-8') as f:
        json.dump(result, f, indent=2, ensure_ascii=True)
        f.write('\n')
    print(json.dumps(dict(output=str(args.out.absolute()),
        profile_match=result['conversion_evidence']['profile_match'],
        runtime_formats_proven=False, linear_pixels_proven=False)))


if __name__ == '__main__':
    main()
