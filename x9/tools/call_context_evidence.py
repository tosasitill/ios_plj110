#!/usr/bin/env python3
"""Read fixed 20H392 call-context ranges from an existing root/cache, without probes.

Modes qualify the personality producer, its dispatch targets, the typed Client
input path, the model's concrete config owner, the controller's bound-instance
owner, its concrete root factory's shared record inputs, or that factory's
manager selection/queue ownership, or the concrete Call driver's incoming-record
action. Requires capstone. Creates only
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
parser.add_argument('--mode', required=True, choices=(
    'producer', 'queue', 'client', 'config', 'instance', 'factory', 'selection',
    'action', 'action-owner', 'action-update', 'action-assignment', 'action-driver'))
parser.add_argument('--root-mount', type=Path, default=Path('/private/tmp/x9root'),
                    help='existing current root mount; this tool never attaches or changes it')
parser.add_argument('--out', required=True, type=Path, help='NEW directory under repository x9/out')
args = parser.parse_args()
QUEUE_ONLY = args.mode == 'queue'
CLIENT_ONLY = args.mode == 'client'
CONFIG_ONLY = args.mode == 'config'
INSTANCE_ONLY = args.mode == 'instance'
FACTORY_ONLY = args.mode == 'factory'
SELECTION_ONLY = args.mode == 'selection'
ACTION_ONLY = args.mode in ('action', 'action-owner', 'action-update', 'action-assignment', 'action-driver')
ACTION_OWNER_ONLY = args.mode == 'action-owner'
ACTION_UPDATE_ONLY = args.mode == 'action-update'
ACTION_ASSIGNMENT_ONLY = args.mode == 'action-assignment'
ACTION_DRIVER_ONLY = args.mode == 'action-driver'
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
image_names = ('root', 'system') if SELECTION_ONLY else ('root',) if FACTORY_ONLY or ACTION_ONLY else ('system',) if CONFIG_ONLY or INSTANCE_ONLY else ('cxx', 'dispatch') if QUEUE_ONLY else tuple(paths)
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
elif ACTION_ONLY:
    # Exact literal-reference lead delimited by original LC_FUNCTION_STARTS.
    # Do not repeat old factory/getter/controller/subscriber-update ranges.
    if ACTION_DRIVER_ONLY:
        slices = []
    elif ACTION_ASSIGNMENT_ONLY:
        slices = [('root-call-model-record-assignment', 'root', 0x1001348a4, 0x78)]
    elif ACTION_UPDATE_ONLY:
        slices = [('root-call-model-record-update', 'root', 0x1007cc880, 0x4c)]
    elif ACTION_OWNER_ONLY:
        slices = [('root-call-record-model-lookup', 'root', 0x100dcbdac, 0x168),
                  ('root-call-model-record-ctor', 'root', 0x1007c276c, 0x4f0)]
    else:
        slices = [('root-call-driver-incoming-record', 'root', 0x100dd6508, 0x9c4)]
elif SELECTION_ONLY:
    # New manager-selection dependencies only. Do not repeat the old root
    # creator, manager added/getter, controller or model/config ranges.
    slices = [
        ('root-manager-factory-selection', 'root', 0x100531f50, 0xc0),
        ('manager-create-factory-owner', 'system', 0x1f2e4ae64, 0x2bc),
        ('manager-C1-forward', 'system', 0x1f2e4b120, 4),
        ('manager-C2-factory-queue-owner', 'system', 0x1f2e4b2e0, 0x424),
    ]
elif FACTORY_ONLY:
    # Original LC_FUNCTION_STARTS delimits this concrete creator. It calls
    # the three named imports below; do not infer it from a nearby symbol.
    slices = [('root-shared-record-factory', 'root', 0x100315488, 0x49c)]
elif INSTANCE_ONLY:
    # This is the controller's real C1/C2, not PersonalitySpecificImpl or
    # the previously read model/config ctors. The instance is mutable: its
    # ownership alone is not a record snapshot, generation, or registration.
    slices = [
        ('controller-C1-forward', 'system', 0x1f2e66b4c, 4),
        ('controller-C2-instance-owner', 'system', 0x1f2e66b6c, 0x5b8),
    ]
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
if QUEUE_ONLY or INSTANCE_ONLY or FACTORY_ONLY or SELECTION_ONLY or ACTION_ONLY:
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
export_images = () if FACTORY_ONLY or ACTION_ONLY else ('system',) if CONFIG_ONLY or INSTANCE_ONLY or SELECTION_ONLY else ('cxx', 'dispatch') if QUEUE_ONLY else ('system', 'base', 'cxx', 'dispatch')
original_headers = {}
for image in export_images:
    obj = images[image]
    base = next(s[1] for s in obj.segs if s[0] == '__TEXT')
    header = read(base, 32)
    commands = read(base + 32, struct.unpack_from('<I', header, 20)[0])
    original_headers[image] = (base, header, commands)
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
elif CONFIG_ONLY or INSTANCE_ONLY or FACTORY_ONLY or SELECTION_ONLY or ACTION_ONLY:
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
root_imports = []
function_boundaries = []
root_fixups = []
if ACTION_ONLY:
    obj = images['root']
    p = 32
    end = 32 + struct.unpack_from('<I', obj.d, 20)[0]
    if end > len(obj.d):
        raise ValueError('root action load commands exceed file')
    metadata = None
    for _ in range(struct.unpack_from('<I', obj.d, 16)[0]):
        if p+8 > end:
            raise ValueError('truncated root action load command')
        cmd, size = struct.unpack_from('<II', obj.d, p)
        if size < 8 or p+size > end:
            raise ValueError('invalid root action load command')
        if cmd == 0x26:
            if size < 16 or metadata is not None:
                raise ValueError('invalid/duplicate root action function starts')
            metadata = struct.unpack_from('<II', obj.d, p+8)
        p += size
    if p != end:
        raise ValueError('root action load-command count/size mismatch')
    if not metadata or not metadata[1] or sum(metadata) > len(obj.d):
        raise ValueError('missing/bounded root action function starts')
    off, size = metadata
    data = obj.d[off:off+size]
    (OUT/'root-action-function-starts.original-data.bin').write_bytes(data)
    address = next(s[1] for s in obj.segs if s[0] == '__TEXT')
    starts = []
    value = shift = 0
    terminated = False
    for byte in data:
        value |= (byte & 127) << shift
        if byte & 128:
            shift += 7
            if shift > 63:
                raise ValueError('root action function-start ULEB overflow')
        else:
            if value == 0:
                terminated = True
                break
            address += value
            starts.append(address)
            value = shift = 0
    if not terminated:
        raise ValueError('unterminated root action function starts')
    for _, _, first, size in slices:
        last = first+size
        if first not in starts or starts[starts.index(first)+1:][:1] != [last]:
            raise ValueError('root action function boundary differs')
        function_boundaries.append({'image': 'root', 'start': hex(first), 'end': hex(last),
            'source': 'original LC_FUNCTION_STARTS in current root executable; literal name remains a lead'})
if INSTANCE_ONLY:
    for va, prefix in ((0x1f2e66b4c, '__ZN2sd23IMSSubscriberControllerC1E'),
                       (0x1f2e66b6c, '__ZN2sd23IMSSubscriberControllerC2E')):
        if not any(e['image'] == 'system' and int(e['va'], 16) == va and
                   e['flags'] == '0x0' and e['symbol'].startswith(prefix) and
                   e['symbol'].endswith('37IMSSubscriberPersonalityBoundInstanceEEE')
                   for e in exports):
            raise ValueError('controller constructor lacks exact regular original export')
    forward = images['system'].read(0x1f2e66b4c, 4)
    instruction = list(md.disasm(forward, 0x1f2e66b4c))
    if len(instruction) != 1 or instruction[0].mnemonic != 'b' or len(instruction[0].operands) != 1 or \
            instruction[0].operands[0].type != ARM64_OP_IMM or instruction[0].operands[0].imm != 0x1f2e66b6c:
        raise ValueError('controller C1 does not directly forward to the qualified C2')
if FACTORY_ONLY or SELECTION_ONLY:
    # Root imports use the executable's original indirect symbol table.
    # On-disk lazy/fixup words are not live pointers and are not decoded
    # with a shared-cache slide mask. Save only the named stub bytes here.
    obj = images['root']
    p = 32
    sym = indirect = function_starts = None
    sections = []
    for _ in range(struct.unpack_from('<I', obj.d, 16)[0]):
        cmd, size = struct.unpack_from('<II', obj.d, p)
        if cmd == 2:
            sym = struct.unpack_from('<4I', obj.d, p+8)
        elif cmd == 0xb:
            indirect = struct.unpack_from('<II', obj.d, p+56)
        elif cmd == 0x26:
            function_starts = struct.unpack_from('<II', obj.d, p+8)
        elif cmd == 0x19:
            for j in range(struct.unpack_from('<I', obj.d, p+64)[0]):
                s = p + 72 + j*80
                if (struct.unpack_from('<I', obj.d, s+64)[0] & 0xff) == 8:
                    a, n = struct.unpack_from('<QQ', obj.d, s+32)
                    first, stride = struct.unpack_from('<II', obj.d, s+68)
                    sections.append((a, n, first, stride))
        p += size
    if not sym or not indirect:
        raise ValueError('missing original root import metadata')
    if not function_starts:
        raise ValueError('missing original root function-start metadata')
    off, size = function_starts
    if off+size > len(obj.d):
        raise ValueError('root function-start metadata exceeds file')
    address = next(s[1] for s in obj.segs if s[0] == '__TEXT')
    starts = []
    value = shift = 0
    for byte in obj.d[off:off+size]:
        value |= (byte & 127) << shift
        if byte & 128:
            shift += 7
            if shift > 63:
                raise ValueError('root function-start ULEB overflow')
        else:
            if value == 0:
                break
            address += value
            starts.append(address)
            value = shift = 0
    if FACTORY_ONLY and (0x100315488 not in starts or starts[starts.index(0x100315488)+1:][:1] != [0x100315924]):
        raise ValueError('root shared-record creator boundaries differ from original metadata')
    if SELECTION_ONLY:
        for first, last in ((0x10052f524, 0x1005328cc), (0x100315488, 0x100315924)):
            if first not in starts or starts[starts.index(first)+1:][:1] != [last]:
                raise ValueError('root selection/target function boundary differs')
            function_boundaries.append({'image': 'root', 'start': hex(first), 'end': hex(last),
                'source': 'original LC_FUNCTION_STARTS in current root executable'})
    so, sn, strings, strings_n = sym
    io, count = indirect
    if so+16*sn > len(obj.d) or strings+strings_n > len(obj.d) or io+4*count > len(obj.d):
        raise ValueError('original root import tables exceed file')
    import_specs = ((0x10107f4a4, '__ZN26SystemDeterminationManager6create'),) if SELECTION_ONLY else (
        (0x10107f990, '__ZN2sd18IMSSubscriberModel6create'),
        (0x10107f99c, '__ZN2sd19IMSSubscriberConfig6create'),
        (0x10107f9a8, '__ZN2sd23IMSSubscriberController6create'))
    for va, expected in import_specs:
        matches = [(a, n, first, stride) for a, n, first, stride in sections
                   if stride == 12 and a <= va and va+stride <= a+n and (va-a) % stride == 0]
        if len(matches) != 1:
            raise ValueError('root import is not one bounded original stub')
        a, _, first, stride = matches[0]
        index = first + (va-a)//stride
        if index >= count:
            raise ValueError('original root indirect index overflow')
        symbol_index = struct.unpack_from('<I', obj.d, io+4*index)[0]
        if symbol_index >= sn:
            raise ValueError('original root import is not a named symbol')
        string_index, kind = struct.unpack_from('<IB', obj.d, so+16*symbol_index)
        if string_index >= strings_n or (kind & 0x0e) != 0:
            raise ValueError('original root import is not undefined')
        end = obj.d.index(0, strings+string_index, strings+strings_n)
        name = obj.d[strings+string_index:end].decode()
        if not name.startswith(expected+'E'):
            raise ValueError('original root import name mismatch')
        data = obj.read(va, stride)
        instructions = list(md.disasm(data, va))
        if sum(i.size for i in instructions) != stride:
            raise ValueError('incomplete original root stub instructions')
        (OUT/f'root-import-{va:x}.original-code.bin').write_bytes(data)
        root_imports.append({'va': hex(va), 'symbol': name, 'hex': data.hex(),
            'indirect_index': index, 'symbol_index': symbol_index,
            'indirect_file_offset': hex(io+4*index), 'nlist_file_offset': hex(so+16*symbol_index),
            'string_file_offset': hex(strings+string_index),
            'instructions': [f'{i.mnemonic} {i.op_str}' for i in instructions],
            'scope': 'original named import; not a live GOT pointer or a loaded call backend'})
if SELECTION_ONLY:
    # Exact exported manager signatures and original function starts; nlist
    # remains a lead only. No old method ranges are read for these guards.
    manager_parameters = ('RKNSt3__110shared_ptrI8RegistryEENS0_10unique_ptrIN2sd29IMSSubscriberFactoryInterface'
                          'ENS0_14default_deleteIS8_EEEE')
    for va, prefix in ((0x1f2e4ae64, '__ZN26SystemDeterminationManager6createE'),
                       (0x1f2e4b120, '__ZN26SystemDeterminationManagerC1E'),
                       (0x1f2e4b2e0, '__ZN26SystemDeterminationManagerC2E')):
        matches = [e for e in exports if int(e['va'], 16) == va]
        if len(matches) != 1 or matches[0]['flags'] != '0x0' or matches[0]['symbol'] != prefix+manager_parameters:
            raise ValueError('manager selection lacks exact REGULAR constructor/create export')
    ins = list(md.disasm(images['system'].read(0x1f2e4b120, 4), 0x1f2e4b120))
    if len(ins) != 1 or ins[0].mnemonic != 'b' or len(ins[0].operands) != 1 or \
            ins[0].operands[0].type != ARM64_OP_IMM or ins[0].operands[0].imm != 0x1f2e4b2e0:
        raise ValueError('manager C1 does not directly forward to qualified C2')
    base, _, commands = original_headers['system']
    p = 0
    link = function_starts = None
    while p < len(commands):
        if p+8 > len(commands):
            raise ValueError('truncated original system load command')
        cmd, size = struct.unpack_from('<II', commands, p)
        if size < 8 or p+size > len(commands):
            raise ValueError('invalid original system load command size')
        if (cmd == 0x19 and size < 72) or (cmd == 0x26 and size < 16):
            raise ValueError('truncated original system selection dependency')
        if cmd == 0x19 and commands[p+8:p+24].split(b'\0')[0] == b'__LINKEDIT':
            vm, _, fo, _ = struct.unpack_from('<4Q', commands, p+24)
            link = (vm, fo)
        elif cmd == 0x26:
            function_starts = struct.unpack_from('<II', commands, p+8)
        p += size
    if not link or not function_starts or not function_starts[1]:
        raise ValueError('original system function starts unavailable; no extracted substitute')
    off, size = function_starts
    data = read(link[0]+off-link[1], size)
    (OUT/'system-function-starts.original-data.bin').write_bytes(data)
    starts = []
    address = base
    value = shift = 0
    for byte in data:
        value |= (byte & 127) << shift
        if byte & 128:
            shift += 7
            if shift > 63:
                raise ValueError('system function-start ULEB overflow')
        else:
            if value == 0:
                break
            address += value
            starts.append(address)
            value = shift = 0
    for first, last in ((0x1f2e4ae64, 0x1f2e4b120), (0x1f2e4b120, 0x1f2e4b124), (0x1f2e4b2e0, 0x1f2e4b704)):
        if first not in starts or starts[starts.index(first)+1:][:1] != [last]:
            raise ValueError('original system selection function boundary differs')
        function_boundaries.append({'image': 'system', 'start': hex(first), 'end': hex(last),
            'source': 'current-cache original LC_FUNCTION_STARTS'})
if SELECTION_ONLY or ACTION_UPDATE_ONLY or ACTION_ASSIGNMENT_ONLY or ACTION_DRIVER_ONLY:
    # Root pointer format comes from its own LC_DYLD_CHAINED_FIXUPS. A root
    # disk word is never decoded with the shared-cache slide-v2 mask.
    obj = images['root']
    p = 32
    end = 32 + struct.unpack_from('<I', obj.d, 20)[0]
    if end > len(obj.d):
        raise ValueError('root fixup load commands exceed file')
    fix = None
    for _ in range(struct.unpack_from('<I', obj.d, 16)[0]):
        if p+8 > end:
            raise ValueError('truncated root fixup load command')
        cmd, size = struct.unpack_from('<II', obj.d, p)
        if size < 8 or p+size > end:
            raise ValueError('invalid root fixup load command')
        if cmd == 0x80000034:
            if size < 16 or fix is not None:
                raise ValueError('invalid/duplicate root chained fixup command')
            fix = struct.unpack_from('<II', obj.d, p+8)
        p += size
    if p != end:
        raise ValueError('root fixup load-command count/size mismatch')
    if not fix or fix[1] < 28 or fix[0]+fix[1] > len(obj.d):
        raise ValueError('root chained fixup metadata unavailable')
    off, size = fix
    version, startoff, _, _, _, _, symbols_format = struct.unpack_from('<7I', obj.d, off)
    if version != 0 or symbols_format != 0 or startoff+4 > size:
        raise ValueError('unsupported original root chained fixup header')
    start = off+startoff
    count = struct.unpack_from('<I', obj.d, start)[0]
    if count != len(obj.segs) or startoff+4+4*count > size:
        raise ValueError('root chained fixup segment table mismatch')
    base = next(s[1] for s in obj.segs if s[0] == '__TEXT')
    if ACTION_DRIVER_ONLY:
        va, expected_target, stem = 0x1015fa360, 0x100dd6508, 'root-call-driver-plus1e0'
        metadata_stem = 'root-call-driver'
    elif ACTION_ASSIGNMENT_ONLY:
        va, expected_target, stem = 0x10159c830, 0x1007cb578, 'root-call-model-plus118'
        metadata_stem = 'root-call-model'
    elif ACTION_UPDATE_ONLY:
        va, expected_target, stem = 0x10159c8f0, 0x1007cc880, 'root-call-model-plus1d8'
        metadata_stem = 'root-call-model'
    else:
        va, expected_target, stem = 0x1015494d8, 0x100315488, 'root-factory-plus30'
        metadata_stem = 'root-factory'
    matches = [(i, s) for i, s in enumerate(obj.segs) if s[1] <= va and va+8 <= s[1]+s[4]]
    if len(matches) != 1:
        raise ValueError('selected root slot not one file-backed segment')
    index, (_, address, _, fo, file_size) = matches[0]
    relative = struct.unpack_from('<I', obj.d, start+4+4*index)[0]
    if not relative or startoff+relative+22 > size:
        raise ValueError('selected root slot segment lacks chained starts')
    meta = start+relative
    metasz, pagesz, fmt, segoff, maxptr, pages = struct.unpack_from('<IHHQIH', obj.d, meta)
    if fmt != 6 or pagesz != 0x4000 or maxptr or base+segoff != address or metasz < 22+2*pages or startoff+relative+metasz > size:
        raise ValueError('unsupported original root format6 segment metadata')
    page, offset = divmod(va-address, pagesz)
    if page >= pages:
        raise ValueError('selected root slot page exceeds chained table')
    entry = struct.unpack_from('<H', obj.d, meta+22+2*page)[0]
    if entry == 0xffff or entry & 0x8000:
        raise ValueError('selected root slot needs unsupported missing/multi chain')
    cursor = entry
    chain = []
    found = False
    while cursor <= pagesz-8:
        fileoff = fo+page*pagesz+cursor
        if fileoff+8 > fo+file_size:
            raise ValueError('root chain leaves file-backed segment')
        raw = struct.unpack_from('<Q', obj.d, fileoff)[0]
        delta = ((raw >> 51) & 0xfff)*4
        chain.append({'offset': hex(cursor), 'raw': hex(raw), 'delta': delta})
        found |= cursor == offset
        if not delta:
            break
        if delta < 4 or cursor+delta > pagesz-8:
            raise ValueError('root chain leaves page')
        cursor += delta
    if not found:
        raise ValueError('selected root slot is not an original format6 chain member')
    raw = struct.unpack('<Q', obj.read(va, 8))[0]
    if raw >> 63 or (raw >> 36) & 0x7fff:
        raise ValueError('selected root slot is not an untagged format6 rebase')
    target = base+(raw & ((1 << 36)-1))
    if target != expected_target:
        raise ValueError('selected root slot target differs from retained target')
    (OUT/(stem+'.original-data.bin')).write_bytes(obj.read(va, 8))
    (OUT/(metadata_stem+'-fixup-header.original-data.bin')).write_bytes(obj.d[off:off+28])
    (OUT/(metadata_stem+'-segment-starts.original-data.bin')).write_bytes(obj.d[meta:meta+metasz])
    root_fixups.append({'va': hex(va), 'raw': hex(raw), 'target': hex(target),
        'format': fmt, 'fixup_offset': hex(off), 'starts_offset': hex(startoff),
        'segment_index': index, 'segment_address': hex(address), 'segment_file_offset': hex(fo),
        'segment_starts_offset': hex(meta), 'page_size': pagesz, 'page_index': page,
        'page_start': hex(entry), 'chain': chain, 'is_original_chain_member': True,
        'scope': 'original root format6 rebase, not loaded GOT or shared-cache slide-mask'})
proof_name = 'original-' + args.mode + '-proof.json'
(OUT/proof_name).write_text(json.dumps({'scope':'Original static context ABI; not loaded backend or live call/SMS verification',
    'sources': {k:str(p) for k,p in paths.items()}, 'slices':proof, 'data':data_proof, 'exports':exports,
    'optimized_queue_stubs':stubs, 'root_imports':root_imports, 'function_boundaries':function_boundaries,
    'root_fixups':root_fixups, 'original_reads':reads}, indent=2)+'\n')
print(f'{len(proof)} text ranges; {len(data_proof)} data ranges; {len(exports)} exports; qualified ABI targets={len(stubs)}; root imports={len(root_imports)}; root fixups={len(root_fixups)}')
