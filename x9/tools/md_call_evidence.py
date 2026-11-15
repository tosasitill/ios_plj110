#!/usr/bin/env python3
"""Read-only recovery of call function boundaries from this modem's dbginfo.

No network, target execution, patched firmware, boot images or NV operations.
Private input and recovered bytes stay out of git. Only selected symbol
metadata is written to x9/out; a nanoMIPS disassembler is still needed to
inspect instructions. A symbol name/address alone is not call-policy proof.
"""
import argparse
import hashlib
import json
import lzma
import mmap
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
NAMES = {
    'mipc_call_dial_req', 'mipc_call_hangup_req', 'mipc_call_answer_get_req',
    'mipc_call_answer_get_cnf', 'mipc_call_get_call_status_req',
    'mipc_call_get_call_status_cnf', 'mipc_call_approve_incoming_cmd',
    'mipc_call_approve_incoming_rsp', 'mipc_call_status_ind', 'mipc_call_event_ind',
}


def recover(path):
    with path.open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        at = 0
        found = []
        while at + 512 <= len(m):
            magic, low = struct.unpack_from('<II', m, at)
            if magic != 0x58881688:
                break  # optional container trailer, not an arbitrary resync scan
            ext, header, version = struct.unpack_from('<III', m, at + 48)
            alignment, high = struct.unpack_from('<II', m, at + 68)
            size = low | (high << 32)
            if (ext != 0x58891689 or header != 512 or version != 1 or alignment != 16 or
                    size > len(m) - at - header):
                raise ValueError(f'unsupported/truncated MTK header at {at:#x}')
            name = m[at + 8:at + 40].split(b'\0', 1)[0]
            if name == b'md1_dbginfo':
                if size > 8 * 1024 * 1024:
                    raise ValueError('compressed dbginfo exceeds offline bound')
                dec = lzma.LZMADecompressor(memlimit=128 * 1024 * 1024)
                data = dec.decompress(m[at + header:at + header + size], max_length=16 * 1024 * 1024)
                if not dec.eof or dec.unused_data:
                    raise ValueError('dbginfo is incomplete/oversized or has an unknown trailer')
                found.append((at, data))
            at = (at + header + size + 15) & ~15
        if len(found) != 1:
            raise ValueError('expected exactly one md1_dbginfo section')
        image_hash = hashlib.sha256(m).hexdigest()
    offset, data = found[0]
    at = data.index(b'INT_Vectors\0')
    selected = []
    while at < len(data):
        end = data.find(b'\0', at, min(at + 1024, len(data)))
        if end < 0 or end + 9 > len(data):
            break
        raw = data[at:end]
        if not raw or any(c < 32 or c > 126 for c in raw):
            break
        start, stop = struct.unpack_from('<II', data, end + 1)
        if stop < start:
            break
        name = raw.decode('ascii')
        if name in NAMES:
            selected.append({'name': name, 'start': start, 'end': stop})
        at = end + 9
    if {x['name'] for x in selected} != NAMES or len(selected) != len(NAMES):
        raise ValueError('missing/ambiguous call symbols; do not reuse stale addresses')
    return {'image_sha256': image_hash, 'dbginfo_header': offset,
            'dbginfo_size': len(data), 'architecture': 'nanoMIPS', 'symbols': selected}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', type=Path, default=ROOT / '.local/x9/modem/parts/modem_a.img')
    ap.add_argument('--out', type=Path, default=ROOT / 'x9/out/call-ios-bridge/md-call-evidence.json')
    args = ap.parse_args()
    if not args.out.resolve().is_relative_to((ROOT / 'x9/out').resolve()):
        ap.error('derived output must stay in this worktree\'s gitignored x9/out')
    try:
        result = recover(args.src)
    except (OSError, ValueError, lzma.LZMAError) as e:
        ap.error(str(e))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    for x in result['symbols']:
        print(f"{x['name']} {x['start']:#x}..{x['end']:#x}")


if __name__ == '__main__':
    main()
