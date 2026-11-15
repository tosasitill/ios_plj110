"""Bounded HFSX lookups and seven-file owner correction on detached COW.

No mount, device transport, tree traversal, allocation or journal writer lives
here. ``dev.read(offset, size)`` must return exact bytes and ``dev.write`` must
modify only the caller's detached COW. The native HFS implementation handles
journal replay, insertion and flushing before this module is used.

Format/comparison sources: Apple's hfs_format.h, hfs_catalog.c:
cat_binarykeycompare, UnicodeWrappers.c:UnicodeBinaryCompare,
FileExtentMapping.c:CompareExtentKeysPlus and BTreeTreeOps.c:SearchTree.
"""
import bisect
from collections.abc import Mapping
import hashlib
import stat
import struct


BINARIES = frozenset((
    '/usr/bin/x9-btctl', '/usr/lib/X9H4Transport.dylib',
    '/usr/libexec/X9BluetoothLaunch', '/usr/libexec/X9BlueTool',
))
TARGETS = BINARIES | frozenset((
    '/System/Library/LaunchDaemons/com.apple.bluetoothd.plist',
    '/System/Library/LaunchDaemons/com.apple.BlueTool.plist',
    '/System/Library/xpc/launchd.plist',
))
MAX_FILE = 64 << 20
MAX_READ = 256 << 20
MAX_NODES = 4096
MAX_EXTENTS = 1024


def be(fmt, data, offset=0):
    return struct.unpack_from('>' + fmt, data, offset)


class _Fork:
    def __init__(self, fs, raw, cnid, kind=0, bootstrap=False):
        if len(raw) != 80:
            raise ValueError('invalid HFSPlusForkData length')
        self.fs, self.cnid, self.kind = fs, cnid, kind
        self.size, self.clump, self.blocks = be('QII', raw)
        if self.size > self.blocks * fs.block or self.blocks > fs.total:
            raise ValueError('invalid fork geometry')
        self.extents = self._extents(raw[16:])
        self.have = sum(count for start, count in self.extents)
        if self.have > self.blocks:
            raise ValueError('inline extents exceed fork blocks')
        self.complete = self.have == self.blocks
        if bootstrap and not self.complete:
            raise ValueError('extents B-tree self-overflow is unsupported')

    def _extents(self, raw):
        if len(raw) != 64:
            raise ValueError('invalid HFSPlusExtentRecord length')
        result, ended = [], False
        for offset in range(0, 64, 8):
            start, count = be('II', raw, offset)
            if not count:
                if start:
                    raise ValueError('nonzero empty extent')
                ended = True
            else:
                if ended or start + count > self.fs.total:
                    raise ValueError('invalid extent range/order')
                result.append((start, count))
        return result

    def resolve(self):
        if not self.complete:
            seen = set()
            while self.have < self.blocks:
                if self.have in seen or len(self.extents) >= MAX_EXTENTS:
                    raise ValueError('overflow extent chain exceeds bounds')
                seen.add(self.have)
                found = self.fs.extents.find((self.cnid, self.kind, self.have))
                if found is None or len(found['body']) != 64:
                    raise ValueError('missing/invalid exact overflow extent key')
                more = self._extents(found['body'])
                count = sum(n for start, n in more)
                if not count or self.have + count > self.blocks:
                    raise ValueError('overflow extent chain exceeds fork')
                self.extents.extend(more)
                self.have += count
            self.complete = True
        ordered = sorted(self.extents)
        if any(a + n > b for (a, n), (b, m) in zip(ordered, ordered[1:])):
            raise ValueError('overlapping fork extents')
        return self.extents

    def ranges(self, offset, size):
        if offset < 0 or size < 0 or offset + size > self.size:
            raise ValueError('fork access exceeds logical EOF')
        logical, left = 0, size
        for start, count in self.resolve():
            length = count * self.fs.block
            if left and logical <= offset < logical + length:
                take = min(left, logical + length - offset)
                yield start * self.fs.block + offset - logical, take
                offset += take
                left -= take
            logical += length
        if left:
            raise ValueError('fork range is not covered')

    def read(self, offset, size):
        return b''.join(self.fs._read(at, count) for at, count in self.ranges(offset, size))

    def write(self, offset, data):
        done = 0
        for at, count in self.ranges(offset, len(data)):
            result = self.fs.dev.write(at, data[done:done + count])
            if result is not None and result != count:
                raise ValueError('short detached COW write')
            done += count


