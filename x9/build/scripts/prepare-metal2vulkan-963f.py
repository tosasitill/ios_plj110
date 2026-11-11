#!/usr/bin/env python3
"""Fetch pristine metal2vulkan 963f0e64 for the X9 Metal bridge (no build, no execution of its code).

    python3 -I x9/build/scripts/prepare-metal2vulkan-963f.py [--dest DIR]

Clones https://github.com/steelbrain/metal2vulkan (blob filter) into DIR (default
.local/x9/work/x9-metal-render/metal2vulkan-963f-source) at 963f0e64f103d2e682cd0cc454a8fe50f52c8cbb (development,
2026-11-05). build-general-metal.py then applies x9/build/third_party/metal2vulkan-963f: AdrenoMetal ce63701's
translator patches (MIT, see adrenometal-ce63701/LICENSE; the series UniXNU 642c4fef pins) and
adrenometal-20be9f9's portable 0031..0034 updates, then 0100-x9-local-changes.patch (our pan194-pan205 Mali/PanVK changes, x9_timing, raw shadows, vertex capture, sample
replay, re-ported onto that series), and records them in DIR/.x9-patches-applied.
"""
import argparse
import subprocess
from pathlib import Path

PIN = '963f0e64f103d2e682cd0cc454a8fe50f52c8cbb'
ROOT = Path(__file__).resolve().parents[3]

ap = argparse.ArgumentParser()
ap.add_argument('--dest', type=Path, default=ROOT / '.local/x9/work/x9-metal-render/metal2vulkan-963f-source')
a = ap.parse_args()
if a.dest.exists():
    raise SystemExit(f'{a.dest} exists; move it away first')
subprocess.run(['git', 'clone', '-q', '--filter=blob:none', 'https://github.com/steelbrain/metal2vulkan.git', str(a.dest)],
               check=True)
subprocess.run(['git', '-C', str(a.dest), 'fetch', '-q', 'origin', 'development'], check=True)
subprocess.run(['git', '-C', str(a.dest), 'checkout', '-q', PIN], check=True)
print(f'{a.dest} at {PIN}; build-general-metal.py applies the x9 series')
