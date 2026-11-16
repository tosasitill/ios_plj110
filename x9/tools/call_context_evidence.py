#!/usr/bin/env python3
"""Read fixed 20H392 call-context ranges from an existing root/cache, without probes.

Modes qualify the personality producer, its dispatch targets, the typed Client
input path, or the model's concrete config owner. Requires capstone. Creates only
a NEW x9/out directory; original cache, root, extracted images and older evidence
are never changed. A successful exit
qualifies static bytes/exports, not a loaded backend, native epoch or call/SMS result.
"""
import argparse
import json
import struct
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM

MAIN = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mode', required=True, choices=('producer', 'queue', 'client', 'config'))
parser.add_argument('--root-mount', type=Path, default=Path('/private/tmp/x9root'),
                    help='existing current root mount; this tool never attaches or changes it')
parser.add_argument('--out', required=True, type=Path, help='NEW directory under repository x9/out')
args = parser.parse_args()
QUEUE_ONLY = args.mode == 'queue'
CLIENT_ONLY = args.mode == 'client'
CONFIG_ONLY = args.mode == 'config'
OUT = args.out.resolve()
if not OUT.is_relative_to((MAIN / 'x9/out').resolve()):
    parser.error('--out must be under repository x9/out')
OUT.mkdir(parents=True, exist_ok=False)
CACHE = args.root_mount / 'private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld'

class Macho:
    """Read only segment metadata needed for bounded byte comparisons/export lookup."""
    def __init__(self, path):
        self.d = path.read_bytes()
        if len(self.d) < 32 or struct.unpack_from('<I', self.d)[0] != 0xfeedfacf:
            raise ValueError('expected little-endian Mach-O64: '+str(path))
        self.segs = []
        p = 32
        for _ in range(struct.unpack_from('<I', self.d, 16)[0]):
            cmd, size = struct.unpack_from('<II', self.d, p)
            if size < 8 or p+size > len(self.d):
                raise ValueError('invalid Mach-O load command')
            if cmd == 0x19:
                name = self.d[p+8:p+24].split(b'\0')[0].decode()
                self.segs.append((name, *struct.unpack_from('<QQQQ', self.d, p+24)))
            p += size
    def read(self, va, size):
        for _, address, _, offset, file_size in self.segs:
            if address <= va and va+size <= address+file_size:
                data = self.d[offset+va-address:offset+va-address+size]
                if len(data) == size:
                    return data
        raise ValueError('unmapped extracted Mach-O range: '+hex(va))

maps = []
slide_maps = []
for path in sorted(CACHE.glob('dyld_shared_cache_arm64*')):
    with path.open('rb') as f:
        h = f.read(32)
        if not h.startswith(b'dyld_v'):
            continue
        off, count = struct.unpack_from('<II', h, 16)
        f.seek(off)
        maps.extend((path, a, n, o) for a, n, o, _, _ in struct.iter_unpack('<QQQII', f.read(count * 32)))
        # This arm64 cache's current mapping-and-slide table, not a guessed pointer mask.
        f.seek(0x138)
        off, count = struct.unpack('<II', f.read(8))
        if count > 16 or off > 0x4000:
            raise ValueError('unexpected mapping-and-slide metadata')
        f.seek(off)
        slide_maps.extend((path, *m) for m in struct.iter_unpack('<6Q2I', f.read(count * 56)))
reads = []
def read(va, n):
    for path, a, size, off in maps:
        if a <= va and va + n <= a + size:
            with path.open('rb') as f:
                f.seek(off + va - a)
                data = f.read(n)
            if len(data) != n:
                raise ValueError('short original read')
            reads.append({'path': str(path), 'va': hex(va), 'size': n, 'offset': hex(off + va - a)})
            return data
    raise ValueError('unmapped original VA: ' + hex(va))

support = MAIN / '.local/apple/dsc-20H392/System/Library/Frameworks/CoreTelephony.framework/Support'
paths = {'system': support / 'libSystemDetermination.dylib', 'base': support / 'libCommCenterBase.dylib',
         'cxx': MAIN / '.local/apple/dsc-20H392/usr/lib/libc++.1.dylib',
         'dispatch': MAIN / '.local/apple/dsc-20H392/usr/lib/system/libdispatch.dylib',
         'root': args.root_mount / 'System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter'}
