#!/usr/bin/env python3
"""Package the just-linked native ICD for its Metal runtime path. No deployment."""
from pathlib import Path
import shutil,subprocess,plistlib,argparse
ap=argparse.ArgumentParser();ap.add_argument("--revision",type=int,default=170);opts=ap.parse_args()
if not 170<=opts.revision<=9999:ap.error("revision must be170..9999")
w=Path(__file__).resolve().parent
source=w/'build-panvk-native-ios/src/panfrost/vulkan/libvulkan_panfrost.dylib'
bundle=w/f'X9PanVK{opts.revision}.bundle';bundle.mkdir(exist_ok=True)
executable=bundle/'X9PanVK'
shutil.copyfile(source,executable)
subprocess.run(['/usr/bin/install_name_tool','-id',f'/System/Library/Extensions/X9PanVK{opts.revision}.bundle/X9PanVK',str(executable)],check=True)
(bundle/'Info.plist').write_bytes(plistlib.dumps({
 'CFBundleIdentifier':'org.heliodarwin.X9PanVK'+str(opts.revision),'CFBundleExecutable':'X9PanVK',
 'CFBundlePackageType':'BNDL','CFBundleVersion':'1','MinimumOSVersion':'16.0',
},fmt=plistlib.FMT_XML))
subprocess.run(['/Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/strip','-S',str(executable)],check=True)
subprocess.run(['/usr/bin/codesign','--force','--sign','-','--pagesize','16384',str(executable)],check=True)
print('Native PanVK bundle packaged and ad-hoc signed; not deployed, loaded or hardware-validated.',flush=True)
