#!/usr/bin/env python3
"""Retire old patches and apply maintained ones before building a source snapshot."""
import argparse
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', required=True, type=Path)
    ap.add_argument('--retire', action='append', default=[], type=Path,
                    help='Reverse this historical patch if present, or require its unpatched baseline')
    ap.add_argument('patches', nargs='*', type=Path)
    args = ap.parse_args()
    if not args.retire and not args.patches:
        ap.error('supply a patch to apply or --retire')
    # Retained snapshots may already include a retired patch. Removing it from
    # the apply list alone would silently keep the old behavior in such trees.
    # Both directions must match without fuzz; unrelated drift fails closed.
    for patch in args.retire:
        cmd = ['/usr/bin/patch', '--batch', '--fuzz=0', '-p1', '-i', str(patch.resolve())]
        reverse = subprocess.run(cmd + ['--dry-run', '--reverse'], cwd=args.source,
                                 capture_output=True, text=True)
        if reverse.returncode == 0:
            subprocess.run(cmd + ['--reverse'], cwd=args.source, check=True)
            print(f'Retired (reversed): {patch.name}', flush=True)
            continue
        forward = subprocess.run(cmd + ['--dry-run', '--forward'], cwd=args.source,
                                 capture_output=True, text=True)
        if forward.returncode != 0:
            raise SystemExit(f'Retired patch {patch.name} does not match this source:\n'
                             + reverse.stdout + reverse.stderr
                             + forward.stdout + forward.stderr)
        print(f'Already absent (retired): {patch.name}', flush=True)
    for patch in args.patches:
        cmd = ['/usr/bin/patch', '--batch', '--fuzz=0', '-p1', '-i', str(patch.resolve())]
        forward = subprocess.run(cmd + ['--dry-run', '--forward'], cwd=args.source,
                                 capture_output=True, text=True)
        if forward.returncode == 0:
            subprocess.run(cmd + ['--forward'], cwd=args.source, check=True)
            print(f'Applied {patch.name}', flush=True)
            continue
        reverse = subprocess.run(cmd + ['--dry-run', '--reverse'], cwd=args.source,
                                 capture_output=True, text=True)
        if reverse.returncode != 0:
            raise SystemExit(f'{patch.name} does not match this source:\n'
                             + forward.stdout + forward.stderr)
        print(f'Already applied: {patch.name}', flush=True)


if __name__ == '__main__':
    main()
