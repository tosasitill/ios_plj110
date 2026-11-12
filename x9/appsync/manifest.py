#!/usr/bin/env python3
"""Generate kernel runtime hash metadata, never embed executable bytes."""
import argparse
import json
from pathlib import Path

PREFIX = '/private/var/x9/jailbreak/20H392'
LIBRARIES = ('UniXNUAppSync.dylib', 'libunixnu-ldid.dylib')


def generate(source, output):
    items = []
    if source:
        value = json.loads(source.read_text())
        if (value.get('abi') != 1 or value.get('build') != '20H392'
                or value.get('target') != 'arm64-apple-ios16.0' or value.get('prefix') != PREFIX):
            raise ValueError('AppSync runtime identity mismatch')
        for name in LIBRARIES:
            path = PREFIX + '/lib/' + name
            entry = value['files'][path]
            size, digest = entry['size'], entry['sha256']
            if type(size) is not int or not 1 <= size <= 64 * 1024 * 1024:
                raise ValueError('invalid runtime size')
            if not isinstance(digest, str) or len(digest) != 64 or len(bytes.fromhex(digest)) != 32:
                raise ValueError('invalid runtime digest')
            items.append((path, size, bytes.fromhex(digest)))
    text = '/* Generated hash metadata only; no Apple/runtime executable bytes. */\n'
    text += '#define X9_APPSYNC_RUNTIME_READY ' + str(int(bool(items))) + '\n'
    text += '#define X9_APPSYNC_LIBRARY "' + PREFIX + '/lib/UniXNUAppSync.dylib"\n'
    if items:
        text += 'static const struct x9_appsync_file x9_appsync_files[] = {\n'
        for path, size, digest in items:
            text += '{"' + path + '", ' + str(size) + ', {' + ','.join(f'0x{b:02x}' for b in digest) + '}},\n'
        text += '};\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    # Compiler depfiles remain stable for an unchanged manifest.
    if not output.is_file() or output.read_text() != text:
        output.write_text(text)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--runtime', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    generate(a.runtime, a.out)