class _Tree:
    def __init__(self, fs, fork, catalog=False):
        self.fs, self.fork, self.catalog = fs, fork, catalog
        self.cache = {}
        probe = fork.read(0, 512)
        self.node_size, = be('H', probe, 32)
        if (self.node_size < 512 or self.node_size > 32768 or
                self.node_size & (self.node_size - 1)):
            raise ValueError('unsupported B-tree node size')
        header = fork.read(0, self.node_size)
        records = self._records(header, 1, 0)
        if len(records) != 3 or records[0][0] != 14 or len(records[0][1]) != 106:
            raise ValueError('invalid B-tree header node records')
        h = records[0][1]
        self.depth, self.root, self.leaf_records, self.first_leaf, self.last_leaf = be('HIIII', h)
        node_size, self.max_key, self.total_nodes, free_nodes = be('HHII', h, 18)
        self.attributes, = be('I', h, 38)
        if (node_size != self.node_size or not self.total_nodes or
                free_nodes > self.total_nodes or
                self.total_nodes * self.node_size > fork.size or
                self.depth > 16 or not self.attributes & 2 or self.attributes & ~7):
            raise ValueError('invalid B-tree header geometry/attributes')
        if catalog and (h[37] != 0xbc or not self.attributes & 4 or self.max_key != 516):
            raise ValueError('requires HFSX BinaryCompare variable catalog keys')
        if not catalog and self.max_key != 10:
            raise ValueError('unsupported extent B-tree keys')
        if self.depth:
            if any(not 0 < n < self.total_nodes for n in
                   (self.root, self.first_leaf, self.last_leaf)) or not self.leaf_records:
                raise ValueError('invalid nonempty B-tree root/leaves')
        elif self.root or self.first_leaf or self.last_leaf or self.leaf_records:
            raise ValueError('invalid empty B-tree')

    def _records(self, data, expected_kind, expected_height):
        if len(data) != self.node_size:
            raise ValueError('short B-tree node')
        forward, backward, kind, height, count, reserved = be('IIbBHH', data)
        if kind != expected_kind or height != expected_height or reserved:
            raise ValueError('invalid B-tree node kind/height')
        if count > (self.node_size - 14) // 2 - 1:
            raise ValueError('invalid B-tree record count')
        offsets = [be('H', data, self.node_size - 2 * (i + 1))[0]
                   for i in range(count + 1)]
        end = self.node_size - 2 * (count + 1)
        if (not offsets or offsets[0] < 14 or offsets[-1] > end or
                any(a >= b or a & 1 for a, b in zip(offsets, offsets[1:]))):
            raise ValueError('invalid B-tree record offsets')
        if hasattr(self, 'total_nodes') and any(n >= self.total_nodes for n in (forward, backward)):
            raise ValueError('B-tree sibling exceeds tree')
        return [(a, data[a:b]) for a, b in zip(offsets, offsets[1:])]

    def _key(self, raw):
        if self.catalog:
            if len(raw) < 6:
                raise ValueError('short catalog key')
            parent, length = be('IH', raw)
            if not parent or length > 255 or len(raw) != 6 + length * 2:
                raise ValueError('invalid catalog UTF16 key length')
            # UTF16BE byte lexicographic order equals Apple's uint16 comparison.
            return parent, raw[6:]
        if len(raw) != 10:
            raise ValueError('invalid extent key length')
        kind, pad, cnid, start = be('BBII', raw)
        if pad or kind not in (0, 255) or not cnid:
            raise ValueError('invalid extent key')
        return cnid, kind, start

    def node(self, number, level):
        if not 0 < number < self.total_nodes:
            raise ValueError('B-tree child outside tree')
        if number not in self.cache:
            self.fs.nodes += 1
            if self.fs.nodes > MAX_NODES:
                raise ValueError('bounded B-tree node budget exceeded')
            self.cache[number] = self.fork.read(number * self.node_size, self.node_size)
        records = self._records(self.cache[number], -1 if level == 1 else 0, level)
        result = []
        for offset, raw in records:
            if len(raw) < 2:
                raise ValueError('short B-tree key record')
            length, = be('H', raw)
            if length > self.max_key or 2 + length > len(raw):
                raise ValueError('B-tree key exceeds record')
            key = self._key(raw[2:2 + length])
            storage = length if level == 1 or self.attributes & 4 else self.max_key
            body_offset = 2 + storage + (storage & 1)
            if body_offset >= len(raw):
                raise ValueError('missing B-tree record body')
            body = raw[body_offset:]
            if level > 1 and len(body) != 4:
                raise ValueError('invalid B-tree index child record')
            result.append({'key': key, 'body': body,
                           'offset': number * self.node_size + offset + body_offset})
        keys = [r['key'] for r in result]
        if not keys or any(a >= b for a, b in zip(keys, keys[1:])):
            raise ValueError('empty/unsorted/duplicate B-tree node keys')
        return result

    def find(self, key):
        if not self.depth:
            return None
        number, seen, lower, upper = self.root, set(), None, None
        for level in range(self.depth, 0, -1):
            if number in seen:
                raise ValueError('B-tree child cycle')
            seen.add(number)
            records = self.node(number, level)
            keys = [r['key'] for r in records]
            if ((lower is not None and keys[0] < lower) or
                    (upper is not None and keys[-1] >= upper)):
                raise ValueError('B-tree child keys escape parent interval')
            if level == 1:
                index = bisect.bisect_left(keys, key)
                return records[index] if index < len(keys) and keys[index] == key else None
            index = max(0, bisect.bisect_right(keys, key) - 1)
            lower = keys[index]
            if index + 1 < len(keys):
                upper = keys[index + 1]
            number, = be('I', records[index]['body'])
        raise ValueError('B-tree search did not reach a leaf')


