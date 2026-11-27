#!/usr/bin/env python3
"""Assemble a stageable storm payload from a bootstrap build directory.

Output (a rootfs overlay, same shape as the tree the root image carries):
  rootfs/private/var/x9/storm/payload/<version>/   bootstrap tree (immutable)
  rootfs/private/var/storm -> /private/var/mobile/Library/X9Storm/current
  payload-manifest.json                            version + per-file SHA256

Nothing here writes to a device or a root image; baking the overlay into an
image / install list is a separate, owner-authorized step.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil

TARGET_DIR = Path('private/var/mobile/Library/X9Storm/current')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--bootstrap', type=Path, required=True,
                    help='build.py output dir (contains bootstrap/ + manifest.json)')
    ap.add_argument('--version', required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    src = (a.bootstrap / 'bootstrap').resolve()
    rootfs = out / 'rootfs'
    payload_dir = rootfs / 'private/var/x9/storm/payload' / a.version
    shutil.copytree(src, payload_dir, symlinks=True)
    (payload_dir / 'VERSION').write_text(a.version + '\n')
    prep_src = Path(__file__).resolve().parent / 'prep.sh'
    shutil.copy2(prep_src, rootfs / 'private/var/x9/storm/prep.sh')
    link = rootfs / 'private/var/storm'
    os.symlink('/' + TARGET_DIR.as_posix(), link)
    files = {}
    for path in sorted(rootfs.rglob('*')):
        rel = '/' + path.relative_to(rootfs).as_posix()
        if path.is_symlink():
            files[rel] = {'symlink': os.readlink(path)}
        elif path.is_file():
            data = path.read_bytes()
            files[rel] = {'size': len(data),
                          'sha256': hashlib.sha256(data).hexdigest(),
                          'mode': oct(path.stat().st_mode & 0o777)}
    manifest = {'format': 'x9-storm-payload-1', 'version': a.version,
                'public_prefix': '/var/storm', 'files': files}
    (out / 'payload-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'payload {a.version}: {len(files)} entries -> {out}')


if __name__ == '__main__':
    main()
