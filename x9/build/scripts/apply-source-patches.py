#!/usr/bin/env python3
"""Apply maintained patches to a retained source snapshot before building it."""
import argparse
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', required=True, type=Path)
    ap.add_argument('patches', nargs='+', type=Path)
    args = ap.parse_args()
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