image_names = ('system',) if CONFIG_ONLY else ('cxx', 'dispatch') if QUEUE_ONLY else tuple(paths)
images = {k: Macho(paths[k]) for k in image_names}
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail = True
slices = [
    ('model-personality-update-getters', 'system', 0x1f2e77c04, 0x19c),
    ('controller-model-getter', 'system', 0x1f2e421c8, 0x24),
    ('controller-info-getter-and-thunk', 'system', 0x1f2e72d40, 0x20),
    ('controller-identifier-getter', 'system', 0x1f2e6734c, 8),
    ('manager-personality-added', 'system', 0x1f2e52048, 0xce4),
    ('manager-personality-enabled', 'system', 0x1f2e52f58, 0xbb8),
    ('manager-personality-removed', 'system', 0x1f2e53bc8, 0x1e0),
    ('manager-added-thunk', 'system', 0x1f2e52f50, 8),
    ('manager-enabled-thunk', 'system', 0x1f2e53bc0, 8),
    ('manager-removed-thunk', 'system', 0x1f2e53da8, 8),
    ('controller-personality-removed', 'system', 0x1f2e69980, 0x140),
    ('tracker-unbind', 'base', 0x1f038a944, 0xfc),
    ('tracker-bind', 'base', 0x1f038aa40, 0x394),
    ('tracker-update-entrypoints', 'base', 0x1f038add4, 0x30),
    ('tracker-hookup-update', 'base', 0x1f038c1c4, 0x258),
    ('tracker-update-async', 'base', 0x1f038c4e0, 0x238),
    ('tracker-update-notify', 'base', 0x1f038c75c, 0xe0),
    ('root-tracker-bind-site', 'root', 0x1007a1fe4, 0x50),
    ('root-tracker-queue-site', 'root', 0x1007a0ce8, 0x18),
    ('root-handler-personality-change', 'root', 0x1007a5150, 0x494),
    ('root-handler-personality-bootstrap', 'root', 0x1007a7398, 0x1dc),
]
if QUEUE_ONLY:
    slices = []
elif CONFIG_ONLY:
    # Only newly qualified ctor/config ranges, not the old model update,
    # manager getters, tracker or queue slices. +58 is a config strong pair.
    slices = [
        ('model-C1-forward', 'system', 0x1f2e76ed0, 4),
        ('model-C2-argument-map', 'system', 0x1f2e77538, 0x34),
        ('model-owned-fields', 'system', 0x1f2e776d8, 0xbc),
        ('config-C1-forward', 'system', 0x1f2e42d6c, 4),
        ('config-C2-argument-map', 'system', 0x1f2e42d70, 0x2c),
        ('config-addresspoint-and-owned-fields', 'system', 0x1f2e42f04, 0x84),
        ('config-update', 'system', 0x1f2e430fc, 8),
        ('owned-pair-assignment', 'system', 0x1f2e43104, 0x78),
    ]
elif CLIENT_ONLY:
    slices = [
        ('controller-personality-added', 'system', 0x1f2e696c4, 0x2bc),
        ('controller-strong-client-getter', 'system', 0x1f2e68b38, 0x24),
        ('manager-strong-client-getter', 'system', 0x1f2e4c210, 0x5bc),
        ('manager-strong-client-getter-block', 'system', 0x1f2e61a04, 0x1b4),
        ('manager-client-getter-sync-wrapper', 'system', 0x1f2e61bb8, 0x7c),
        ('manager-client-getter-wait-wrapper', 'system', 0x1f2e61c98, 0x7c),
        ('root-typed-client-owner', 'root', 0x100dd1168, 0x14c),
        ('root-typed-normal-client-owner', 'root', 0x100dd3498, 0x1c0),
        ('root-typed-manager-registry', 'root', 0x1001acd00, 0x158),
    ]
    slices.append(('registry-is-present', 'base', 0x1f038b750, 0x108))
