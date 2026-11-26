#!/usr/bin/env python3
"""Generate a single-file Preferences diagnostic root header, never install it.

Input is the complete validated package; output contains ONLY the diagnostic.
An optional replacement accepts only the validated package diagnostic as its
predecessor. Install and rollback reject files outside these identities.
"""
import argparse
import hashlib
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import root_header

PATH = '/usr/lib/X9SettingsBTDiag.dylib'


def generate(payload, apple, out, replacement=None):
    entries, requires = root_header.load(payload.resolve(), apple.resolve())
    selected = [entry for entry in entries if entry[0] == PATH]
    if len(selected) != 1 or selected[0][2] != 0o755 or selected[0][4] is not None:
        raise ValueError('diagnostic must be a unique new 0755 root payload file')
    if out.exists() or out.is_symlink():
        raise ValueError('output already exists')
    out.parent.mkdir(parents=True, exist_ok=True)
    predecessor = []
    if replacement is not None:
        _, previous, mode, previous_digest, _ = selected[0]
        data = root_header.regular(replacement.parent, replacement.name, root_header.FILE_LIMIT)
        digest = hashlib.sha256(data).digest()
        if digest == previous_digest:
            raise ValueError('replacement equals the existing diagnostic')
        predecessor.append((len(previous), previous_digest))
        selected = [(PATH, data, mode, digest, None)]
    root_header.write(out, "#define X9_BT_ROOT_SETTINGS_ONLY 1\n" +
                      root_header.render(selected, requires, {PATH: predecessor}))
    print(f'embedded one diagnostic file ({len(selected[0][1])} bytes); no device operation: {out}')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--payload', type=Path, required=True)
    p.add_argument('--apple-originals', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--replacement-dylib', type=Path)
    args = p.parse_args()
    try:
        generate(args.payload, args.apple_originals, args.out, args.replacement_dylib)
    except (OSError, ValueError, KeyError) as e:
        sys.exit(f'settings_header failed: {e}')
