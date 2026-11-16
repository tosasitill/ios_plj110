#!/usr/bin/env python3
"""Narrow, read-only CMPhoto 20H392 alpha evidence reader.

--case consumes retained qualification/objdump/key/stub records WITHOUT reading
Apple images again. --group replays only one selected text group against the
already-mounted root clone. Neither mode mounts anything, uses ADB, executes
target code, verifies built artifacts, or enables auxiliary-alpha decoding.
Names of the two runAsyncDecodeRequest functions are deliberately not unique.
Outputs must be new files in this checkout's gitignored x9/out.
"""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
IMAGE = '.local/apple/dsc-20H392/System/Library/PrivateFrameworks/CMPhoto.framework/CMPhoto'
UUID = '8c636e49-40af-38ac-87e6-4943a376eed2'
CACHE = Path('/private/tmp/x9root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld')
GROUPS = {
    'helper': ('qualified-alpha-original.json', 'selected-alpha.asm', (
        (0x1A5DA787C, 968, '_CMPhotoCreateImageFromAlphaAndMainBuffer'),
        (0x1A5DC04E8, 168, '_CMPhotoGetAuxiliaryImageTypeFromURN'),
        (0x1A5DDD77C, 380, '_CMPhotoDecompressionContainerCreateAlphaDecodeOptions'),
        (0x1A5DDD8F8, 140, '_CMPhotoDecompressionContainerGetDefaultFormatWhenDecodingAlpha'),
    )),
    'caller': ('qualified-alpha-callers.json', 'alpha-caller.asm', (
        (0x1A5D6D990, 212, '__runAsyncDecodeRequest'),
        (0x1A5D911D0, 392, '__findAlphaForImage'),
        (0x1A5DC9850, 820, '__runAsyncDecodeRequest'),
    )),
    'subtype': ('qualified-alpha-subtype.json', 'alpha-subtype.asm', (
        (0x1A5DCCDE4, 332, '__readAlphaInfoSEIPayload'),
    )),
}
STUBS = {
    0x1A9450324: (0x191611248, '_FigCFEqual'),
    0x1A94502DC: (0x191612304, '_FigCFDictionarySetInt'),
    0x1A9451314: (0x18C104618, '_vImageOverwriteChannels_ARGB8888'),
    0x1A9451308: (0x18C10465C, '_vImageOverwriteChannels_ARGB16U'),
    0x1A9450210: (0x191612394, '_FigCFArrayGetInt32AtIndex'),
    0x1A9450240: (0x191611270, '_FigCFDictionaryCreateMutableCopy'),
    0x1A944F664: (0x186E73A40, '_CFDictionaryRemoveValue'),
    0x1A94500C0: (0x1A1EBD5D8, '_CVPixelBufferLockBaseAddress'),
    0x1A94500FC: (0x1A1EAD308, '_CVPixelBufferUnlockBaseAddress'),
}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def read_exact(path, offset, size):
    require(0 <= offset and 0 < size <= 1024 * 1024, 'read outside narrow bound')
    with path.open('rb') as source:
        source.seek(offset)
        data = source.read(size)
    require(len(data) == size, 'truncated original range')
    return data


def image_info(raw):
    require(len(raw) >= 32, 'short Mach-O header')
    magic, _, _, _, count, command_bytes, _, _ = struct.unpack_from('<8I', raw)
    require(magic == 0xFEEDFACF and 0 < count <= 256, 'unsupported Mach-O header')
    require(32 + command_bytes <= len(raw), 'truncated Mach-O commands')
    cursor, identities, segments = 32, [], []
    for _ in range(count):
        require(cursor + 8 <= 32 + command_bytes, 'short load command')
        command, size = struct.unpack_from('<II', raw, cursor)
        require(size >= 8 and cursor + size <= 32 + command_bytes, 'bad load command size')
        if command == 0x19:
            require(size >= 72, 'short segment command')
            name, va, vsize, offset, fsize = struct.unpack_from('<16s4Q', raw, cursor + 8)
            segments.append((name.rstrip(b'\0').decode('ascii'), va, vsize, offset, fsize))
        elif command == 0x1B:
            require(size == 24, 'bad UUID command')
            identities.append(str(uuid.UUID(bytes=raw[cursor + 8:cursor + 24])))
        cursor += size
    require(cursor == 32 + command_bytes and len(identities) == 1, 'ambiguous Mach-O identity')
    return identities[0], segments


