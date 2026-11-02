#!/usr/bin/env python3
"""Build the X9-owned backend with the existing configured PanVK toolchain."""
from pathlib import Path
import os
import subprocess
import sys
# Machine paths: environment, then x9/build/local.json, then the defaults.
import json as _json, os as _os
from pathlib import Path as _Path
_local = _Path(__file__).resolve().parents[1] / 'local.json'
_cfg = _json.loads(_local.read_text()) if _local.is_file() else {}
DEV = _os.environ.get('DEVELOPER_DIR') or _cfg.get('developer_dir') or '/Applications/Xcode.app/Contents/Developer'
BREW = _os.environ.get('X9_HOMEBREW') or _cfg.get('homebrew') or '/opt/homebrew'

w = Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
subprocess.run([sys.executable, '-I', str(Path(__file__).with_name('apply-source-patches.py')),
    '--source', str(w / 'mesa-render-source'),
    str(Path(__file__).resolve().parents[1] / 'third_party/02-panvk-host-linear-images.patch'),
    str(Path(__file__).resolve().parents[1] / 'third_party/03-kraid-hsr-metadata.patch'),
    str(Path(__file__).resolve().parents[1] / 'third_party/04-panvk-hsr-cull-all-rts.patch'),
    str(Path(__file__).resolve().parents[1] / 'third_party/05-panvk-index-fetch-audit.patch'),
    str(Path(__file__).resolve().parents[1] / 'third_party/06-panvk-widen-index-range.patch'),
    str(Path(__file__).resolve().parents[1] / 'third_party/07-panvk-fpk-write-masks.patch'),
    str(Path(__file__).resolve().parents[1] / 'third_party/08-panvk-serialize-passes.patch'),
    str(Path(__file__).resolve().parents[1] / 'third_party/09-panvk-v14-cull-zero-area.patch'),
    str(Path(__file__).resolve().parents[1] / 'third_party/10-pan-compiler-bir-ab.patch'),
    str(Path(__file__).resolve().parents[1] / 'third_party/11-panvk-fbd-log.patch')], check=True)
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
# The configured build also compiles the X9-owned KMD as an external source.
subprocess.run([BREW+'/bin/ninja', '-C', str(w / 'build-panvk-native-ios'),
    '-j6', 'src/panfrost/vulkan/libvulkan_panfrost.dylib'], env=env, check=True)
print('Native PanVK compiled/linked; no tests or device execution', flush=True)
