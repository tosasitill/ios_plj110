#!/usr/bin/env python3
"""Build the X9-owned backend with the existing configured PanVK toolchain."""
from pathlib import Path
import os
import subprocess
# Machine paths: environment, then x9/build/local.json, then the defaults.
import json as _json, os as _os
from pathlib import Path as _Path
_local = _Path(__file__).resolve().parents[1] / 'local.json'
_cfg = _json.loads(_local.read_text()) if _local.is_file() else {}
DEV = _os.environ.get('DEVELOPER_DIR') or _cfg.get('developer_dir') or '/Applications/Xcode.app/Contents/Developer'
BREW = _os.environ.get('X9_HOMEBREW') or _cfg.get('homebrew') or '/opt/homebrew'

w = Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
env = os.environ.copy()
env.pop('CC', None)
env.pop('CXX', None)
env.pop('RUSTC', None)
env['PATH'] = ':'.join(map(str, [w / 'build-env/bin', w / 'build-tools/bin',
    w / 'build-panvk-host/src/compiler/clc', w / 'build-panvk-host/src/compiler/spirv',
    w / 'build-panvk-host/src/panfrost/clc'])) + ':'+BREW+'/bin:/usr/bin:/bin'
env['DEVELOPER_DIR'] = DEV
env['IPHONEOS_DEPLOYMENT_TARGET'] = '16.0'
env['LIBCLANG_PATH'] = BREW+'/opt/llvm/lib'
env['BINDGEN_EXTRA_CLANG_ARGS'] = '--target=arm64-apple-ios16.0 -isysroot ' + env['DEVELOPER_DIR'] + '/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
# No source-preparation pass or Mesa source edits. The configured build already
# compiles MaliMetal/X9/General/panvk/X9PanVKMemory.c as an external source.
subprocess.run([BREW+'/bin/ninja', '-C', str(w / 'build-panvk-native-ios'),
    '-j6', 'src/panfrost/vulkan/libvulkan_panfrost.dylib'], env=env, check=True)
print('Native PanVK compiled/linked; no tests or device execution', flush=True)
