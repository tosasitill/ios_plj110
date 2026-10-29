#!/usr/bin/env python3
"""Build native PanVK v14 command/pipeline objects and shader generators.

Uses a separate pinned source export. Does not run a renderer or test suite.
"""
from pathlib import Path
import os,subprocess
w=Path(__file__).resolve().parent
src=w/'mesa-render-source'
# Native IOKit KMD will provide resource/queue/sync transport. Keep the public
# DRM declarations for Mesa's existing CSF queue records while porting it.
p=src/'meson.build';s=p.read_text()
anchor='# X9 PanVK transport declarations'
if anchor not in s:
 at=s.index('if dep_libdrm.found()')
 s=s[:at]+"dep_libdrm = declare_dependency(include_directories : include_directories('"+str(w/'libdrm-2.4.133')+"', '"+str(w/'libdrm-2.4.133/include/drm')+"')) "+anchor+'\n'+s[at:]
 p.write_text(s)
# The system libclc SPIR-V contains the same OpenCL builtin library contract;
# expose its actual file through the filenames expected by nir_load_libclc.
pc=w/'render-pkgconfig';pc.mkdir(exist_ok=True)
clc=w/'render-libclc';clc.mkdir(exist_ok=True)
for target,arch in [('spirv','spirv32'),('spirv64','spirv64')]:
 link=clc/(target+'-mesa3d-.spv')
 if not link.exists():link.symlink_to('/opt/homebrew/opt/libclc/share/clc/'+arch+'-unknown-unknown/libclc.spv')
(pc/'mesa-libclc.pc').write_text('prefix='+str(clc)+'\nlibexecdir=${prefix}\nName: mesa-libclc\nDescription: Exact system libclc SPIR-V build input\nVersion: 23.1.4\n')
env=os.environ.copy()
env['PATH']=str(w/'build-env/bin')+':'+str(w/'build-tools/bin')+':/opt/homebrew/opt/llvm/bin:/opt/homebrew/bin:/usr/bin:/bin'
env['LIBCLANG_PATH']='/opt/homebrew/opt/llvm/lib'
env['PKG_CONFIG_PATH']=str(pc)+':/opt/homebrew/opt/spirv-llvm-translator/lib/pkgconfig:/opt/homebrew/opt/spirv-tools/lib/pkgconfig:/opt/homebrew/lib/pkgconfig'
env['LLVM_CONFIG']='/opt/homebrew/opt/llvm/bin/llvm-config'
env['CC']='/usr/bin/clang';env['CXX']='/usr/bin/clang++';env['OBJC']='/usr/bin/clang'
env['RUSTC']=subprocess.check_output(['/opt/homebrew/bin/rustup','which','--toolchain','1.98.0','rustc'],text=True).strip()
args=[str(w/'build-env/bin/meson'),'setup',str(w/'build-panvk-host'),str(src),'--buildtype=release','-Dplatforms=[]','-Dgallium-drivers=[]','-Dvulkan-drivers=panfrost','-Dglx=disabled','-Degl=disabled','-Dgbm=disabled','-Dopengl=false','-Dgles1=disabled','-Dgles2=disabled','-Dllvm=enabled','-Dbuild-tests=false','-Dtools=[]','-Dvalgrind=disabled','-Dlibunwind=disabled','-Dzstd=disabled','-Dshader-cache=disabled','-Dpanfrost-rust=true','-Dpanfrost-disassemble=false','-Dvideo-codecs=[]','-Dvulkan-layers=[]','-Dxmlconfig=disabled']
if (w/'build-panvk-host/build.ninja').exists():args.insert(2,'--reconfigure')
subprocess.run(args,env=env,check=True)
subprocess.run(['/opt/homebrew/bin/ninja','-C',str(w/'build-panvk-host'),'-j6','src/panfrost/vulkan/libpanvk_v14.a'],env=env,check=True)
print('PanVK v14 pipeline/command objects built; native transport link and hardware execution remain separate',flush=True)
