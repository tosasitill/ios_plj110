#!/usr/bin/env python3
"""Prepare the pinned general Metal runtime and toolchain; no tests/device access."""
from pathlib import Path
import subprocess,urllib.request,tarfile,json
# Machine paths: environment, then x9/build/local.json, then the defaults.
import json as _json, os as _os
from pathlib import Path as _Path
_local = _Path(__file__).resolve().parents[1] / 'local.json'
_cfg = _json.loads(_local.read_text()) if _local.is_file() else {}
DEV = _os.environ.get('DEVELOPER_DIR') or _cfg.get('developer_dir') or '/Applications/Xcode.app/Contents/Developer'
BREW = _os.environ.get('X9_HOMEBREW') or _cfg.get('homebrew') or '/opt/homebrew'
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
pin='720ef1ce9ae2a2068999a8675717418b9416283e'
source=w/'metal2vulkan-source'
archive=w/f'metal2vulkan-{pin}.tar.gz'
if not source.exists():
 if not archive.exists():urllib.request.urlretrieve(f'https://codeload.github.com/steelbrain/metal2vulkan/tar.gz/{pin}',archive)
 with tarfile.open(archive) as t:t.extractall(w,filter='data')
 (w/f'metal2vulkan-{pin}').rename(source)
print('Pinned general Metal runtime source ready',flush=True)
subprocess.run([BREW+'/bin/rustup','toolchain','install','1.98.0','--profile','minimal','--target','aarch64-apple-ios'],check=True)
print('Rust1.98/iOS cross target ready',flush=True)
(w/'general-runtime-inputs.json').write_text(json.dumps({'translator':{'repository':'https://github.com/steelbrain/metal2vulkan','commit':pin},'target':'aarch64-apple-ios,iOS16','scope':'General Metal object frontend/AIR-to-SPIRV runtime; intended native PanVK v14 backend','runtime_status':'NOT RUN'},indent=2)+'\n')
