#!/usr/bin/env python3
"""Cross-compile the generic Metal object/runtime bridge; never execute it."""
from pathlib import Path
import os,json,shutil,subprocess,plistlib,argparse
ap=argparse.ArgumentParser();ap.add_argument("--revision",type=int,default=170);opts=ap.parse_args()
if not 170<=opts.revision<=9999:ap.error("revision must be170..9999")
w=Path(__file__).resolve().parent
root=w.parents[1]
source=w/'metal2vulkan-source'
bridge=root/'MaliMetal/X9/General'
bridge.mkdir(parents=True,exist_ok=True)
reference=root/'work/legacy-import/xnu_x9/drivers/NativeMetal'
for name in ['NativeBridgeMetalFrontend.mm','NativeBridgeMetalFrontend.h']:
 target=bridge/name
 if not target.exists():shutil.copyfile(reference/name,target)
(bridge/'bridge').mkdir(exist_ok=True)
for name in ['NativeMetalBridge.rs','NativeMetalBridge.h','NativeMetalObjects.rs','NativeMetalObjects.h']:
 target=bridge/'bridge'/name
 if not target.exists():shutil.copyfile(reference/'bridge'/name,target)
# All bridge references to Turnip/Adreno are documentation. This instance uses
# a caller-supplied Vulkan loader; no Qualcomm MMIO or KMD is imported.
p=bridge/'NativeBridgeMetalFrontend.mm'
s=p.read_text().replace('the metal2vulkan runtime on Turnip, on the Adreno 740 through\n// NativeAdreno.','the pinned metal2vulkan runtime over the native X9 PanVK loader.\n// MT6993 hardware access belongs to the X9 KMD, not this API layer.')
p.write_text(s)
generated=w/'general-build-inputs';generated.mkdir(exist_ok=True)
llvm_as='/opt/homebrew/opt/llvm/bin/llvm-as'
fixtures=[('NMB_FIXTURE_AIR','runtime_examples.air','crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll'),('NMB_FC_AIR','function_constant.air','crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll')]
env=os.environ.copy()
for variable,name,path in fixtures:
 subprocess.run([llvm_as,str(source/path),'-o',str(generated/name)],check=True)
 env[variable]=str(generated/name)
manifest=generated/'Cargo.toml'
manifest.write_text('\n'.join(['[package]','name="x9-general-metal-bridge"','version="0.1.0"','edition="2024"','rust-version="1.98"','[workspace]','[lib]','crate-type=["staticlib"]','path='+json.dumps(str(bridge/'bridge/NativeMetalBridge.rs')),'[dependencies]','metal2vulkan={path='+json.dumps(str(source/'crates/metal2vulkan'))+',features=["ash"]}','metal2vulkan-runtime={path='+json.dumps(str(source/'crates/metal2vulkan-runtime'))+'}','libc="0.2"','[profile.release]','panic="unwind"','debug=1','incremental=true','']))
env['RUSTC']=subprocess.check_output(['/opt/homebrew/bin/rustup','which','--toolchain','1.98.0','rustc'],text=True).strip()
env['RUSTDOC']=subprocess.check_output(['/opt/homebrew/bin/rustup','which','--toolchain','1.98.0','rustdoc'],text=True).strip()
env['IPHONEOS_DEPLOYMENT_TARGET']='16.0'
env['DEVELOPER_DIR']='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer'
env['SDKROOT']=env['DEVELOPER_DIR']+'/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
env['PATH']='/opt/homebrew/bin:'+env['DEVELOPER_DIR']+'/Toolchains/XcodeDefault.xctoolchain/usr/bin:/usr/bin:/bin'
subprocess.run(['/opt/homebrew/bin/rustup','run','1.98.0','cargo','build','--release','--manifest-path',str(manifest),'--target','aarch64-apple-ios','--target-dir',str(w/'general-target')],env=env,check=True)
cc=env['DEVELOPER_DIR']+'/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++'
subprocess.run([cc,'-target','arm64-apple-ios16.0','-isysroot',env['SDKROOT'],'-std=c++17','-fobjc-arc','-fblocks','-O2','-g','-Wno-protocol','-Wno-incomplete-implementation','-Wno-objc-protocol-property-synthesis','-Wno-deprecated-declarations','-c',str(bridge/'NativeBridgeMetalFrontend.mm'),'-o',str(generated/'X9GeneralMetalFrontend.o')],env=env,check=True)
transport=generated/'X9RenderTransport.o'
subprocess.run([env['DEVELOPER_DIR']+'/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang','-target','arm64-apple-ios16.0','-isysroot',env['SDKROOT'],'-std=c11','-O2','-Wall','-Wextra','-Werror','-c',str(bridge/'X9RenderTransport.c'),'-o',str(transport)],env=env,check=True)
factory=generated/'X9GeneralMetalFactory.o'
flags=[cc,'-DX9_PANVK_BUNDLE_PATH="/System/Library/Extensions/X9PanVK'+str(opts.revision)+'.bundle/X9PanVK"','-target','arm64-apple-ios16.0','-isysroot',env['SDKROOT'],'-std=c++17','-fobjc-arc','-fblocks','-O2','-g','-Wno-protocol','-Wno-incomplete-implementation','-Wno-objc-protocol-property-synthesis','-Wno-deprecated-declarations']
subprocess.run(flags+['-c',str(bridge/'X9GeneralMetalFactory.mm'),'-o',str(factory)],env=env,check=True)
bundle=w/f'X9GeneralMetal{opts.revision}.bundle';bundle.mkdir(exist_ok=True)
subprocess.run(flags+['-bundle',str(factory),str(transport),str(generated/'X9GeneralMetalFrontend.o'),str(w/'general-target/aarch64-apple-ios/release/libx9_general_metal_bridge.a'),'-framework','Foundation','-framework','Metal','-framework','IOSurface','-framework','IOKit','-framework','Security','-Wl,-dead_strip','-o',str(bundle/'X9GeneralMetal')],env=env,check=True)
(bundle/'Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier':'org.heliodarwin.X9GeneralMetal'+str(opts.revision),'CFBundleExecutable':'X9GeneralMetal','CFBundlePackageType':'BNDL','CFBundleVersion':'1','NSPrincipalClass':'X9GeneralMetalFactory','MinimumOSVersion':'16.0'},fmt=plistlib.FMT_XML))
for name in ['LICENSE','LICENSE-GPL-2.0','LICENSE-LGPL-3.0','LICENSE-GPL-3.0']:shutil.copyfile(source/name,bundle/name)
subprocess.run([env['DEVELOPER_DIR']+'/Toolchains/XcodeDefault.xctoolchain/usr/bin/strip','-S',str(bundle/'X9GeneralMetal')],check=True)
subprocess.run(['/usr/bin/codesign','--force','--sign','-','--pagesize','16384',str(bundle)],check=True)
print('Generic Metal runtime and iOS16 bundle compiled/linked/signed; no execution or deployment;requires native rendering KMD+PanVK loader',flush=True)
