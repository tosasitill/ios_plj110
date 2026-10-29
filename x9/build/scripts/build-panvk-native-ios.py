#!/usr/bin/env python3
"""Cross-build PanVK v14 render objects for iOS16, reusing host shader tools."""
from pathlib import Path
import os,subprocess
w=Path(__file__).resolve().parent
sdk='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
toolchain='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/'
rustc=subprocess.check_output(['/opt/homebrew/bin/rustup','which','--toolchain','1.98.0','rustc'],text=True).strip()
flags=['-target','arm64-apple-ios16.0','-isysroot',sdk,'-DX9_NATIVE_KMOD=1','-I'+str(w.parents[1]/'MaliMetal/X9/General/panvk')]
cross=w/'panvk-native-ios.cross'
cross.write_text("[binaries]\nc = "+repr(toolchain+'clang')+"\ncpp = "+repr(toolchain+'clang++')+"\nobjc = "+repr(toolchain+'clang')+"\nrust = "+repr(rustc)+"\nar = '/usr/bin/ar'\nstrip = '/usr/bin/strip'\npkg-config = '/usr/bin/false'\n[host_machine]\nsystem = 'darwin'\ncpu_family = 'aarch64'\ncpu = 'arm64'\nendian = 'little'\n[properties]\nneeds_exe_wrapper = true\n[built-in options]\n"+''.join(k+' = '+repr(flags)+'\n' for k in ['c_args','cpp_args','objc_args','c_link_args','cpp_link_args','objc_link_args'])+"rust_args = ['--target', 'aarch64-apple-ios']\n")
native=w/'panvk-native-ios.native'
native.write_text("[binaries]\nc = '/usr/bin/clang'\ncpp = '/usr/bin/clang++'\nrust = "+repr(rustc)+"\n")
env=os.environ.copy();env.pop('RUSTC',None);env.pop('CC',None);env.pop('CXX',None)
host=w/'build-panvk-host'
env['PATH']=str(w/'build-env/bin')+':'+str(w/'build-tools/bin')+':'+str(host/'src/compiler/clc')+':'+str(host/'src/compiler/spirv')+':'+str(host/'src/panfrost/clc')+':/opt/homebrew/bin:/usr/bin:/bin'
env['LIBCLANG_PATH']='/opt/homebrew/opt/llvm/lib'
env['DEVELOPER_DIR']='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer'
env['IPHONEOS_DEPLOYMENT_TARGET']='16.0'
env['BINDGEN_EXTRA_CLANG_ARGS']='--target=arm64-apple-ios16.0 -isysroot '+sdk
args=[str(w/'build-env/bin/meson'),'setup',str(w/'build-panvk-native-ios'),str(w/'mesa-render-source'),'--cross-file',str(cross),'--native-file',str(native),'--buildtype=release','-Dplatforms=[]','-Dgallium-drivers=[]','-Dvulkan-drivers=panfrost','-Dglx=disabled','-Degl=disabled','-Dgbm=disabled','-Dopengl=false','-Dgles1=disabled','-Dgles2=disabled','-Dllvm=disabled','-Dmesa-clc=system','-Dprecomp-compiler=system','-Dbuild-tests=false','-Dtools=[]','-Dvalgrind=disabled','-Dlibunwind=disabled','-Dzstd=disabled','-Dshader-cache=disabled','-Dpanfrost-rust=true','-Dpanfrost-disassemble=false','-Dvideo-codecs=[]','-Dvulkan-layers=[]','-Dx9-native-kmod=true','-Dxmlconfig=disabled','-Dlmsensors=disabled','-Dexpat=disabled']
if (w/'build-panvk-native-ios/build.ninja').exists():args.insert(2,'--reconfigure')
subprocess.run(args,env=env,check=True)
subprocess.run(['/opt/homebrew/bin/ninja','-C',str(w/'build-panvk-native-ios'),'-j6','src/panfrost/vulkan/libvulkan_panfrost.dylib'],env=env,check=True)
print('Native PanVK dynamic library linked; no device execution or rendering validation',flush=True)
