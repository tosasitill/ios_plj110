#!/usr/bin/env python3
"""Read-only copy of iOS unified-log stores out of the X9 HFSX root image.

The root is a pinned+immutable file in Android userdata whose blocks iOS
writes raw, so every read goes through the file's own `pinfile rawreadat`
(O_DIRECT on /dev/block/sdc100) exactly like check_plaintext_root.py. Nothing
is written to the device. Run check_plaintext_root.py --manifest first.

HFS+ support: volume header, catalog and extents-overflow B-trees, hard links,
and an in-memory replay of a dirty journal (iOS was usually not shut down
cleanly before the phone is rebooted into Android). The device image is never
modified; the replay only overlays reads.

  pull_ios_logs.py --manifest M --list            # volume/journal state + sizes
  pull_ios_logs.py --manifest M --out DIR         # copy diagnostics + uuidtext
"""
import argparse, bisect, json, shlex, struct, subprocess, sys
from pathlib import Path

DEVICE = '/dev/block/sdc100'
CHUNK = 8 << 20


class Raw:
    def __init__(self, manifest, serial=None):
        m = json.loads(Path(manifest).read_text())
        self.path, self.size = m['device_file'], m['bytes']
        self.tool = str(Path(self.path).parent / 'pinfile')
        # 2026-11-10: the HFS spans root||extension_policy file (32 GiB device).
        self.parts = [(0, m['bytes'], m['device_file'])]
        ext = m.get('extension_policy')
        if ext:
            self.parts.append((m['bytes'], ext['bytes'], ext['device_file']))
            self.size += ext['bytes']
        self.serial = serial
        self.reads = 0

    def _adb(self, cmd, timeout=120, expect=None):
        # A flaky USB link re-enumerates adb every few seconds (transport_id keeps
        # climbing); reads are idempotent, so wait for the device and retry.
        args = ['adb'] + (['-s', self.serial] if self.serial else []) + ['exec-out', 'su', '-c', cmd]
        for attempt in range(40):
            try:
                out = subprocess.run(args, check=True, capture_output=True, timeout=timeout).stdout
                if expect is None or len(out) == expect:
                    return out
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                if attempt == 39:
                    raise
            subprocess.run(['adb'] + (['-s', self.serial] if self.serial else []) + ['wait-for-device'],
                           capture_output=True, timeout=60)
            self.retries = getattr(self, 'retries', 0) + 1
        return out

    def read(self, off, n):
        if off < 0 or n < 0 or off + n > self.size:
            raise ValueError('read outside root image: %#x+%#x' % (off, n))
        out = bytearray()
        for base, length, path in self.parts:
            if n and base <= off < base + length:
                k = min(n, base + length - off)
                out += self._read_file(path, off - base, k)
                off += k; n -= k
        if n:
            raise ValueError('read not covered by root/extension: %#x+%#x' % (off, n))
        return bytes(out)

    def _read_file(self, path, off, n):
        out = bytearray()
        while n:
            # rawreadat wants 4 KiB alignment for O_DIRECT; widen and trim.
            a = off & ~4095
            take = min(CHUNK, ((off + n + 4095) & ~4095) - a)
            q = shlex.quote
            data = self._adb(q(self.tool) + ' rawreadat ' + q(path) + ' ' + DEVICE +
                             ' %d %d 2>/dev/null' % (a, take), expect=take)
            self.reads += 1
            if len(data) != take:
                raise IOError('short raw read at %#x: %d of %d' % (a, len(data), take))
            k = min(n, a + take - off)
            out += data[off - a:off - a + k]
            off += k; n -= k
        return bytes(out)


class Overlay:
    """Journal blocks replayed in memory on top of the raw reader."""
    def __init__(self, raw):
        self.raw, self.keys, self.blocks = raw, [], {}

    def put(self, off, data):
        if off not in self.blocks:
            bisect.insort(self.keys, off)
        self.blocks[off] = data

    def read(self, off, n):
        buf = bytearray(self.raw.read(off, n))
        i = bisect.bisect_right(self.keys, off) - 1
        i = max(i, 0)
        while i < len(self.keys) and self.keys[i] < off + n:
            k = self.keys[i]; d = self.blocks[k]
            lo, hi = max(off, k), min(off + n, k + len(d))
            if lo < hi:
                buf[lo - off:hi - off] = d[lo - k:hi - k]
            i += 1
        return bytes(buf)


def be(fmt, b, o=0):
    return struct.unpack_from('>' + fmt, b, o)


