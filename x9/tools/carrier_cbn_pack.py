#!/usr/bin/env python3
"""Embed an original, digest-qualified CBN_cn 54.1.0 IPCC, without changing its files."""
import argparse
import hashlib
import io
import json
import plistlib
import stat
import sys
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--src', type=Path)
    ap.add_argument('--expected-sha1', help='Digest from the captured Apple carrier index')
    ap.add_argument('--out', type=Path, default=ROOT / '.local/x9/carrier/cbn.pack')
    ap.add_argument('--metadata', type=Path, required=True)
    a = ap.parse_args()
    inc = ROOT / 'x9/kernel/generated/x9_carrier_cbn.inc'
    if a.src is None:
        if a.expected_sha1:
            sys.exit('carrier_cbn: expected Digest supplied without an IPCC')
        text = '/* Generated: no CBN IPCC selected. */\n#define X9_CBN_PRESENT 0\n'
        metadata = {'present': False}
    else:
        blob = a.src.read_bytes()
        digest = hashlib.sha1(blob).hexdigest()
        if not a.expected_sha1 or digest != a.expected_sha1.lower():
            sys.exit('carrier_cbn: source does not match the selected Apple index Digest')
        entries = []
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            names = set()
            for info in z.infolist():
                if info.is_dir():
                    continue
                p = PurePosixPath(info.filename)
                if (info.filename in names or len(p.parts) < 3 or p.parts[:2] != ('Payload', 'CBN_cn.bundle')
                        or any(c in ('', '.', '..') for c in p.parts) or p.is_absolute()
                        or stat.S_IFMT(info.external_attr >> 16) != stat.S_IFREG
                        or info.file_size > 1024 * 1024):
                    sys.exit('carrier_cbn: invalid/duplicate/non-regular archive entry')
                names.add(info.filename)
                entries.append(('/System/Library/Carrier Bundles/iPhone/' + '/'.join(p.parts[1:]), z.read(info)))
            info = plistlib.loads(z.read('Payload/CBN_cn.bundle/Info.plist'))
            carrier = plistlib.loads(z.read('Payload/CBN_cn.bundle/carrier.plist'))
            if (info.get('CFBundleIdentifier') != 'com.apple.CBN_cn' or info.get('CFBundleVersion') != '54.1.0'
                    or carrier.get('SupportedSIMs') != ['46015']):
                sys.exit('carrier_cbn: source identity/version/SIM selector differs from the qualified package')
        # Preserve the original IPCC too. Extracted DER/plists/signatures remain byte-identical.
        entries.insert(0, ('/private/var/x9/carrier/CBN_cn-54.1.0.ipcc', blob))
        packed = bytearray()
        rows = []
        for path, data in entries:
            if len(path.encode()) >= 256 or not data:
                sys.exit('carrier_cbn: invalid output path/empty file')
            offset = len(packed)
            packed.extend(data)
            sha = hashlib.sha256(data).digest()
            rows.append(' {' + json.dumps(path) + f', {offset}u, {len(data)}u, {{' +
                        ','.join(f'0x{b:02x}' for b in sha) + '}},')
        a.out = a.out.resolve()
        a.out.parent.mkdir(parents=True, exist_ok=True)
        if not a.out.is_file() or a.out.read_bytes() != packed:
            a.out.write_bytes(packed)
        sha = hashlib.sha256(packed).hexdigest()
        text = ('/* Generated private Apple carrier input; never commit. */\n'
                f'/* sha256 {sha}: invalidate ccache when input changes. */\n'
                '#define X9_CBN_PRESENT 1\n'
                f'#define X9_CBN_PACK_BYTES {len(packed)}u\n'
                'struct x9_cbn_file { const char *path; uint32_t offset, bytes; uint8_t sha[32]; };\n'
                'static const struct x9_cbn_file x9_cbn_files[] = {\n' + '\n'.join(rows) + '\n};\n'
                '__asm__(".section __TEXT,__const\\n.p2align 4\\n.globl _x9_cbn_pack\\n_x9_cbn_pack:\\n.incbin '
                '\\"' + str(a.out) + '\\"\\n.globl _x9_cbn_pack_end\\n_x9_cbn_pack_end:\\n.text\\n");\n'
                'extern const uint8_t x9_cbn_pack[], x9_cbn_pack_end[];\n')
        metadata = {'present': True, 'bundle': 'CBN_cn', 'version': '54.1.0', 'mccmnc': '46015',
                    'ipcc_sha1': digest, 'ipcc_bytes': len(blob), 'installed_files': len(entries),
                    'pack_bytes': len(packed), 'pack_sha256': sha, 'modified_apple_files': False}
    if not inc.is_file() or inc.read_text() != text:
        inc.write_text(text)
    a.metadata.write_text(json.dumps(metadata, indent=2) + '\n')
    print('carrier_cbn: ' + ('original 54.1.0, ' + str(metadata['installed_files']) +
                            ' files including IPCC, 46015 alias pending boot install' if a.src else 'not selected (stub)'))


if __name__ == '__main__':
    main()
