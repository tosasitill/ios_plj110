#!/usr/bin/env python3
"""Normal rebuild of frozen pan169 kernel sources; no tests/device access."""
from pathlib import Path
import json,os,subprocess
out=Path(__file__).resolve().parent
env=os.environ.copy();env['DEVELOPER_DIR']='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer'
for record in json.loads((out/'commands.json').read_text()):
 subprocess.run(record['args'],cwd=record['cwd'],env=env,check=True)