class HFS:
    def __init__(self, dev):
        self.dev = dev
        vh = dev.read(1024, 512)
        if vh[:2] not in (b'H+', b'HX'):
            raise ValueError('not an HFS+/HFSX volume')
        self.sig = vh[:2].decode()
        self.attributes, = be('I', vh, 4)
        self.jib, = be('I', vh, 12)
        self.block, self.total = be('II', vh, 40)
        self.vh = vh
        self.journal = None
        if self.attributes & (1 << 13) and self.jib:
            self.journal = self._replay()
            vh = self.dev.read(1024, 512)  # header itself may be journaled
            self.vh = vh
        self.extents_fork = self._fork(self.vh, 192)
        self.catalog_fork = self._fork(self.vh, 272)
        self.extents_tree = None
        self.extents_tree = self._tree(4, self.extents_fork)
        self.catalog = self._tree(4, self.catalog_fork, cnid=4)
        self._index()

    @staticmethod
    def _fork(b, o):
        size, clump, blocks = be('QII', b, o)
        ext = [be('II', b, o + 16 + 8 * i) for i in range(8)]
        return {'size': size, 'blocks': blocks, 'extents': [e for e in ext if e[1]]}

    # -- journal ---------------------------------------------------------
    def _replay(self):
        jib = self.dev.read(self.jib * self.block, 512)
        flags, = be('I', jib, 0)
        if not flags & 1:  # kJIJournalInFSMask
            return {'state': 'external journal, not replayed'}
        joff, jsize = be('QQ', jib, 36)
        hdr = self.dev.read(joff, 512)
        magic, endian = struct.unpack_from('<II', hdr, 0)
        e = '<' if (magic, endian) == (0x4a4e4c78, 0x12345678) else '>'
        magic, endian, start, end, size, blhdr_size, cksum, jhdr_size = struct.unpack_from(e + 'IIQQQiIi', hdr, 0)
        if magic != 0x4a4e4c78 or size != jsize:
            return {'state': 'journal header invalid (magic %#x size %d/%d)' % (magic, size, jsize)}
        info = {'state': 'clean' if start == end else 'dirty', 'start': start, 'end': end,
                'size': size, 'blhdr_size': blhdr_size, 'jhdr_size': jhdr_size, 'transactions': 0, 'blocks': 0}
        if start == end:
            return info
        ov = Overlay(self.dev.raw if isinstance(self.dev, Overlay) else self.dev)

        def jread(pos, n):
            out = b''
            while n:
                if pos >= size:
                    pos = jhdr_size
                k = min(n, size - pos)
                out += self.dev.read(joff + pos, k)
                pos += k; n -= k
            return out, (jhdr_size + (pos - size) if pos >= size else pos)

        pos, guard = start, 0
        while pos != end and guard < 100000:
            guard += 1
            bl, _ = jread(pos, blhdr_size)
            max_blocks, num_blocks, bytes_used = struct.unpack_from(e + 'HHi', bl, 0)
            if num_blocks < 1 or bytes_used < blhdr_size or bytes_used > size or 16 + 16 * num_blocks > blhdr_size:
                info['state'] += ', stopped at bad block list'
                break
            data_pos = pos + blhdr_size
            if data_pos >= size:
                data_pos = jhdr_size + (data_pos - size)
            for i in range(1, num_blocks):
                bnum, bsize = struct.unpack_from(e + 'QI', bl, 16 + 16 * i)
                block, data_pos = jread(data_pos, bsize)
                if bnum != 0xffffffffffffffff:
                    ov.put(bnum * jhdr_size, block)
                    info['blocks'] += 1
            info['transactions'] += 1
            pos = pos + bytes_used
            if pos >= size:
                pos = jhdr_size + (pos - size)
        self.dev = ov
        return info

    # -- B-trees ---------------------------------------------------------
    def fork_extents(self, fork, cnid=None, ftype=0):
        ext = list(fork['extents'])
        have = sum(c for _, c in ext)
        if have < fork['blocks'] and cnid is not None and self.extents_tree:
            # Key (length field stripped): forkType u8, pad u8, fileID u32,
            # startBlock u32. Leaf order is (fileID, startBlock), i.e. file order.
            for key, rec in self.extents_tree['leaf']:
                kt, kid = key[0], be('I', key, 2)[0]
                if kt == ftype and kid == cnid:
                    ext += [e for e in (be('II', rec, 8 * i) for i in range(8)) if e[1]]
        if sum(c for _, c in ext) < fork['blocks']:
            raise ValueError('missing extents for CNID %s' % cnid)
        return ext

    def read_fork(self, fork, cnid=None, ftype=0, limit=None):
        out, left = bytearray(), fork['size'] if limit is None else min(limit, fork['size'])
        for start, count in self.fork_extents(fork, cnid, ftype):
            if left <= 0:
                break
            n = min(left, count * self.block)
            out += self.dev.read(start * self.block, n)
            left -= n
        return bytes(out)

    def _tree(self, _unused, fork, cnid=3):
        data = self.read_fork(fork, cnid=cnid)
        node = be('H', data, 32)[0]
        first = be('I', data, 24)[0]
        leaf, seen, n = [], set(), first
        while n:
            if n in seen or (n + 1) * node > len(data):
                raise ValueError('B-tree leaf chain broken at node %d' % n)
            seen.add(n)
            b = data[n * node:(n + 1) * node]
            flink, = be('I', b, 0)
            kind = struct.unpack_from('b', b, 8)[0]
            count, = be('H', b, 10)
            if kind != -1:
                raise ValueError('node %d is not a leaf' % n)
            offs = [be('H', b, node - 2 * (i + 1))[0] for i in range(count + 1)]
            for i in range(count):
                o = offs[i]
                klen, = be('H', b, o)
                key = b[o + 2:o + 2 + klen]
                rec = b[o + 2 + klen + (klen & 1):offs[i + 1]]
                leaf.append((key, rec))
            n = flink
        return {'leaf': leaf}

    def _index(self):
        self.children, self.byid = {}, {}
        for key, rec in self.catalog['leaf']:
            parent, = be('I', key, 0)
            nlen, = be('H', key, 4)
            name = key[6:6 + 2 * nlen].decode('utf-16-be', 'replace')
            rtype, = be('h', rec, 0)
            if rtype == 1:
                cnid, = be('I', rec, 8)
                ent = {'type': 'dir', 'id': cnid, 'name': name, 'parent': parent}
            elif rtype == 2:
                cnid, = be('I', rec, 8)
                ent = {'type': 'file', 'id': cnid, 'name': name, 'parent': parent,
                       'mtime': be('I', rec, 16)[0], 'mode': be('H', rec, 42)[0],
                       'special': be('I', rec, 44)[0], 'ftype': rec[48:52], 'creator': rec[52:56],
                       'data': self._fork(rec, 88)}
            else:
                continue
            self.children.setdefault(parent, {})[name] = ent
            self.byid[cnid] = ent

    def lookup(self, path):
        cur = 2  # root folder CNID
        ent = self.byid.get(2, {'type': 'dir', 'id': 2})
        for part in [p for p in path.split('/') if p]:
            ent = self.children.get(cur, {}).get(part)
            if not ent:
                return None
            cur = ent['id']
        return ent

    def resolve(self, ent):
        """Follow an HFS+ file hard link to its iNode in the private directory."""
        if ent['type'] == 'file' and ent['ftype'] == b'hlnk' and ent['creator'] == b'hfs+':
            priv = self.children.get(2, {}).get('\x00\x00\x00\x00HFS+ Private Data')
            if priv:
                target = self.children.get(priv['id'], {}).get('iNode%d' % ent['special'])
                if target:
                    return target
        return ent

    def walk(self, ent, prefix=''):
        for name, child in sorted(self.children.get(ent['id'], {}).items()):
            rel = prefix + name
            if child['type'] == 'dir':
                yield rel + '/', child
                yield from self.walk(child, rel + '/')
            else:
                yield rel, self.resolve(child)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--serial')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--out', type=Path)
    ap.add_argument('--paths', nargs='*', default=['private/var/db/diagnostics', 'private/var/db/uuidtext'])
    ap.add_argument('--recent-hours', type=float,
                    help='copy only .tracev3 files modified within H hours of the newest one (other files always)')
    a = ap.parse_args()
    raw = Raw(a.manifest, a.serial)
    fs = HFS(raw)
    print(json.dumps({'signature': fs.sig, 'block': fs.block, 'total_blocks': fs.total,
                      'journal': fs.journal, 'catalog_bytes': fs.catalog_fork['size'],
                      'catalog_records': len(fs.catalog['leaf'])}, indent=1))
    for p in a.paths:
        ent = fs.lookup(p)
        if not ent:
            print('%s: not found' % p); continue
        if ent['type'] == 'file':  # a single file path copies just that file
            ent = fs.resolve(ent)
            files = [('', ent)]
        else:
            files = [(r, e) for r, e in fs.walk(ent) if e['type'] == 'file']
        if a.recent_hours is not None:
            logs = [e['mtime'] for r, e in files if r.endswith('.tracev3')]
            if logs:
                floor = max(logs) - a.recent_hours * 3600
                files = [(r, e) for r, e in files if not r.endswith('.tracev3') or e['mtime'] >= floor]
        total = sum(e['data']['size'] for _, e in files)
        print('%s: %d files, %.1f MiB' % (p, len(files), total / 2**20))
        if a.list:
            for r, e in files:
                if '/' not in r.rstrip('/') or r.count('/') <= 1:
                    print('  %10d %s' % (e['data']['size'], r))
            continue
        if a.out:
            base = a.out / Path(p).name
            for r, e in files:
                dst = base / r if r else base
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(fs.read_fork(e['data'], cnid=e['id']))
            print('  copied to %s (%d raw reads so far)' % (base, raw.reads))


if __name__ == '__main__':
    main()
