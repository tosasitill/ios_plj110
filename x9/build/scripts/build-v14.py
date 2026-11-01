#!/usr/bin/env python3
"""Build Mesa's v14 compiler without Linux DRM, WSI, Vulkan or test runners."""
from pathlib import Path
import os,subprocess
# Machine paths: environment, then x9/build/local.json, then the defaults.
import json as _json, os as _os
from pathlib import Path as _Path
_local = _Path(__file__).resolve().parents[1] / 'local.json'
_cfg = _json.loads(_local.read_text()) if _local.is_file() else {}
DEV = _os.environ.get('DEVELOPER_DIR') or _cfg.get('developer_dir') or '/Applications/Xcode.app/Contents/Developer'
BREW = _os.environ.get('X9_HOMEBREW') or _cfg.get('homebrew') or '/opt/homebrew'
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
src=w/'mesa-source'
p=src/'meson.build';s=p.read_text()
anchor='# X9 standalone v14 backend'
if anchor not in s:
 at=s.index('\n# needed in the loader',s.index('with_any_panfrost ='))
 s=s[:at]+'\nwith_any_panfrost = true '+anchor+'\n'+s[at:];p.write_text(s)
p=src/'src/panfrost/meson.build'
# Compiler source is unchanged. Restrict this private export's build graph.
p.write_text('''# X9 standalone build graph: no DRM/WSI or hardware tools.
inc_panfrost = include_directories(['.', 'shared', 'compiler', 'lib', 'libpan', 'model'])
compile_args_panfrost = ['-Wno-pointer-arith']
panfrost_rust_args = [rust_global_args, rust_2024_lint_args, '-Anon_snake_case', '-Aclippy::upper_case_acronyms']
subdir('shared')
subdir('model')
subdir('compiler')
''')
env=os.environ.copy();env['PATH']=str(w/'build-env/bin')+':'+str(w/'build-tools/bin')+':'+BREW+'/bin:/usr/bin:/bin'
env['LIBCLANG_PATH']=BREW+'/opt/llvm/lib'
env['CC']='/usr/bin/clang';env['CXX']='/usr/bin/clang++'
args=[str(w/'build-env/bin/meson'),'setup',str(w/'build-host'),str(src),'--reconfigure','--buildtype=release','-Dplatforms=[]','-Dgallium-drivers=[]','-Dvulkan-drivers=[]','-Dglx=disabled','-Degl=disabled','-Dgbm=disabled','-Dopengl=false','-Dgles1=disabled','-Dgles2=disabled','-Dllvm=disabled','-Dbuild-tests=false','-Dtools=[]','-Dvalgrind=disabled','-Dlibunwind=disabled','-Dzstd=disabled','-Dshader-cache=disabled','-Dpanfrost-rust=true','-Dpanfrost-disassemble=false']
subprocess.run(args,env=env,check=True)
subprocess.run([BREW+'/bin/ninja','-C',str(w/'build-host'),'src/panfrost/compiler/libpanfrost_compiler.a','src/panfrost/compiler/kraid/libkraid.a'],env=env,check=True)
print('Mesa v14 compiler libraries built; no GPU execution or tests',flush=True)