proof = []
for name, image, va, n in slices:
    data = images[image].read(va, n)
    if len(data) != n:
        raise ValueError('short extracted text')
    if image != 'root' and data != read(va, n):
        raise ValueError('extracted text differs from current original: ' + name)
    ins = list(md.disasm(data, va))
    if sum(i.size for i in ins) != n:
        raise ValueError('incomplete instruction coverage: ' + name)
    (OUT / (name + '.original-code.bin')).write_bytes(data)
    (OUT / (name + '.original.asm')).write_text('\n'.join(f'{i.address:016x} {i.bytes.hex()} {i.mnemonic} {i.op_str}' for i in ins) + '\n')
    proof.append({'name': name, 'image': image, 'va': hex(va), 'size': n,
                  'source': 'current root executable' if image == 'root' else 'current original cache; extracted text equals original'})

data_ranges = [('model-vtable-context', 'system', 0x20e058f38, 0x28),
               ('controller-vtable-model', 'system', 0x20e058180, 8),
               ('controller-vtable-identifier', 'system', 0x20e058198, 8),
               ('manager-secondary-personality-slots', 'system', 0x20e057718, 0x18),
               ('manager-secondary-addresspoint', 'system', 0x20e0575e0, 0x18),
               ('root-personality-event-jump-table', 'root', 0x10114d135, 8)]
if QUEUE_ONLY:
    data_ranges = []
elif CONFIG_ONLY:
    data_ranges = [('config-personality-update-vtable-slot', 'system', 0x20e056830, 8)]
elif CLIENT_ONLY:
    data_ranges = [('manager-client-ref-vtable-slot', 'system', 0x20e057350, 8),
                   ('controller-client-ref-vtable-slot', 'system', 0x20e0581a8, 8),
                   ('controller-create-client-vtable-slot', 'system', 0x20e0581d8, 8),
                   ('controller-remove-operations-vtable-slots', 'system', 0x20e0581e8, 16),
                   ('controller-personality-actions-vtable-slots', 'system', 0x20e0582b8, 16),
                   ('model-personality-actions-vtable-slots', 'system', 0x20e0591e0, 16),
                   ('root-manager-interface-rtti-import', 'root', 0x10151b320, 8)]
data_proof = []
for name, image, va, n in data_ranges:
    data = images[image].read(va, n) if image == 'root' else read(va, n)
    (OUT / (name + '.original-data.bin')).write_bytes(data)
    record = {'name': name, 'va': hex(va), 'hex': data.hex(), 'image': image}
    if name == 'root-personality-event-jump-table':
        record['case_targets'] = {str(j): hex(0x1007a51a0 + v*4) for j, v in enumerate(data)}
    else:
        record['qwords'] = [hex(v[0]) for v in struct.iter_unpack('<Q', data)]
    data_proof.append(record)

# Read current-cache export tries, not reconstructed nlist, to name new ABI/queue targets.
exports = []
export_images = ('system',) if CONFIG_ONLY else ('cxx', 'dispatch') if QUEUE_ONLY else ('system', 'base', 'cxx', 'dispatch')
for image in export_images:
    obj = images[image]
    base = next(s[1] for s in obj.segs if s[0] == '__TEXT')
    header = read(base, 32)
    commands = read(base + 32, struct.unpack_from('<I', header, 20)[0])
    p = 0
    link = exp = None
    for _ in range(struct.unpack_from('<I', header, 16)[0]):
        cmd, n = struct.unpack_from('<II', commands, p)
        if cmd == 0x19 and commands[p+8:p+24].split(b'\0')[0] == b'__LINKEDIT':
            vm, _, off, _ = struct.unpack_from('<QQQQ', commands, p + 24)
            link = (vm, off)
        if cmd == 0x80000033:
            exp = struct.unpack_from('<II', commands, p + 8)
        p += n
    if not link or not exp:
        raise ValueError('missing original export metadata: ' + image)
    trie = read(link[0] + exp[0] - link[1], exp[1])
    def uleb(p):
        v = shift = 0
        while True:
            b = trie[p]; p += 1; v |= (b & 127) << shift
            if not b & 128:
                return v, p
            shift += 7
            if shift > 63:
                raise ValueError('export integer overflow')
    def walk(p, name, seen):
        if p in seen:
            raise ValueError('export cycle')
        size, q = uleb(p); end = q + size
        if size:
            flags, r = uleb(q)
            if not flags & 8:
                value, r = uleb(r); va = base + value
                selected = any(k == image and a <= va < a+n for _, k, a, n in slices)
                selected |= image == 'cxx' and name.endswith('__shared_weak_count4lockEv')
                selected |= image == 'dispatch' and name in ('_dispatch_async_f', '_dispatch_group_notify_f', '_dispatch_queue_attr_make_with_qos_class', '_dispatch_queue_create_with_target$V2')
                if CLIENT_ONLY:
                    selected |= image == 'base' and va == 0x1f038b750
                    selected |= image == 'dispatch' and name in ('_dispatch_sync', '_dispatch_async_and_wait')
                if CONFIG_ONLY:
                    selected |= image == 'system' and va in (0x20e056810, 0x20e058ef8)
                if selected:
                    exports.append({'image': image, 'symbol': name, 'flags': hex(flags), 'va': hex(va)})
        count = trie[end]; q = end + 1
        for _ in range(count):
            z = trie.index(0, q); suffix = trie[q:z].decode(); child, q = uleb(z+1)
            walk(child, name + suffix, seen | {p})
    walk(0, '', set())

