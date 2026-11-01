#!/usr/bin/env python3
"""Run an archived Python tool against the local migrated workspace.

Historical installation/recovery tools may WRITE device storage. Inspect the
script and obtain approval for the specific device operation before execution.
"""
import argparse
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('script', help='Path relative to x9/legacy/work')
ap.add_argument('args', nargs=argparse.REMAINDER)
a = ap.parse_args()
base = Path(__file__).resolve().parent / 'work'
script = (base / a.script).resolve()
if not script.is_relative_to(base) or script.suffix != '.py':
    ap.error('select an archived Python script within work/')
source = script.read_text()
if 'X9_SERIAL_REQUIRED' in source:
    serial = os.environ.get('X9_SERIAL')
    if not serial or not serial.isalnum():
        ap.error('set X9_SERIAL to the intended device serial')
    source = source.replace('X9_SERIAL_REQUIRED', serial)
for old, new in [
    ('/Users/tosasitill/HelioDarwin-x9-lk-preserve', ROOT / '.local/x9'),
    ('/Users/tosasitill/HelioDarwin/xnu/build-dxb/dxb', ROOT / '.local/dxb'),
    ('/Users/tosasitill/HelioDarwin', ROOT / '.local/heliodarwin'),
]:
    source = source.replace(old, str(new))
local_script = ROOT / '.local/x9/work' / script.relative_to(base)
if not local_script.parent.is_dir():
    ap.error('missing migrated local data directory: ' + str(local_script.parent))
sys.argv = [str(local_script), *a.args]
sys.path.insert(0, str(local_script.parent))
exec(compile(source, str(script), 'exec'), {'__name__': '__main__', '__file__': str(local_script)})
