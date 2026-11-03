#!/usr/bin/env python3
"""Rebuild frozen X9 native UFS objects and kernel; no post-build checks."""
from pathlib import Path
import json,os,subprocess
out=Path(__file__).resolve().parent
(out/'kernel-objects').mkdir(exist_ok=True)
env=os.environ.copy();env['DEVELOPER_DIR']='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer'
env['PATH']=env['DEVELOPER_DIR']+'/Toolchains/XcodeDefault.xctoolchain/usr/bin:/usr/bin:/bin:/opt/homebrew/bin'
for step in json.loads((out/'kernel-compile-commands.json').read_text()):subprocess.run(step['args'],cwd=step['cwd'],env=env,check=True)
commands=json.loads((out/'kernel-link-commands.json').read_text())
for name in ['link','strip']:subprocess.run(commands[name]['args'],cwd=commands[name]['cwd'],env=env,check=True)
print('Frozen native UFS kernel compiled, linked and stripped; not hardware-qualified.')