page_cache = {}
def file_read(path, off, size):
    with path.open('rb') as f:
        f.seek(off)
        data = f.read(size)
    if len(data) != size:
        raise ValueError('short original metadata read')
    reads.append({'path': str(path), 'offset': hex(off), 'size': size, 'source': 'original slide metadata/page'})
    return data

def rebase_v2(got):
    matches = [m for m in slide_maps if m[1] <= got and got+8 <= m[1]+m[2]]
    if len(matches) != 1:
        raise ValueError('ambiguous original GOT mapping')
    path, a, size, fo, so, ss, flags, maxprot, initprot = matches[0]
    header = file_read(path, so, 40)
    version, page_size, starts_off, starts_n, extras_off, extras_n, mask, add = struct.unpack('<6I2Q', header)
    if version != 2 or page_size != 4096 or not mask or ss < 40:
        raise ValueError('unsupported original GOT slide format')
    index, offset = divmod(got-a, page_size)
    if index >= starts_n or starts_off+2*starts_n > ss or (extras_n and extras_off+2*extras_n > ss):
        raise ValueError('original slide arrays out of bounds')
    entry = struct.unpack('<H', file_read(path, so+starts_off+2*index, 2))[0]
    if entry == 0x4000:
        raise ValueError('GOT page has no rebases')
    starts = []
    if entry & 0x8000:
        j = entry & 0x3fff
        while True:
            if j >= extras_n:
                raise ValueError('original slide extras overflow')
            e = struct.unpack('<H', file_read(path, so+extras_off+2*j, 2))[0]
            starts.append((e & 0x3fff)*4)
            if e & 0x8000:
                break
            j += 1
    else:
        starts.append(entry*4)
    key = (path, index)
    if key not in page_cache:
        page_cache[key] = file_read(path, fo+index*page_size, page_size)
        (OUT/f'got-page-{a+index*page_size:x}.original-data.bin').write_bytes(page_cache[key])
    page = page_cache[key]
    shift = (mask & -mask).bit_length()-1-2
    chain = []
    found = False
    for start in starts:
        p = start
        while p <= page_size-8:
            raw = struct.unpack_from('<Q', page, p)[0]
            delta = (raw & mask) >> shift
            chain.append({'offset': hex(p), 'raw': hex(raw), 'delta': delta})
            found |= p == offset
            if delta == 0:
                break
            if delta < 4 or p+delta >= page_size:
                raise ValueError('invalid bounded original rebase chain')
            p += delta
    if not found:
        raise ValueError('GOT is not an original rebase-chain member')
    raw = struct.unpack_from('<Q', page, offset)[0]
    value = raw & (~mask & ((1 << 64)-1))
    target = value+add if value else 0
    return target, {'got': hex(got), 'raw': hex(raw), 'mapping_address': hex(a), 'mapping_file_offset': hex(fo),
        'slide_offset': hex(so), 'slide_size': ss, 'header_hex': header.hex(), 'version': version,
        'page_size': page_size, 'page_index': index, 'page_entry': hex(entry), 'delta_mask': hex(mask),
        'value_add': hex(add), 'chain': chain, 'is_original_chain_member': True, 'unslid_target': hex(target)}

