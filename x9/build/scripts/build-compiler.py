#!/usr/bin/env python3
"""Build the pinned AIR frontend and Mesa NIR; no test execution."""
from pathlib import Path
import os, shutil, subprocess
# Machine paths: environment, then x9/build/local.json, then the defaults.
import json as _json, os as _os
from pathlib import Path as _Path
_local = _Path(__file__).resolve().parents[1] / 'local.json'
_cfg = _json.loads(_local.read_text()) if _local.is_file() else {}
DEV = _os.environ.get('DEVELOPER_DIR') or _cfg.get('developer_dir') or '/Applications/Xcode.app/Contents/Developer'
BREW = _os.environ.get('X9_HOMEBREW') or _cfg.get('homebrew') or '/opt/homebrew'
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
root=w.parents[1]
src=w/'mesa-source'
air=src/'src/compiler/air'
if not air.exists():shutil.copytree(w.parents[2]/'references/hackios-xnu-kernel/tools/mesa/air',air)
for f in air.glob('*.[ch]'):
 text=f.read_text().replace('gl_shader_stage','mesa_shader_stage')
 text=text.replace('bool was_exact = c->b.exact;','uint32_t was_fp_ctrl = c->b.fp_math_ctrl;')
 text=text.replace('c->b.exact = was_exact;','c->b.fp_math_ctrl = was_fp_ctrl;').replace('c->b.exact = false;','c->b.fp_math_ctrl = nir_fp_fast_math;')
 text=text.replace('c->b.exact = !(in->flags & (AIR_FMF_UNSAFE_ALGEBRA | AIR_FMF_REASSOC));','c->b.fp_math_ctrl = (in->flags & (AIR_FMF_UNSAFE_ALGEBRA | AIR_FMF_REASSOC)) ? nir_fp_fast_math : nir_fp_exact;')
 text=text.replace('nir_block_create(c->nir)','nir_block_create(c->b.impl)').replace('nir_shader_create(mem_ctx, stage, nir_options, NULL)','nir_shader_create(mem_ctx, stage, nir_options)')
 text=text.replace('nir_copy_prop','nir_opt_copy_prop')
 f.write_text(text)
p=src/'meson.build'
s=p.read_text()
if '# X9 standalone shader compiler' not in s:s=s.replace('if with_gfx_compute\n','with_gfx_compute = true # X9 standalone shader compiler\nif with_gfx_compute\n',1);p.write_text(s)
p=src/'src/compiler/meson.build'
s=p.read_text()
if "subdir('air')" not in s:p.write_text(s.replace("subdir('nir')","subdir('nir')\nsubdir('air')"))
env=os.environ.copy()
env['PATH']=str(w/'build-env/bin')+':'+BREW+'/bin:/usr/bin:/bin'
env['CC']='/usr/bin/clang';env['CXX']='/usr/bin/clang++'
args=[str(w/'build-env/bin/meson'),'setup',str(w/'build-host'),str(src),'--buildtype=release','-Dplatforms=[]','-Dgallium-drivers=[]','-Dvulkan-drivers=[]','-Dglx=disabled','-Degl=disabled','-Dgbm=disabled','-Dopengl=false','-Dgles1=disabled','-Dgles2=disabled','-Dllvm=disabled','-Dbuild-tests=false','-Dtools=[]','-Dvalgrind=disabled','-Dlibunwind=disabled','-Dzstd=disabled','-Dshader-cache=disabled']
if (w/'build-host/build.ninja').exists():args.insert(2,'--reconfigure')
subprocess.run(args,env=env,check=True)
subprocess.run([BREW+'/bin/ninja','-C',str(w/'build-host'),'src/compiler/air/air2nir'],env=env,check=True)
print('AIR/NIR host compiler build completed; no probes or tests executed',flush=True)
