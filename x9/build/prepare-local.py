#!/usr/bin/env python3
"""Connect migrated local dependencies to maintained repository sources.

Inputs are private local snapshots, not downloaded by this helper. Run after
copying them into .local/ as described in x9/BUILD.md. No device operations.
"""
import argparse
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOCAL = ROOT / '.local'
WORK = LOCAL / 'x9/work/x9-metal-render'


def connect(link, target):
    if not target.exists():
        raise FileNotFoundError(target)
    if link.is_symlink():
        link.unlink()
    elif link.exists():
        saved = link.with_name(link.name + '.before-migration')
        if saved.exists():
            raise RuntimeError(f'preserved copy already exists: {saved}')
        link.rename(saved)
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(os.path.relpath(target, link.parent), target_is_directory=target.is_dir())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true', help='Validate paths without changing files')
    a = ap.parse_args()
    required = [LOCAL / 'dxb/build/xnu.obj', LOCAL / 'dxb/xnu',
                LOCAL / 'x9/x9/out/pan220-batched-cache-ranges/root-manifest.json',
                WORK / 'mesa-source', WORK / 'mesa-render-source',
                WORK / 'metal2vulkan-pan205-source', WORK / 'libdrm-2.4.133']
    missing = [str(p.relative_to(ROOT)) for p in required if not p.exists()]
    if missing:
        ap.error('missing local inputs: ' + ', '.join(missing))
    links = [(LOCAL / 'x9/MaliMetal/X9/General', ROOT / 'x9/metal/General')]
    links += [(WORK / p.name, p) for p in (ROOT / 'x9/build/scripts').glob('*.py')
              if "'.local/x9/work/x9-metal-render'" in p.read_text()]
    # Scripts moved out of build/scripts (or deleted) leave dangling links.
    stale = [p for p in WORK.glob('*.py') if p.is_symlink() and not p.exists()
             and (ROOT / 'x9/build/scripts') in (p.parent / os.readlink(p)).resolve().parents]
    for p in stale:
        if a.check:
            print('note: dangling link to a moved script: ' + str(p.relative_to(ROOT)))
        else:
            p.unlink()
    for link, target in links:
        if link.is_symlink() and link.resolve() == target.resolve():
            continue
        if a.check:
            ap.error('missing/stale maintained-source link: ' + str(link.relative_to(ROOT)))
        connect(link, target)
    print('Local kernel, boot and userspace input paths verified; no device operations.')


if __name__ == '__main__':
    main()