stubs = []
stub_specs = ((0x1f03c19b8, '_dispatch_async_f'), (0x1f03c1a0c, '_dispatch_group_notify_f'),
                     (0x1f03c1a24, '_dispatch_queue_attr_make_with_qos_class'),
                     (0x1f03c1a3c, '_dispatch_queue_create_with_target$V2'),
                     (0x1f2f7b31c, '__ZNSt3__119__shared_weak_count4lockEv'))
if CLIENT_ONLY:
    stub_specs = ((0x1f2fb16cc, '__Z9isPresentRKNSt3__110shared_ptrIK8RegistryEERK13PersonalityID'),
                  (0x1f2e82fe8, '_dispatch_async_and_wait'), (0x1f2e83084, '_dispatch_sync'))
elif CONFIG_ONLY:
    stub_specs = ()
for va, expected in stub_specs:
    data = read(va, 12); ins = list(md.disasm(data, va)); regs = {}; target = None; got_proof = None
    if len(ins) != 3:
        raise ValueError('unexpected original stub size')
    for i in ins:
        p = i.operands
        if i.mnemonic == 'adrp' and p[1].type == ARM64_OP_IMM:
            regs[p[0].reg] = p[1].imm
        elif i.mnemonic == 'add' and p[1].reg in regs and p[2].type == ARM64_OP_IMM and not p[2].shift.value:
            regs[p[0].reg] = regs[p[1].reg] + p[2].imm
        elif i.mnemonic == 'ldr' and p[1].type == ARM64_OP_MEM and p[1].mem.base in regs and not p[1].mem.index:
            regs[p[0].reg], got_proof = rebase_v2(regs[p[1].mem.base]+p[1].mem.disp)
        elif i.mnemonic == 'br' and p[0].reg in regs:
            target = regs[p[0].reg]
        else:
            raise ValueError('unsupported original stub instruction: '+i.mnemonic+' '+i.op_str)
    matches = [e for e in exports if target is not None and int(e['va'],16) == target]
    if not any(e['symbol'] == expected for e in matches):
        raise ValueError('original stub target does not match expected true export: '+expected)
    (OUT/f'stub-{va:x}.original-code.bin').write_bytes(data)
    stubs.append({'va': hex(va), 'hex': data.hex(), 'instructions': [f'{i.mnemonic} {i.op_str}' for i in ins],
                  'target': hex(target), 'original_target_exports': matches, 'got_rebase': got_proof})
if CLIENT_ONLY or CONFIG_ONLY:
    for record in data_proof:
        if record['image'] == 'system':
            record['rebased_slots'] = [rebase_v2(int(record['va'],16)+offset)[1] for offset in range(0,len(bytes.fromhex(record['hex'])),8)]
            if [int(r['raw'],16) for r in record['rebased_slots']] != [int(v,16) for v in record['qwords']]:
                raise ValueError('original slot bytes differ from recorded data')
if CONFIG_ONLY:
    slot = data_proof[0]['rebased_slots'][0]
    if int(slot['unslid_target'], 16) != 0x1f2e430fc or not any(
            int(e['va'], 16) == 0x1f2e430fc and 'IMSSubscriberConfig' in e['symbol'] and
            'updatePersonalityInfo' in e['symbol'] for e in exports):
        raise ValueError('config update slot does not name the concrete original export')
    if not any(e['symbol'] == '__ZTVN2sd19IMSSubscriberConfigE' and
               int(e['va'], 16) == 0x20e056810 for e in exports):
        raise ValueError('config addresspoint lacks the concrete original vtable export')
proof_name = 'original-' + args.mode + '-proof.json'
(OUT/proof_name).write_text(json.dumps({'scope':'Original static context ABI; not loaded backend or live call/SMS verification',
    'sources': {k:str(p) for k,p in paths.items()}, 'slices':proof, 'data':data_proof, 'exports':exports,
    'optimized_queue_stubs':stubs, 'original_reads':reads}, indent=2)+'\n')
print(f'{len(proof)} text ranges; {len(data_proof)} data ranges; {len(exports)} exports; qualified ABI targets={len(stubs)}')