class TargetHFS:
    """Detached HFSX reader; caller is responsible for transport/COW budgets.

    lookup returns None or a dictionary with CNID/type/mode/uid/gid/size/body.
    read_file follows bounded symlinks, never indirect hard-link stubs.
    patch_owners and verify_targets accept exactly TARGETS -> immutable bytes.
    """
    def __init__(self, dev):
        self.dev, self.read_bytes, self.nodes = dev, 0, 0
        vh = self._read(1024, 512, initial=True)
        if be('HH', vh) != (0x4858, 5):
            raise ValueError('requires an HFSX version-5 volume')
        self.block, self.total = be('II', vh, 40)
        if (self.block < 512 or self.block > (1 << 20) or
                self.block & (self.block - 1) or not self.total):
            raise ValueError('invalid HFSX allocation geometry')
        self.size = self.block * self.total
        if hasattr(dev, 'size') and self.size > dev.size:
            raise ValueError('HFSX exceeds detached device')
        self.vh = vh
        # Bootstrap only an inline-covered extents tree. Unknown self-overflow
        # cannot be satisfied by recursively assuming its own missing mapping.
        self.extents = _Tree(self, _Fork(self, vh[192:272], 3, bootstrap=True))
        self.catalog = _Tree(self, _Fork(self, vh[272:352], 4), catalog=True)

    def _read(self, offset, size, initial=False):
        if offset < 0 or size < 0 or size > MAX_FILE or (not initial and offset + size > self.size):
            raise ValueError('read exceeds HFSX range/bounded request')
        self.read_bytes += size
        if self.read_bytes > MAX_READ:
            raise ValueError('bounded targeted-HFS read budget exceeded')
        data = self.dev.read(offset, size)
        if not isinstance(data, bytes) or len(data) != size:
            raise ValueError('short/nonbytes targeted-HFS read')
        return data

    def _child(self, parent, name):
        encoded = name.encode('utf-16be', 'strict')
        if len(encoded) > 510:
            raise ValueError('catalog name exceeds HFSX key')
        record = self.catalog.find((parent, encoded))
        if record is None:
            return None
        body = record['body']
        kind, = be('H', body)
        if kind not in (1, 2) or len(body) != (88 if kind == 1 else 248):
            raise ValueError('unsupported catalog file/folder record')
        flags, = be('H', body, 2)
        cnid, = be('I', body, 8)
        uid, gid = be('II', body, 32)
        mode, = be('H', body, 42)
        if (not cnid or flags & 0x20 or
                (kind == 2 and body[48:56] in (b'hlnkhfs+', b'fdrpMACS'))):
            raise ValueError('indirect/link-chain catalog object unsupported')
        if kind == 1 and stat.S_IFMT(mode) not in (0, stat.S_IFDIR):
            raise ValueError('invalid directory catalog mode')
        entry = dict(record, type='dir' if kind == 1 else 'file', id=cnid,
                     parent=parent, name=name, flags=flags, mode=mode, uid=uid, gid=gid)
        if kind == 2:
            entry['data'] = _Fork(self, body[88:168], cnid)
            entry['size'] = entry['data'].size
        return entry

    def lookup(self, path, follow_symlinks=True):
        if (not isinstance(path, str) or not path.startswith('/') or
                len(path) > 4096 or '\0' in path):
            raise ValueError('requires an absolute catalog path')
        todo, resolved, links = path.split('/')[1:], [], 0
        parent, entry = 2, {'type': 'dir', 'id': 2}
        while todo:
            part = todo.pop(0)
            if part in ('', '.'):
                continue
            if part == '..':
                raise ValueError('catalog path traversal unsupported')
            entry = self._child(parent, part)
            if entry is None:
                return None
            if entry['type'] == 'file' and stat.S_ISLNK(entry['mode']) and (todo or follow_symlinks):
                links += 1
                if links > 16 or entry['size'] > 8192:
                    raise ValueError('catalog symlink exceeds bounds')
                target = entry['data'].read(0, entry['size']).decode('utf-8', 'strict')
                if not target or '\0' in target:
                    raise ValueError('invalid catalog symlink')
                normalized = [] if target.startswith('/') else list(resolved)
                for item in target.split('/'):
                    if item in ('', '.'):
                        continue
                    if item == '..':
                        if not normalized:
                            raise ValueError('symlink escapes volume root')
                        normalized.pop()
                    else:
                        normalized.append(item)
                todo = normalized + todo
                resolved, parent = [], 2
                entry = {'type': 'dir', 'id': 2}
                continue
            resolved.append(part)
            if todo and any(x not in ('', '.') for x in todo) and entry['type'] != 'dir':
                raise ValueError('non-directory catalog path ancestor')
            parent = entry['id']
        return dict(entry, path='/' + '/'.join(resolved))

    def read_file(self, path, max_bytes=MAX_FILE):
        entry = self.lookup(path)
        if (entry is None or entry['type'] != 'file' or not stat.S_ISREG(entry['mode']) or
                not 0 <= entry['size'] <= min(max_bytes, MAX_FILE)):
            raise ValueError('missing/nonregular/oversized catalog file')
        if entry['body'][41] & 0x20:
            raise ValueError('compressed file requires native HFS logical reads')
        return entry['data'].read(0, entry['size'])

    def _targets(self, files, owners):
        if not isinstance(files, Mapping) or set(files) != TARGETS:
            raise ValueError('owner verification requires the exact seven Bluetooth paths')
        if any(not isinstance(data, bytes) or not 0 < len(data) <= MAX_FILE for data in files.values()):
            raise ValueError('requires bounded immutable target payload bytes')
        if sum(map(len, files.values())) > 128 << 20:
            raise ValueError('target payload budget exceeded')
        result, cnids, ranges = [], set(), []
        for path, expected in files.items():
            entry = self.lookup(path, follow_symlinks=False)
            mode = stat.S_IFREG | (0o755 if path in BINARIES else 0o644)
            if (entry is None or entry['type'] != 'file' or entry['mode'] != mode or
                    entry['size'] != len(expected) or entry['id'] < 16 or entry['id'] in cnids):
                raise ValueError('target path/CNID/type/mode/data-fork size mismatch: ' + path)
            if entry['body'][41] & 0x20:  # BSD ownerFlags: raw fork is not logical contents.
                raise ValueError('compressed target owner correction unsupported')
            if owners and (entry['uid'], entry['gid']) != (0, 0):
                raise ValueError('target on-disk UID/GID mismatch: ' + path)
            for offset in range(0, len(expected), 1 << 20):
                size = min(1 << 20, len(expected) - offset)
                if entry['data'].read(offset, size) != expected[offset:offset + size]:
                    raise ValueError('target actual data-fork bytes mismatch: ' + path)
            cnids.add(entry['id'])
            spans = list(self.catalog.fork.ranges(entry['offset'] + 32, 8))
            for start, length in spans:
                if any(start < a + n and a < start + length for a, n in ranges):
                    raise ValueError('target owner fields alias each other')
                ranges.append((start, length))
            result.append((path, expected, entry))
        return result

    def patch_owners(self, files):
        """Preflight ALL seven files, then change only each UID/GID field."""
        checked = self._targets(files, owners=False)
        # Revalidate every whole selected record before the first COW write.
        for path, expected, entry in checked:
            if self.catalog.fork.read(entry['offset'], 248) != entry['body']:
                raise ValueError('target catalog record changed before owner correction')
        result = []
        for path, expected, entry in checked:
            if (entry['uid'], entry['gid']) != (0, 0):
                self.catalog.fork.write(entry['offset'] + 32, bytes(8))
            result.append({'path': path, 'cnid': entry['id'], 'mode': entry['mode'],
                           'size': len(expected), 'sha256': hashlib.sha256(expected).hexdigest(),
                           'old_uid': entry['uid'], 'old_gid': entry['gid'], 'uid': 0, 'gid': 0})
        self.catalog.cache.clear()  # Subsequent verification must observe COW owner changes.
        for path, expected, entry in checked:
            want = entry['body'][:32] + bytes(8) + entry['body'][40:]
            if self.catalog.fork.read(entry['offset'], 248) != want:
                raise ValueError('owner correction changed non-owner catalog bytes')
        return result

    def verify_targets(self, files):
        """Check actual seven data forks, exact modes and on-disk root owners."""
        self.catalog.cache.clear()
        return [{'path': path, 'cnid': entry['id'], 'mode': entry['mode'],
                 'size': len(expected), 'sha256': hashlib.sha256(expected).hexdigest(),
                 'uid': entry['uid'], 'gid': entry['gid']}
                for path, expected, entry in self._targets(files, owners=True)]
