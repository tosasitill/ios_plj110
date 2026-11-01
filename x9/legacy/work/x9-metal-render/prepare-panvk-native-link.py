#!/usr/bin/env python3
"""Prepare a separate build that compiles all PanVK callers with native ABI.
Sources remain the pinned export plus retained port diffs. This never changes
candidate images or enables a renderer on a device.
"""
from pathlib import Path
import difflib,subprocess
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
root=w.parents[1]
subprocess.run(['python3',str(w/'prepare-panvk-native-physical.py')],check=True)
source=w/'mesa-render-source'
p=source/'src/panfrost/vulkan/meson.build';s=p.read_text();old=s
marker='# X9 native transport sources selected by the native build option.'
if marker not in s:
 native=root/'MaliMetal/X9/General'
 filenames=[native/'X9RenderTransport.c']+[native/'panvk'/n for n in ['X9PanVKMemory.c','X9PanVKSync.c','X9PanVKTilerHeap.c','X9PanVKSubmitPlan.c','X9PanVKQueue.c']]
 anchor="libvulkan_panfrost = shared_library("
 block=marker+"\nif get_option('x9-native-kmod')\n  libpanvk_files += files(\n"+''.join("    '"+str(f)+"',\n" for f in filenames)+"  )\nendif\n\n"
 if s.count(anchor)!=1:raise RuntimeError('shared library anchor changed')
 s=s.replace(anchor,block+anchor)
 (w/'panvk-native-link-sources.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/src/panfrost/vulkan/meson.build',tofile='b/src/panfrost/vulkan/meson.build')))
 p.write_text(s)
p=source/'meson_options.txt'
if not p.exists():p=source/'meson.options'
s=p.read_text()
if "option('x9-native-kmod'" not in s:p.write_text(s+"\noption('x9-native-kmod', type : 'boolean', value : false, description : 'Experimental X9 native IOKit renderer')\n")
# Reuse the proven SDK, compiler, dependencies and generated host tools. The
# separate build directory avoids mixing native queue layouts with DRM ones.
s=(w/'build-panvk-ios.py').read_text()
s=s.replace("w/'build-panvk-ios'","w/'build-panvk-native-ios'")
s=s.replace("w/'panvk-ios.cross'","w/'panvk-native-ios.cross'").replace("w/'panvk-ios.native'","w/'panvk-native-ios.native'")
s=s.replace("'-isysroot',sdk]","'-isysroot',sdk,'-DX9_NATIVE_KMOD=1','-I'+str(w.parents[1]/'MaliMetal/X9/General/panvk')]")
s=s.replace("'-Dxmlconfig=disabled'","'-Dx9-native-kmod=true','-Dxmlconfig=disabled'")
s=s.replace("src/panfrost/vulkan/libpanvk_v14.a","src/panfrost/vulkan/libvulkan_panfrost.dylib")
s=s.replace("iOS16/arm64 PanVK v14 pipeline/texture/render-command objects compiled; transport still requires native link","Native PanVK dynamic library linked; no device execution or rendering validation")
(w/'build-panvk-native-ios.py').write_text(s)
print('Separate native full-build recipe and native source linkage prepared',flush=True)