def direct_calls(data, start):
    calls = []
    for offset in range(0, len(data), 4):
        word = struct.unpack_from('<I', data, offset)[0]
        if word & 0xFC000000 == 0x94000000:
            immediate = word & 0x3FFFFFF
            if immediate & 0x2000000:
                immediate -= 0x4000000
            calls.append({'call': hex(start + offset),
                          'target': hex(start + offset + immediate * 4)})
    return calls


def replay(group, cache_directory):
    """Requalify the requested fixed group, not a whole-image or build hash."""
    image = ROOT / IMAGE
    identity, segments = image_info(read_exact(image, 0, 16384))
    require(identity == UUID, 'wrong CMPhoto version; do not reuse addresses')
    mappings = []
    for path in sorted(cache_directory.glob('dyld_shared_cache_arm64*')):
        if path.name.endswith('.symbols') or not path.is_file():
            continue
        header = read_exact(path, 0, 32)
        if header[:16].rstrip(b' \0') != b'dyld_v1   arm64':
            continue
        offset, count = struct.unpack_from('<II', header, 16)
        require(0 < count <= 64, 'unsupported dyld mapping count')
        table = read_exact(path, offset, count * 32)
        for index in range(count):
            va, size, file_offset, _, _ = struct.unpack_from('<QQQII', table, index * 32)
            mappings.append((path, va, size, file_offset))

    def cache_bytes(va, size):
        matches = [m for m in mappings if m[1] <= va and va + size <= m[1] + m[2]]
        require(len(matches) == 1, 'missing/ambiguous retained cache mapping')
        path, base, _, file_offset = matches[0]
        offset = file_offset + va - base
        return read_exact(path, offset, size), {'path': str(path), 'offset': hex(offset)}

    texts = [s for s in segments if s[0] == '__TEXT']
    require(len(texts) == 1, 'ambiguous extracted __TEXT')
    cache_header, header_map = cache_bytes(texts[0][1], 16384)
    cache_identity, _ = image_info(cache_header)
    require(cache_identity == UUID, 'retained cache UUID mismatch')
    ranges = []
    for va, size, name in GROUPS[group][2]:
        matches = [s for s in segments if s[0] == '__TEXT' and s[1] <= va and va + size <= s[1] + s[4]]
        require(len(matches) == 1, 'selected range outside extracted __TEXT')
        segment = matches[0]
        offset = segment[3] + va - segment[1]
        data = read_exact(image, offset, size)
        original, where = cache_bytes(va, size)
        require(data == original, 'extracted and retained original text differ')
        ranges.append({'symbol': name, 'va': hex(va), 'size': size, 'same': True,
                       'extracted_offset': hex(offset), 'cache': where,
                       'raw': original.hex(), 'direct_bl': direct_calls(original, va)})
    return {'mode': 'new_original_qualification', 'group': group, 'uuid': identity,
            'cache_uuid': cache_identity, 'image': IMAGE, 'header_map': header_map,
            'ranges': ranges, 'limit': 'fixed selected text only; no new key/stub or runtime evidence'}


def load(case, filename):
    return json.loads((case / filename).read_text())


def saved(case):
    """Consume prior observations; do not present them as new original reads."""
    ranges, proofs = [], []
    for _, (proof_file, assembly, selected) in GROUPS.items():
        proof = load(case, proof_file)
        images = proof['images']
        require(len(images) == 1, 'ambiguous saved image')
        image = images[0]
        require(image['uuid'] == UUID and image['cache_uuid'] == UUID and image['image'] == IMAGE,
                'saved qualification identity differs')
        expected = {(hex(va), size, name) for va, size, name in selected}
        actual = {(r['va'], r['size'], r['symbol']) for r in image['ranges']}
        require(actual == expected and len(actual) == len(image['ranges']), 'saved coverage differs')
        asm = (case / assembly).read_text()
        words = {int(a, 16): int(w, 16) for a, w in re.findall(
            r'^([0-9a-f]+):\s+([0-9a-f]{8})\s', asm, re.M)}
        for row in image['ranges']:
            require(row['same'] is True and row['asm'] == assembly, 'saved qualification failed')
            va, size = int(row['va'], 16), row['size']
            require(all(at in words for at in range(va, va + size, 4)), 'incomplete saved disassembly')
            raw = b''.join(struct.pack('<I', words[at]) for at in range(va, va + size, 4))
            ranges.append({**row, 'raw_from_saved_objdump': raw.hex(),
                           'direct_bl': direct_calls(raw, va)})
        proofs.append({'file': proof_file, 'range_count': len(selected),
                       'saved_assertion': 'all selected extracted text equalled retained cache text'})

    cells = load(case, 'alpha-direct-cells.json')
    imports = load(case, 'alpha-import-cells.json')
    keys = load(case, 'resolved-alpha-keys-final.json')
    require(len(cells) == 7 and len(imports) == 5 and len(keys) == 12, 'saved key coverage differs')
    raw_objects = {}
    for record in cells + imports:
        hops = [record['pointer']] if 'pointer' in record else record['hops']
        require(len(hops) == (1 if 'pointer' in record else 2), 'unexpected pointer depth')
        for index, hop in enumerate(hops):
            target = int(hop['raw'], 16) & ~int(hop['delta_mask'], 16)
            if target:
                target += int(hop['value_add'], 16)
            require(target == int(hop['target'], 16), 'saved slide-v2 pointer mismatch')
            if index + 1 < len(hops):
                require(hop['target'] == hops[index + 1]['slot'], 'broken saved import chain')
        raw_objects[hops[0]['slot']] = (hops[-1]['target'], record['target_raw'])
    require(len(raw_objects) == 12, 'duplicate saved key slot')
    for key in keys:
        require(raw_objects.get(key['slot']) == (key['cf'], key['raw']), 'saved CF object differs')
        _, flags, _, length = struct.unpack('<4Q', bytes.fromhex(key['raw']))
        if 'text' in key:
            require(flags == 0x7C8 and length == len(key['text'].encode('ascii')) == key['length'],
                    'saved CFString length/flags differ')
        else:
            require(key['uninterpreted_object'] is True, 'nonstring object interpreted as string')

    stubs = load(case, 'resolved-alpha-stubs-final.json')
    require({int(s['stub'], 16) for s in stubs} == set(STUBS) and len(stubs) == 9,
            'saved stub coverage differs')
    for stub in stubs:
        va = int(stub['stub'], 16)
        a, d, branch = struct.unpack('<3I', bytes.fromhex(stub['bytes']))
        require(a & 0x9F00001F == 0x90000010 and d & 0xFFC003FF == 0x91000210
                and branch == 0xD61F0200, 'not a selected ADRP/ADD/BR stub')
        immediate = ((a >> 29) & 3) | (((a >> 5) & 0x7FFFF) << 2)
        if immediate & (1 << 20):
            immediate -= 1 << 21
        target = (va & ~4095) + (immediate << 12) + ((d >> 10) & 4095)
        require(target == int(stub['target'], 16) == STUBS[va][0], 'saved stub target differs')
        require(STUBS[va][1] in {s['symbol'] for s in stub['exact_symbols']}, 'saved stub name differs')
    return {'mode': 'saved_evidence_consumption_only', 'case': str(case), 'uuid': UUID,
            'qualification_files': proofs, 'range_count': len(ranges), 'ranges': ranges,
            'direct_key_count': len(cells), 'imported_key_count': len(imports), 'keys': keys,
            'key_pointer_records': cells + imports, 'stub_count': len(stubs), 'stubs': stubs,
            'limit': 'saved qualifications and names, not fresh original/runtime/export verification'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--case', type=Path, help='consume saved records only')
    mode.add_argument('--group', choices=GROUPS, help='requalify just this fixed original text group')
    parser.add_argument('--cache', type=Path, default=CACHE, help='already-mounted original cache directory')
    parser.add_argument('--out', required=True, type=Path, help='new JSON under this checkout x9/out')
    args = parser.parse_args()
    out_root = (ROOT / 'x9/out').resolve()
    output = args.out.resolve()
    try:
        require(output.is_relative_to(out_root), 'output must stay in this checkout x9/out')
        require(not output.exists(), 'preserve prior output; choose a new filename')
        if args.case:
            case = args.case.resolve()
            require(case.is_relative_to(out_root) and case.is_dir(), 'saved case must be in this checkout x9/out')
            result = saved(case)
        else:
            result = replay(args.group, args.cache.resolve())
        result.update(scope='original Apple CMPhoto static software-alpha evidence only',
                      runtime_reachability=False, alpha_implementation=False,
                      command=[sys.executable, *sys.argv])
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open('x') as destination:
            os.chmod(output, 0o600)
            destination.write(json.dumps(result, indent=2) + '\n')
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        parser.error(str(error))
    print(json.dumps({'mode': result['mode'], 'range_count': len(result['ranges']),
                      'stub_count': result.get('stub_count', 0), 'output': str(output)}))


if __name__ == '__main__':
    main()
